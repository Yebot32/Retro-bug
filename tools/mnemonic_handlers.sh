#!/usr/bin/env bash
# mnemonic_handlers.sh - map llvm-objdump mnemonic demand onto Dynarmic A32 decoder
# cases and classify each case's translator body (implemented / declared-but-refuses /
# interpret-fallback / no-translator-at-all).
#
# Usage (from repo root):
#   tools/mnemonic_handlers.sh [histogram-path] [report-path]
# Defaults:
#   histogram: build/isa-coverage/mnemonics.txt   (produced by tools/isa_coverage.sh)
#   report:    build/isa-coverage/handlers.txt
#
# Tooling constraints: bash + awk/gawk/sed/grep only (no python, no host compiler).
#
# The decoder-case universe is the INST(name, "mnemonic", "bits") rows of
# frontend/A32/decoder/*.inc.  The vendored disassembler only spells out arm_/vfp_/
# thumb16_ cases; thumb32_/asimd_/v8_ cases exist only in decoder + translator, so the
# .inc tables are the authoritative presence source and the disassembler is used only
# as a cross-check of front-end presence.  Translator bodies are extracted from
# translate/impl/*.cpp plus inline definitions in a32_translate_impl.h.

set -euo pipefail

HIST="${1:-build/isa-coverage/mnemonics.txt}"
REPORT="${2:-build/isa-coverage/handlers.txt}"

if [ ! -d external/dynarmic/src/dynarmic/frontend/A32 ]; then
    cd "$(dirname "$0")/.."
fi

D=external/dynarmic/src/dynarmic/frontend/A32
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ ! -f "$HIST" ]; then
    echo "ERROR: histogram file not found: $HIST (run tools/isa_coverage.sh first)" >&2
    exit 1
fi
for f in "$D"/decoder/arm.inc "$D"/decoder/thumb16.inc "$D"/decoder/thumb32.inc \
         "$D"/decoder/vfp.inc "$D"/decoder/asimd.inc \
         "$D"/translate/impl/a32_translate_impl.h; do
    [ -f "$f" ] || { echo "ERROR: missing $f" >&2; exit 1; }
done

