# patches/

RetroBug-side changes to vendored dependencies, per architecture decision AD-14: the trees under
`external/` stay byte-identical to their pinned upstream commits, and every modification lives here as a
patch file. Nothing applies them automatically at configure time (an earlier revision of this sentence
said it did; no `git apply`, `patch -p` or `PATCH_COMMAND` exists anywhere in the build), so the patch
files are the *record* of edits that are also present in the tree — see `dynarmic/README.md` for the
invocation, and rule 3 below for what keeps the two in agreement.

Layout:

```
patches/<upstream>/NNNN-<short-description>.patch
patches/<upstream>/README.md        why each patch exists, and the condition for retiring it
```

Rules:

1. A patch must state **why** it is needed and what would let us delete it. "Upstream is archived" is not a
   reason; "upstream assumes a single-core console guest and we need Linux userspace semantics" is.
2. Patches must be small and reviewable. Anything large enough to be called a rewrite is RetroBug code and
   belongs in `core/`, wrapping the upstream API rather than editing it.
3. `diff -r` between the in-tree copy and the pinned upstream commit must be explicable entirely by
   `patches/`. A mismatch is a build break, not a detail.
4. On every re-import (`external/PROVENANCE.md` §"Re-import procedure"), each patch is re-examined: some
   exist only because of a bug since fixed, and carrying them forward is how a vendored tree silently
   diverges.

The lesson behind this policy, from the developer's prior emulator work audited in
`docs/research/08`: keeping the upstream tree pristine and wiring or swapping from outside is what made
re-importing cheap there, and the teams that edited in place paid for it on every update.