# ---------------------------------------------------------------------------
# Step 1: decoder case table -> bases.tsv lines: name <TAB> group <TAB> base
# ---------------------------------------------------------------------------
gawk '
function addbase(k, base) { bases[k SUBSEP base]=1 }
match($0, /^INST\(([A-Za-z0-9_]+),[[:space:]]*"([^"]*)"/, m) {
    name=m[1]; mnem=m[2]
    group=name; sub(/_.*/, "", group)
    grp[name]=group
    head=mnem; sub(/ *\(.*$/, "", head)              # drop "(reg)"-style qualifiers
    # skip rows whose mnemonic is a dynarmic placeholder for reserved/undefined space
    # (e.g. INST(arm_UDF, "Undefined VSTM/VLDM", ...) must NOT register a "vldm" base)
    hw=head; hw=tolower(hw); if (hw ~ /^(undefined|unallocated|reserved|invalid)/) next
    n=split(head, parts, /\//)                       # alias pairs: STMIA/STMEA
    for (i=1; i<=n; i++) {
        t=parts[i]; gsub(/^[ \t]+|[ \t]+$/, "", t)
        split(t, ws, / +/)
        b=ws[1]; sub(/_.*$/, "", b)                   # VMVN_reg style suffixes -> vmvn
        gsub(/[^A-Za-z0-9{},:-]/, "", b); b=tolower(b)
        if (b=="") continue
        if (b ~ /\{[^}]*\}/) {                       # placeholder expansion (first: braces contain commas)
            stem=b; sub(/\{.*$/, "", stem)
            spec=b; sub(/^[^{]*\{/, "", spec); sub(/\}.*$/, "", spec)
            if (spec ~ /^[0-9]+-[0-9]+$/) {
                split(spec, rr, "-")
                for (k=rr[1]+0; k<=rr[2]+0; k++) addbase(name, stem k)
            } else {
                split(spec, el, /,/)
                for (k in el) { gsub(/ /, "", el[k]); addbase(name, stem tolower(el[k])) }
            }
        } else {
            m2=split(b, alts, /,/)                   # SMLABB, SMLTBT style aliases
            for (j=1; j<=m2; j++) {
                tok=alts[j]
                if (tok ~ /^[a-z][a-z0-9]*$/) {
                    addbase(name, tok)
                    # the "xy" family bases spell out the four concrete sign pairs:
                    # dynarmic "SMULxy" == objdump smulbb / smulbt / smultb / smultt
                    if (tok ~ /xy$/) {
                        stem2=tok; sub(/xy$/, "", stem2)
                        addbase(name, stem2 "bb"); addbase(name, stem2 "bt")
                        addbase(name, stem2 "tb"); addbase(name, stem2 "tt")
                    }
                    # the "wy" family has the two halfword variants: SMULWy == smulwb / smulwt
                    if (tok ~ /wy$/) {
                        stem2=tok; sub(/wy$/, "", stem2)
                        addbase(name, stem2 "wb"); addbase(name, stem2 "wt")
                    }
                }
            }
        }
    }
}
END { for (combo in bases) { split(combo, x, SUBSEP); print x[1] "\t" grp[x[1]] "\t" x[2] } }
' "$D"/decoder/arm.inc "$D"/decoder/thumb16.inc "$D"/decoder/thumb32.inc \
  "$D"/decoder/vfp.inc "$D"/decoder/asimd.inc | LC_ALL=C sort -u > "$TMP/bases.tsv"

# ---------------------------------------------------------------------------
# Step 2: translator definitions -> defs.tsv lines:
#         name <TAB> file <TAB> line <TAB> status <TAB> body(single line)
# Statuses: implemented | implemented-noop | interpret-fallback | refuses | unknown.
# 'refuses' = whole body is a single trap: return UndefinedInstruction()/
# UnpredictableInstruction()/DecodeError()/false; or UNREACHABLE().
# No #define-generated bodies exist in translate/impl/*.cpp (asserted below).
# ---------------------------------------------------------------------------
if grep -q '^#define' "$D"/translate/impl/*.cpp; then
    echo "WARNING: #define found in translate/impl - some bodies may be macro-generated" >&2
fi
gawk '
function flush_body(   b, st) {
    b=body
    sub(/^\{[ \t]*/, "", b)
    sub(/\}[[:space:]]*$/, "", b)
    gsub(/^[ \t]+|[ \t]+$/, "", b)
    if (b=="return false;" || b=="UNREACHABLE();" || b ~ /^return (UndefinedInstruction|UnpredictableInstruction|DecodeError)\(\);$/) st="refuses"
    else if (b=="return InterpretThisInstruction();") st="interpret-fallback"
    else if (b=="return true;") st="implemented-noop"
    else if (b ~ /return /) st="implemented"      # any real body ends in return true/false
    else st="unknown"
    gsub(/\t/, " ", b)
    print fname "\t" curfile "\t" fline "\t" st "\t" b
    state="none"; fname=""
}
FNR==1 { curfile=FILENAME }
BEGIN { state="none"; fname="" }
state=="none" && /^[[:space:]]*bool (TranslatorVisitor::)?(arm|thumb16|thumb32|vfp|asimd|v8)_[A-Za-z0-9_]+\(/ {
    nm=$0
    sub(/^[[:space:]]*bool (TranslatorVisitor::)?/, "", nm)
    sub(/\(.*/, "", nm)
    fname=nm; fline=FNR
    sig=$0; sub(/^[^{]*/, "", sig)                   # text from first "{" on (rest of line)
    if (index($0,"{")>0) {
        gsub(/\/\/.*/, "", sig)
        o=gsub(/\{/, "{", sig); c=gsub(/\}/, "}", sig)
        depth=o-c; body=sig; gsub(/^[ \t]+|[ \t]+$/, "", body)
        state=(depth<=0) ? "done" : "body"
        if (depth<=0) flush_body()
    } else { state="sig"; depth=0; body="" }
    next
}
state=="sig" {
    if (index($0,";")>0) { state="none"; fname=""; next }     # plain declaration
    if (index($0,"{")>0) {
        s=$0; sub(/^[^{]*/, "", s); gsub(/\/\/.*/, "", s)
        o=gsub(/\{/, "{", s); c=gsub(/\}/, "}", s)
        depth=o-c; body=s
        state="body"
    }
    next
}
state=="body" {
    line=$0; gsub(/\/\/.*/, "", line)
    o=gsub(/\{/, "{", line); c=gsub(/\}/, "}", line)
    depth+=o-c
    gsub(/^[ \t]+|[ \t]+$/, "", line)
    if (line!="") body=body (body==""?"":" ") line
    if (depth<=0) flush_body()
}
END { if (state=="body" && fname!="") { print fname "\t" curfile "\t" fline "\tunknown\tUNTERMINATED" } }
' "$D"/translate/impl/*.cpp "$D"/translate/impl/a32_translate_impl.h > "$TMP/defs.raw.tsv"

# Step 2b: resolve pure delegating bodies ("return arm_BX(cond, m);" /
# "return thumb16_UDF();") to the status of the case they forward to.
gawk -F'\t' '
function eff(n,   t, b, d) {
    for (d=0; d<6; d++) {
        b=body[n]
        if (b ~ /^return (arm|thumb16|thumb32|vfp|asimd|v8)_[A-Za-z0-9_]+\(.*\);$/) {
            t=b; sub(/^return /, "", t); sub(/\(.*/, "", t)
            if (!(t in body)) break
            n=t
        } else break
    }
    return cur[n]
}
{ nm[NR]=$1; fl[$1]=$2; li[$1]=$3; cur[$1]=$4; body[$1]=$5 }
END {
    for (i=1; i<=NR; i++) {
        n=nm[i]; s=cur[n]
        if (s=="implemented") {
            s2=eff(n)
            if (s2=="refuses" || s2=="interpret-fallback" || s2=="implemented-noop") { s=s2 }
        }
        print n "\t" fl[n] "\t" li[n] "\t" s "\t" body[n]
    }
}' "$TMP/defs.raw.tsv" > "$TMP/defs.tsv"

# ---------------------------------------------------------------------------
# Step 3: disassembler presence (cross-check)
# ---------------------------------------------------------------------------
grep -hoE 'std::string (arm|thumb16|vfp)_[A-Za-z0-9_]+\(' "$D"/disassembler/disassembler_arm.cpp \
     "$D"/disassembler/disassembler_thumb.cpp \
  | sed -E 's/std::string ([A-Za-z0-9_]+)\(/\1/' | LC_ALL=C sort -u > "$TMP/disasm.txt"

# ---------------------------------------------------------------------------
# Step 4: join histogram rows against decoder bases, classify buckets
# ---------------------------------------------------------------------------
gawk -v BASES="$TMP/bases.tsv" -v DEFS="$TMP/defs.tsv" -v DIS="$TMP/disasm.txt" -v ROWS="$TMP/rows.tsv" -v UNM="$TMP/unmatched.tsv" '
function rank(st) { return st=="implemented"?4 : st=="implemented-noop"?4 : st=="interpret-fallback"?3 : st=="refuses"?2 : st=="unknown"?1 : st=="notrans"?0 : -1 }
function st_of(nm) { return (nm in defname) ? dstatus[nm] : "notrans" }
function enqueue(c, fl,   old) {
    if (c in flagset) {
        old=flagset[c]
        if (index("," old ",", "," fl ",")>0) return
        flagset[c]=old "," fl
        qb[++qt]=c; qf[qt]=fl
    } else {
        flagset[c]=fl
        qb[++qt]=c; qf[qt]=fl
    }
}
BEGIN {
    FS="\t"
    while ((getline l < BASES) > 0)  { split(l, f, "\t"); case_of[f[3] SUBSEP f[1]]=1; baseexists[f[3]]=1; group[f[1]]=f[2]; casedec[f[1]]=1 }
    while ((getline l < DEFS) > 0)   { split(l, f, "\t"); defname[f[1]]=1; dstatus[f[1]]=f[4]; dfile[f[1]]=f[2]; dline[f[1]]=f[3]; dbody[f[1]]=f[5] }
    while ((getline l < DIS) > 0)    { dis[l]=1 }
    split("eq ne cs hs cc lo mi pl vs vc hi ls ge lt gt le al", cc, " ")
    for (i in cc) cond[cc[i]]=1
}
{
    if ($0 ~ /^[ \t]*$/) next
    _l=$0; sub(/^[ \t]+/, "", _l)
    nf=split(_l, a, /[ \t]+/)
    if (nf < 2 || a[1] !~ /^[0-9]+$/) { bad++; next }
    cnt=a[1]+0; mn=tolower(a[2])
    total++; totw+=cnt

    # --- normalisation closure; each flag is one rule (counts audited in report)
    delete qb; delete qf; delete processed; delete flagset
    flagset[mn]="-"
    qt=1; qb[1]=mn; qf[1]="-"
    while (1) {
        done=1
        for (qi=1; qi<=qt; qi++) {
            c=qb[qi]; fl=qf[qi]
            if (processed[c SUBSEP fl]++) continue
            processed[c SUBSEP fl]=1
            if (c ~ /\.w$/) { enqueue(substr(c, 1, length(c)-2), fl "W"); done=0 }
            else if (c ~ /\./) { d=c; sub(/\.[^.]*$/, "", d); enqueue(d, fl "D"); done=0 }
            if (length(c) > 1) {
                if (substr(c, length(c)) == "s") { enqueue(substr(c, 1, length(c)-1), fl "S"); done=0 }
                for (k in cond) if (length(c) > length(k) && substr(c, length(c)-length(k)+1) == k) {
                    enqueue(substr(c, 1, length(c)-length(k)), fl "C"); done=0
                }
            }
            if (c ~ /^it[te]+$/) { enqueue("it", fl "I"); done=0 }
            # objdump assembles "ADD/SUB (sp plus imm)" under the pseudo-mnemonics
            # addw/subw (observed: "addw r5, r0, #0x564", "subw sp, sp, #0x414", "addweq")
            if (c ~ /^addw/) { enqueue("add" substr(c, 5), fl "A"); done=0 }
            if (c ~ /^subw/) { enqueue("sub" substr(c, 5), fl "A"); done=0 }
            # VFP multi-register load/store: llvm spells the full addressing-mode name
            # (observed "vldmia", "vstmia", "vstmiami") where the dynarmic INST base is VLDM/VSTM
            if (c ~ /^v(ld|st)m..$/ && substr(c, length(c)-1) ~ /^(ia|ib|da|db|ea|ed|fa|fd|md|ms|sd)$/) {
                enqueue(substr(c, 1, length(c)-2), fl "M"); done=0
            }
            # llvm widening-move alias: observed "vmovl.u32 q10, d19" == vshll.u32 q10, d19, #0
            if (c == "vmovl") { enqueue("vshll", fl "L"); done=0 }
        }
        if (done) break
    }

    prehit=(mn in baseexists) ? 1 : 0
    # --- collect cases matching any candidate
    ncase=0; hasw32=0
    delete chosen
    for (c in flagset) {
        for (combo in case_of) {
            split(combo, x, SUBSEP)
            if (x[1]==c) {
                if (!(x[2] in chosen)) { chosen[x[2]]=flagset[c]; ncase++ }
                if (group[x[2]]=="thumb32" && flagset[c] ~ /W/) hasw32=1
            }
        }
    }
    if (mn ~ /\.w$/ && hasw32) {                     # ".w" pins the Thumb-32 group
        for (nm in chosen) if (group[nm]!="thumb32") { delete chosen[nm]; ncase-- }
    }
    matched=(ncase>0)
    if (prehit) { pre++; prew+=cnt }
    if (matched) { post++; postw+=cnt }

    # --- bucket (best status among matched cases; caveat stated in report)
    best=""; bestrank=-1
    for (nm in chosen) { r=rank(st_of(nm)); if (r>bestrank) { bestrank=r; best=st_of(nm) } }
    if (!matched) best="unmatched"
    else if (best=="implemented" || best=="implemented-noop") best="implemented"
    else if (best=="refuses") best="declared-but-refuses"
    else if (best=="notrans") best="no-translator-at-all"
    else if (best=="interpret-fallback") best="interpret-fallback"
    else best="unknown-status"
    brcnt[best]++; brw[best]+=cnt
    if (matched) {
        hr=0; hi=0
        for (nm in chosen) { s=st_of(nm)
            if (s=="refuses") hr=1
            if (s=="interpret-fallback") hi=1
        }
        if (hr && best=="implemented") { mixRn++; mixRw+=cnt; mixRstr=mixRstr " " mn }
        if (hi && best=="implemented") { mixIn++; mixIw+=cnt; mixIstr=mixIstr " " mn }
    }

    # --- rule attribution over the candidates that actually matched cases
    for (nm in chosen) {
        fl=chosen[nm]
        if (fl ~ /W/) { if (!(mn in ruleWseen)) { ruleWseen[mn]=1; ruleW+=cnt; ruleWn++ } }
        if (fl ~ /D/) { if (!(mn in ruleDseen)) { ruleDseen[mn]=1; ruleD+=cnt; ruleDn++ } }
        if (fl ~ /C/) { if (!(mn in ruleCseen)) { ruleCseen[mn]=1; ruleC+=cnt; ruleCn++ } }
        if (fl ~ /S/) { if (!(mn in ruleSseen)) { ruleSseen[mn]=1; ruleS+=cnt; ruleSn++ } }
        if (fl ~ /I/) { if (!(mn in ruleIseen)) { ruleIseen[mn]=1; ruleI+=cnt; ruleIn++ } }
        if (fl ~ /A/) { if (!(mn in ruleAseen)) { ruleAseen[mn]=1; ruleA+=cnt; ruleAn++ } }
        if (fl ~ /M/) { if (!(mn in ruleMseen)) { ruleMseen[mn]=1; ruleM+=cnt; ruleMn++ } }
        if (fl ~ /L/) { if (!(mn in ruleLseen)) { ruleLseen[mn]=1; ruleL+=cnt; ruleLn++ } }
    }

    # --- location of the deciding case
    loc="absent"
    want=(best=="implemented") ? "implemented" : (best=="declared-but-refuses") ? "refuses" : (best=="interpret-fallback") ? "interpret-fallback" : best
    for (nm in chosen) {
        st=st_of(nm)
        if (st==want || (want=="implemented" && st=="implemented-noop")) { loc=dfile[nm] ":" dline[nm]; break }
    }
    casesstr=""; gstr=""
    for (nm in chosen) {
        casesstr=casesstr (casesstr==""?"":";") nm
        if (index(gstr, "/" group[nm] "/")==0) gstr=gstr "/" group[nm] "/"
    }
    print cnt "\t" mn "\t" best "\t" loc "\t" ncase "\t" substr(gstr,2,length(gstr)-2) "\t" casesstr >> ROWS

    if (!matched) {
        why="no-decoder-case-spelled-like-this"
        if (mn ~ /^\./) why="objdump-pseudo/directive-token"
        else if (mn == "trap" || mn == "bad" || mn == "rrx") why="objdump-pseudo-or-operand-token(not-a-mnemonic)"
        else if (mn ~ /^[0-9a-f][0-9a-f]$/) why="objdump-.word-data-byte-token(not-a-mnemonic)"
        else if (mn ~ /^it[te]/) why="IT-block-spelling"
        else { b=mn; sub(/\.[^.]*$/, "", b); if (b!=mn) why="dotted-form-with-no-base-case:" b }
        print cnt "\t" mn "\t" why >> UNM
    }
    delete qb; delete qf; delete processed
}
END {
    printf "TOTAL\t%d\t%d\n", total, totw
    printf "PRE\t%d\t%d\n", pre, prew
    printf "POST\t%d\t%d\n", post, postw
    printf "BAD\t%d\n", bad+0
    for (b in brcnt) printf "BUCKET\t%s\t%d\t%d\n", b, brcnt[b], brw[b]
    printf "RULE\tW .w-suffix -> thumb32-wide\t%d\t%d\n", ruleWn+0, ruleW+0
    printf "RULE\tD .size/.type-suffix strip\t%d\t%d\n", ruleDn+0, ruleD+0
    printf "RULE\tC baked-in condition strip\t%d\t%d\n", ruleCn+0, ruleC+0
    printf "RULE\tS baked-in S-bit suffix strip\t%d\t%d\n", ruleSn+0, ruleS+0
    printf "RULE\tI it/itt/ite... -> IT case\t%d\t%d\n", ruleIn+0, ruleI+0
    printf "RULE\tA addw/subw -> add/sub (sp-plus-imm alias)\t%d\t%d\n", ruleAn+0, ruleA+0
    printf "RULE\tM vldm?/vstm? addressing-suffix strip\t%d\t%d\n", ruleMn+0, ruleM+0
    printf "RULE\tL vmovl -> vshll (widening-move alias)\t%d\t%d\n", ruleLn+0, ruleL+0
    printf "MIXREF\t%d\t%d\t%s\n", mixRn+0, mixRw+0, mixRstr
    printf "MIXINT\t%d\t%d\t%s\n", mixIn+0, mixIw+0, mixIstr
}' "$HIST" > "$TMP/stats.tsv"

# ---------------------------------------------------------------------------
# Step 5: case-level status table
# ---------------------------------------------------------------------------
gawk -F'\t' '
FILENAME==ARGV[1] { grp[$1]=$2; next }
FILENAME==ARGV[2] { st[$1]=$4; loc[$1]=$2":"$3; next }
FILENAME==ARGV[3] { dis[$1]=1; next }
END {
    for (c in grp) {
        s = (c in st) ? st[c] : "no-translator-at-all"
        l = (c in loc) ? loc[c] : "absent"
        print c "\t" grp[c] "\t" s "\t" l "\t" (c in dis ? "yes" : "no")
    }
}' "$TMP/bases.tsv" "$TMP/defs.tsv" "$TMP/disasm.txt" | LC_ALL=C sort -t$'\t' -k1,1 > "$TMP/casestatus.tsv"

# ---------------------------------------------------------------------------
# Step 6: render report
# ---------------------------------------------------------------------------
TOTR=$(awk -F'\t' '$1=="TOTAL"{print $2}' "$TMP/stats.tsv")
TOTW=$(awk -F'\t' '$1=="TOTAL"{print $3}' "$TMP/stats.tsv")
NDEC=$(cut -f1 "$TMP/bases.tsv" | LC_ALL=C sort -u | wc -l)
NDIS=$(wc -l < "$TMP/disasm.txt")

{
echo "================================================================================"
echo " Mnemonic -> Dynarmic handler map (tools/mnemonic_handlers.sh)"
echo " generated: $(date -u +%Y-%m-%dT%H:%MZ)   histogram: $HIST"
echo "================================================================================"
echo
echo "Decoder-case universe: ${NDEC} unique INST cases in frontend/A32/decoder/*.inc"
echo "  (groups: arm_ thumb16_ thumb32_ vfp_ asimd_ v8_). Disassembler defines ${NDIS} of"
echo "  them (arm_/vfp_/thumb16_ only); thumb32_/asimd_/v8_ exist only in decoder+translator."
echo "Join key: objdump mnemonic -> lowercase first word of the INST mnemonic string"
echo "(with alias/'{placeholder}' expansion); translator status from the extracted body."
echo
echo "--------------------------------------------------------------------------------"
echo " 1. JOIN QUALITY"
echo "--------------------------------------------------------------------------------"
awk -F'\t' '$1=="PRE"  {printf "rows matched BEFORE normalisation (raw lowercase == a decoder base): %5d / '"$TOTR"'  (%d instructions)\n", $2, $3}' "$TMP/stats.tsv"
awk -F'\t' '$1=="POST" {printf "rows matched AFTER  normalisation:                                  %5d / '"$TOTR"'  (%d instructions)\n", $2, $3}' "$TMP/stats.tsv"
awk -F'\t' '$1=="POST" {printf "rows unmatched: %d\n", '"$TOTR"'-$2}' "$TMP/stats.tsv"
echo "Instruction base: $TOTW, the sum of the histogram rows - percentages below use it."
echo "(Deliberately not a quoted figure from docs/32: that document's totals were corrected twice, and a"
echo " base recomputed here cannot go stale the way a copied number does. If the two disagree, the"
echo " histogram and docs/32 §1 were regenerated at different revisions - re-run isa_coverage.sh first.)"
awk -F'\t' '$1=="BAD" && $2>0 {printf "malformed histogram rows skipped: %d\n", $2}' "$TMP/stats.tsv"
echo
echo "Normalisation rules (rows/instructions where the rule was on the matching chain):"
grep '^RULE' "$TMP/stats.tsv" | awk -F'\t' '{printf "  %-34s %5d rows  %9d instr\n", $2, $3, $4}'
echo
echo "Unmatched mnemonics (all, sorted by demand):"
if [ -s "$TMP/unmatched.tsv" ]; then
    LC_ALL=C sort -t$'\t' -k1,1nr -k2,2 "$TMP/unmatched.tsv" | head -60 \
      | awk -F'\t' '{printf "  %7d  %-14s  %s\n", $1, $2, $3}'
    UNM=$(wc -l < "$TMP/unmatched.tsv")
    if [ "$UNM" -gt 60 ]; then echo "  ... ($UNM unmatched rows total)"; fi
    echo
    echo "Unmatched causes observed:"
    awk -F'\t' '{w[$3]+=$1; n[$3]++} END {for (c in n) printf "%d\t%d\t%s\n", n[c], w[c], c}' "$TMP/unmatched.tsv" \
      | LC_ALL=C sort -k1,1nr | head -40 \
      | awk -F'\t' '{printf "  %-52s %4d rows %8d instr\n", $3, $1, $2}'
else
    echo "  none - every histogram row matched at least one decoder case"
fi
echo
echo "--------------------------------------------------------------------------------"
echo " 2. BUCKETS OVER DEMANDED MNEMONICS (headline for docs/32-coverage.md §4)"
echo "--------------------------------------------------------------------------------"
awk -F'\t' -v totr="$TOTR" -v totw="$TOTW" '
$1=="BUCKET" { printf "%s\t%s\t%d\t%d\n", $2, $3, $4, totr }' "$TMP/stats.tsv" > "$TMP/buckets.tsv"
awk -F'\t' '{ rows[$1]=$2; w[$1]=$3 }
END {
  split("implemented interpret-fallback declared-but-refuses no-translator-at-all unmatched unknown-status", order, " ")
  for (i in order) { b=order[i]
    printf "%9d  %6.2f%%  %4d rows %5.1f%%  %s\n", w[b]+0, 100*(w[b]+0)/'"$TOTW"', rows[b]+0, 100*(rows[b]+0)/'"$TOTR"', b }
}' "$TMP/buckets.tsv"
echo "  (percentages: instruction-weighted first, then mnemonic-count; base = $TOTW instructions)"
awk -F'\t' '$1=="MIXREF" && $2>0 { printf "  NOTE: %d implemented rows (%d instr) DO have at least one matched decoder case\n        whose translator body refuses: %s\n", $2, $3, $4 }' "$TMP/stats.tsv"
awk -F'\t' '$1=="MIXINT" && $2>0 { printf "  NOTE: %d implemented rows (%d instr) include a matched decoder case that only\n        falls back to the interpreter (not compiled): %s\n", $2, $3, $4 }' "$TMP/stats.tsv"
echo "  rollup rule: a mnemonic lands in the BEST bucket among its matched decoder cases"
echo "  (e.g. bare 'add' has ARM + thumb16 + thumb32 cases; its real encodings are split"
echo "  across them, so exact per-encoding attribution is impossible from mnemonics alone)."
echo
echo "--------------------------------------------------------------------------------"
echo " 3. TRIAGE LIST - every non-implemented row, sorted by instruction demand"
echo "--------------------------------------------------------------------------------"
printf "%9s  %-14s  %-22s  %s\n" "INSTR" "MNEMONIC" "BUCKET" "DYNARMIC SOURCE LOCATION (translate/impl file:line, or 'absent')"
LC_ALL=C sort -t$'\t' -k1,1nr "$TMP/rows.tsv" | awk -F'\t' '$3!="implemented" && $2 !~ /^[0-9a-f][0-9a-f]$/' | head -60 \
  | awk -F'\t' '{printf "%9d  %-14s  %-22s  %s\n", $1, $2, $3, $4}'
NTRI=$(awk -F'\t' '$3!="implemented"' "$TMP/rows.tsv" | wc -l)
NHEX=$(awk -F'\t' '$3!="implemented" && $2 ~ /^[0-9a-f][0-9a-f]$/' "$TMP/rows.tsv" | wc -l)
echo "  ($NTRI non-implemented rows total; $NHEX of them are objdump '.word' data-byte tokens (not instructions) and are suppressed above.)"
echo "  ('no-translator-at-all' => decoder case with no TranslatorVisitor:: definition; 'absent' on unmatched rows => no decoder case at all)"
echo
echo "--------------------------------------------------------------------------------"
echo " 3b. CLASSIFICATION EVIDENCE - every 'declared-but-refuses' and interpret-fallback"
echo "     case with its translator body verbatim"
echo "--------------------------------------------------------------------------------"
LC_ALL=C sort -t$'\t' -k1,1 "$TMP/defs.tsv" | LC_ALL=C awk -F'\t' '$4=="refuses" || $4=="interpret-fallback" {printf "  %-14s %-20s %s:%s\n      body: %s\n", $4, $1, $2, $3, $5}'
echo "  Sample of 'implemented' (first 150 chars of extracted bodies):"
LC_ALL=C awk -F'\t' '$1 ~ /^(arm_ADC_imm|arm_LDR_imm|arm_BL|thumb32_ADD_imm_1)$/ {printf "    %-18s %s:%s  [%s]  %s\n", $1, $2, $3, $4, substr($5,1,150)}' "$TMP/defs.tsv"
NUNK=$(awk -F'\t' '$4=="unknown"' "$TMP/defs.tsv" | wc -l)
NTOT=$(wc -l < "$TMP/defs.tsv")
echo "  Bodies not classifiable by direct textual reading: $NUNK of $NTOT definitions."
if [ "$NUNK" -gt 0 ]; then
    LC_ALL=C awk -F'\t' '$4=="unknown" {printf "    %s  %s:%s  %s\n", $1, $2, $3, substr($5,1,110)}' "$TMP/defs.tsv" | head -20
fi
echo "  All 759 decoder cases by translator status (case-level, not demand-weighted):"
gawk -F'\t' 'NR==FNR{if(!seen[$1]++)c[$1]=1;next} ($1 in c){s[$4]++} END{for(k in s) printf "    %-22s %4d\n", k, s[k]}' \
    "$TMP/bases.tsv" "$TMP/defs.tsv"
echo
echo "--------------------------------------------------------------------------------"
echo " 4. MACHINE-READABLE BLOCKS"
echo "--------------------------------------------------------------------------------"
echo "#CSV-MNEMONIC mnemonic,instructions,bucket,deciding_location,ncases,isa_groups,cases"
LC_ALL=C sort -t$'\t' -k1,1nr "$TMP/rows.tsv" | awk -F'\t' '{printf "%s,%d,%s,%s,%d,%s,%s\n", $2,$1,$3,$4,$5,$6,$7}'
echo "#CSV-CASE case,group,status,source_location,in_disassembler"
LC_ALL=C sed 's/\t/,/g' "$TMP/casestatus.tsv"
} | tee "$REPORT"

echo >&2 "report written to $REPORT"
