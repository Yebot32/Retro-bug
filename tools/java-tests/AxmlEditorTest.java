// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for AxmlEditor. Plain main() rather than JUnit so the suite runs on a machine that has only a
// JDK and the SDK on it. Fixtures are the APKs tools/run_java_tests.sh builds with `aapt2 link` from the
// XML under tests/corpus/manifests, so the bytes under test are what the real toolchain emits, not
// something this file hand-assembled to agree with the parser.
//
// args: <fixture dir holding min_only, target22, target_only, no_uses_sdk .apk> <scratch dir> <aapt2 path>
package com.retrobug.launcher;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

public class AxmlEditorTest {
    static int checks = 0, failures = 0;
    static File fixtures, scratch;
    static String aapt2;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static byte[] manifestOf(File apk) throws IOException {
        try (ZipFile z = new ZipFile(apk)) {
            ZipEntry e = z.getEntry("AndroidManifest.xml");
            if (e == null) throw new IOException(apk + " has no AndroidManifest.xml");
            try (InputStream in = z.getInputStream(e)) { return in.readAllBytes(); }
        }
    }

    static byte[] fixture(String name) throws IOException { return manifestOf(new File(fixtures, name + ".apk")); }

    static void minOnlyReadsBack() throws IOException {
        AxmlEditor e = AxmlEditor.parse(fixture("min_only"));
        check("com.retrobug.fixture".equals(e.packageName()), "min_only packageName: " + e.packageName());
        check(Integer.valueOf(8).equals(e.minSdk()), "min_only minSdk: " + e.minSdk());
        check(e.targetSdk() == null, "min_only targetSdk should be absent: " + e.targetSdk());
    }

    // The authority on whether the bytes are a valid manifest is aapt2, not our own parser: swap only the
    // manifest entry of the aapt2-built APK (so resources.arsc is still there) and ask it for the badging.
    static String badging(String base, byte[] manifest, String tag) throws IOException, InterruptedException {
        File out = new File(scratch, tag + ".apk");
        try (ZipFile src = new ZipFile(new File(fixtures, base + ".apk"));
             ZipOutputStream zo = new ZipOutputStream(Files.newOutputStream(out.toPath()))) {
            for (Enumeration<? extends ZipEntry> en = src.entries(); en.hasMoreElements(); ) {
                ZipEntry se = en.nextElement();
                zo.putNextEntry(new ZipEntry(se.getName()));
                if (se.getName().equals("AndroidManifest.xml")) zo.write(manifest);
                else try (InputStream in = src.getInputStream(se)) { in.transferTo(zo); }
                zo.closeEntry();
            }
        }
        File txt = new File(scratch, tag + ".badging.txt");
        int rc = new ProcessBuilder(aapt2, "dump", "badging", out.getPath())
                .redirectErrorStream(true).redirectOutput(txt).start().waitFor();
        String text = new String(Files.readAllBytes(txt.toPath()), "UTF-8");
        check(rc == 0, tag + ": aapt2 dump badging exit " + rc + ": " + text);
        return text;
    }

    /** Edit {@code manifest} to target 24 and judge the result with the parser and with aapt2. */
    static void editAndJudge(String base, byte[] manifest, String tag, Integer expectMin) throws Exception {
        AxmlEditor e = AxmlEditor.parse(manifest);
        e.setTargetSdk(24);
        byte[] edited = e.toBytes();
        AxmlEditor r = AxmlEditor.parse(edited);
        check(Integer.valueOf(24).equals(r.targetSdk()), tag + ": targetSdk re-parses: " + r.targetSdk());
        check(java.util.Objects.equals(expectMin, r.minSdk()), tag + ": minSdk is " + expectMin + ": " + r.minSdk());
        check("com.retrobug.fixture".equals(r.packageName()), tag + ": packageName survives: " + r.packageName());
        String b = badging(base, edited, tag);
        check(b.contains("targetSdkVersion:'24'"), tag + ": badging reports targetSdkVersion 24: " + b);
        check(b.contains("package: name='com.retrobug.fixture'"), tag + ": badging reports package: " + b);
        if (expectMin != null) check(b.contains("minSdkVersion:'" + expectMin + "'"), tag + ": badging keeps minSdk: " + b);
        check(poolIsUtf8(edited) == poolIsUtf8(manifest), tag + ": pool encoding preserved by the edit");
    }

    /** Edit {@code manifest} to minSdk 24 and judge it: parser, attribute order, pool encoding, aapt2. */
    static void editMinAndJudge(String base, byte[] manifest, String tag, Integer expectTarget) throws Exception {
        AxmlEditor e = AxmlEditor.parse(manifest);
        e.setMinSdk(24);
        byte[] edited = e.toBytes();
        AxmlEditor r = AxmlEditor.parse(edited);
        check(Integer.valueOf(24).equals(r.minSdk()), tag + ": minSdk re-parses: " + r.minSdk());
        check(java.util.Objects.equals(expectTarget, r.targetSdk()), tag + ": targetSdk is " + expectTarget + ": " + r.targetSdk());
        check("com.retrobug.fixture".equals(r.packageName()), tag + ": packageName survives: " + r.packageName());
        java.util.List<String> names = r.attrNames("uses-sdk");
        if (expectTarget != null)
            check(names.equals(java.util.Arrays.asList("minSdkVersion", "targetSdkVersion")),
                    tag + ": uses-sdk attributes sorted by id (min 0x0101020c before target 0x01010270): " + names);
        String b = badging(base, edited, tag);
        check(b.contains("minSdkVersion:'24'"), tag + ": badging reports minSdkVersion 24: " + b);
        check(b.contains("package: name='com.retrobug.fixture'"), tag + ": badging reports package: " + b);
        if (expectTarget != null) check(b.contains("targetSdkVersion:'" + expectTarget + "'"), tag + ": badging keeps targetSdk: " + b);
        check(poolIsUtf8(edited) == poolIsUtf8(manifest), tag + ": pool encoding preserved by the min edit");
    }

    static void minSdkEdits() throws Exception {
        byte[] minOnly = fixture("min_only"), targetOnly = fixture("target_only"), none = fixture("no_uses_sdk");
        check(AxmlEditor.parse(targetOnly).minSdk() == null && Integer.valueOf(22).equals(AxmlEditor.parse(targetOnly).targetSdk()),
                "target_only fixture: no minSdk, targetSdk 22");
        check(!poolIsUtf8(targetOnly), "target_only fixture carries a UTF-16 pool");
        // rewrite in place: same length
        AxmlEditor rw = AxmlEditor.parse(minOnly);
        rw.setMinSdk(24);
        check(rw.toBytes().length == minOnly.length, "min rewrite in place keeps length");
        editMinAndJudge("min_only", minOnly, "min-utf16-rewrite", null);
        editMinAndJudge("target22", fixture("target22"), "min-utf16-rewrite-with-target", 22);
        editMinAndJudge("target_only", targetOnly, "min-utf16-insert", 22);
        editMinAndJudge("no_uses_sdk", none, "min-utf16-create", null);
        editMinAndJudge("min_only", toUtf8Pool(minOnly), "min-utf8-rewrite", null);
        editMinAndJudge("target_only", toUtf8Pool(targetOnly), "min-utf8-insert", 22);
        editMinAndJudge("no_uses_sdk", toUtf8Pool(none), "min-utf8-create", null);
        // What the repackager does to a manifest with no <uses-sdk>: create via min, then add target.
        for (byte[] m : new byte[][] {none, toUtf8Pool(none)}) {
            AxmlEditor e = AxmlEditor.parse(m);
            e.setMinSdk(24);
            e.setTargetSdk(24);
            byte[] out = e.toBytes();
            check(AxmlEditor.parse(out).attrNames("uses-sdk").equals(java.util.Arrays.asList("minSdkVersion", "targetSdkVersion")),
                    "min then target on a created uses-sdk stays id-sorted: " + AxmlEditor.parse(out).attrNames("uses-sdk"));
            String b = badging("no_uses_sdk", out, "min-then-target-" + (poolIsUtf8(m) ? "utf8" : "utf16"));
            check(b.contains("minSdkVersion:'24'") && b.contains("targetSdkVersion:'24'"), "min+target both 24 in badging: " + b);
        }
    }

    static void insertTargetSdk() throws Exception { editAndJudge("min_only", fixture("min_only"), "insert", 8); }

    static void createUsesSdk() throws Exception {
        byte[] m = fixture("no_uses_sdk");
        check(AxmlEditor.parse(m).targetSdk() == null && AxmlEditor.parse(m).minSdk() == null, "no_uses_sdk has no sdk attributes");
        editAndJudge("no_uses_sdk", m, "create-uses-sdk", null);
    }

    // ---- Pool encodings. aapt2 emits UTF-16 manifests (Gumball Lite's is too), so the fixtures exercise
    // that path natively; the UTF-8 path, which other tools produce, is covered by transcoding a fixture's
    // pool here, test-side, rather than teaching AxmlEditor a mode it does not need.
    static int poolOff(byte[] a) {
        for (int p = 8; p + 8 <= a.length; p += le(a, p + 4)) if (le16(a, p) == 1) return p;
        throw new IllegalStateException("no string pool");
    }
    static int le16(byte[] b, int o) { return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8; }
    static int le(byte[] b, int o) { return le16(b, o) | le16(b, o + 2) << 16; }
    static void w32(byte[] b, int o, int v) { for (int i = 0; i < 4; i++) b[o + i] = (byte) (v >> 8 * i); }
    static boolean poolIsUtf8(byte[] a) { return (le(a, poolOff(a) + 16) & 0x100) != 0; }

    /** Re-encode a UTF-16 pool as UTF-8 (flag 0x100), fixing offsets, stringsStart and every chunk size. */
    static byte[] toUtf8Pool(byte[] a) {
        int p = poolOff(a), size = le(a, p + 4), hdr = le16(a, p + 2), n = le(a, p + 8), start = le(a, p + 20);
        java.io.ByteArrayOutputStream data = new java.io.ByteArrayOutputStream();
        int[] offs = new int[n];
        for (int i = 0; i < n; i++) {
            int s = p + start + le(a, p + hdr + 4 * i), len = le16(a, s);
            String str = new String(a, s + 2, len * 2, java.nio.charset.StandardCharsets.UTF_16LE);
            byte[] u = str.getBytes(java.nio.charset.StandardCharsets.UTF_8);
            offs[i] = data.size();
            data.write(str.length()); data.write(u.length);   // fixture strings are all < 128
            data.write(u, 0, u.length); data.write(0);
        }
        while (data.size() % 4 != 0) data.write(0);
        int newStart = hdr + 4 * n, newSize = newStart + data.size();
        byte[] pool = new byte[newSize];
        System.arraycopy(a, p, pool, 0, hdr);
        w32(pool, 4, newSize);
        w32(pool, 16, le(a, p + 16) | 0x100);
        w32(pool, 20, newStart);
        for (int i = 0; i < n; i++) w32(pool, hdr + 4 * i, offs[i]);
        System.arraycopy(data.toByteArray(), 0, pool, newStart, data.size());
        byte[] out = new byte[a.length - size + newSize];
        System.arraycopy(a, 0, out, 0, p);
        System.arraycopy(pool, 0, out, p, newSize);
        System.arraycopy(a, p + size, out, p + newSize, a.length - p - size);
        w32(out, 4, out.length);
        return out;
    }

    static void poolEncodings() throws Exception {
        // UTF-16 is what aapt2 wrote; prove it so these checks cannot silently turn into UTF-8 ones.
        check(!poolIsUtf8(fixture("min_only")), "aapt2 fixture carries a UTF-16 pool");
        editAndJudge("min_only", fixture("min_only"), "utf16-insert", 8);
        editAndJudge("target22", fixture("target22"), "utf16-rewrite", 8);
        editAndJudge("no_uses_sdk", fixture("no_uses_sdk"), "utf16-create", null);
        byte[] ins = toUtf8Pool(fixture("min_only")), rew = toUtf8Pool(fixture("target22"));
        check(poolIsUtf8(ins) && poolIsUtf8(rew), "transcoded fixtures carry a UTF-8 pool");
        check("com.retrobug.fixture".equals(AxmlEditor.parse(ins).packageName()), "UTF-8 pool decodes packageName");
        editAndJudge("min_only", ins, "utf8-insert", 8);
        editAndJudge("target22", rew, "utf8-rewrite", 8);
        editAndJudge("no_uses_sdk", toUtf8Pool(fixture("no_uses_sdk")), "utf8-create", null);
    }

    // Truncations whose outer length is patched to match, so they get past the file-header check and into
    // the chunk readers, where short or lying chunks used to escape as ArrayIndexOutOfBounds.
    static void refusesTruncation() throws IOException {
        for (String name : new String[]{"min_only", "target22"}) {
            byte[] full = fixture(name);
            for (int cut = 4; cut < full.length; cut += 4) {
                byte[] t = java.util.Arrays.copyOf(full, cut);
                if (cut >= 8) w32(t, 4, cut);
                try {
                    AxmlEditor e = AxmlEditor.parse(t);
                    e.packageName(); e.minSdk(); e.targetSdk(); e.extractNativeLibs();
                    e.setTargetSdk(24); e.toBytes();
                } catch (IOException ok) {
                    // expected for most cuts; a cut on a chunk boundary may legitimately parse
                } catch (RuntimeException ex) {
                    check(false, name + " truncated to " + cut + " threw " + ex);
                    return;
                }
            }
            byte[] half = java.util.Arrays.copyOf(full, full.length / 2);
            try { AxmlEditor.parse(half); check(false, name + " half-length with stale header should throw"); }
            catch (IOException ok) { check(true, "ok"); }
        }
    }

    static void rewriteTargetSdk() throws IOException {
        byte[] orig = fixture("target22");
        AxmlEditor e = AxmlEditor.parse(orig);
        check(Integer.valueOf(22).equals(e.targetSdk()), "target22 targetSdk: " + e.targetSdk());
        e.setTargetSdk(24);
        check(Integer.valueOf(24).equals(e.targetSdk()), "targetSdk after rewrite: " + e.targetSdk());
        byte[] edited = e.toBytes();
        check(edited.length == orig.length, "in-place rewrite keeps length: " + orig.length + " -> " + edited.length);
        check(Integer.valueOf(24).equals(AxmlEditor.parse(edited).targetSdk()), "rewrite survives re-parse");
    }

    static void readsExtractNativeLibs() throws IOException {
        check(Boolean.FALSE.equals(AxmlEditor.parse(fixture("target22")).extractNativeLibs()),
                "target22 extractNativeLibs should be false");
        check(AxmlEditor.parse(fixture("min_only")).extractNativeLibs() == null,
                "min_only extractNativeLibs should be absent");
    }

    /** Diagnostics option: android:debuggable="true" on <application>, inserted when absent; aapt2 reads it back. */
    static void setsDebuggable() throws Exception {
        for (String base : new String[] {"min_only", "target22"}) {
            AxmlEditor ed = AxmlEditor.parse(fixture(base));
            ed.setDebuggable();
            String tree = xmltree(base, ed.toBytes(), base + "-debuggable");
            check(tree.contains("android:debuggable") && tree.contains("=true"), base + ": debuggable not true in\n" + tree);
        }
    }

    /** API 31+: every intent-filtered component gets the old default exported="true"; one that sets it keeps its value. */
    static void setsExportedDefaults() throws Exception {
        AxmlEditor ed = AxmlEditor.parse(fixture("filters"));
        int n = ed.setExportedDefaults();
        String tree = xmltree("filters", ed.toBytes(), "filters-exported");
        check(n == 1, "filters: " + n + " components changed (want 1: the activity)");
        int act = tree.indexOf("E: activity"), svc = tree.indexOf("E: service"), rx = tree.indexOf("E: receiver");
        check(act >= 0 && svc > act && rx > svc, "filters: element order in\n" + tree);
        if (act >= 0 && svc > act && rx > svc) {
            check(tree.substring(act, svc).contains("exported(0x01010010)=true"), "activity exported=true in\n" + tree);
            check(tree.substring(svc, rx).contains("exported(0x01010010)=false"), "service keeps exported=false in\n" + tree);
            check(!tree.substring(rx).contains("exported"), "the filterless receiver is untouched in\n" + tree);
        }
        AxmlEditor again = AxmlEditor.parse(ed.toBytes());
        check(again.setExportedDefaults() == 0, "a second pass changes nothing");
    }

    static void refusesGarbage() {
        try {
            AxmlEditor.parse(new byte[]{1, 2, 3});
            check(false, "garbage should throw");
        } catch (IOException ex) {
            check(ex.getMessage() != null && ex.getMessage().contains("not a binary XML"),
                    "garbage message names the problem: " + ex.getMessage());
        }
    }

    // Truncation alone is stopped by the outer size checks, so also corrupt every dword in turn with values
    // that lie about lengths and offsets; whatever parses must still never throw a bare runtime exception.
    static void refusesLyingFields() {
        int[] lies = {0, 0x7ffffff0, -1, 0x10000, 3};
        for (String name : new String[]{"min_only", "target22"}) {
            byte[] full;
            try { full = fixture(name); } catch (IOException ex) { check(false, "fixture " + name); return; }
            for (int o = 8; o + 4 <= full.length; o += 4) {
                for (int lie : lies) {
                    byte[] t = full.clone();
                    w32(t, o, lie);
                    try {
                        AxmlEditor e = AxmlEditor.parse(t);
                        e.packageName(); e.minSdk(); e.targetSdk(); e.extractNativeLibs();
                        e.setTargetSdk(24); e.toBytes();
                    } catch (IOException ok) {
                        // named refusal: what is wanted
                    } catch (RuntimeException ex) {
                        check(false, name + " dword at " + o + " set to " + lie + " threw " + ex);
                        return;
                    }
                }
            }
        }
    }

    /** aapt2's `dump xmltree` of {@code manifest} inside a copy of the {@code base} fixture APK. */
    static String xmltree(String base, byte[] manifest, String tag) throws Exception {
        badging(base, manifest, tag);                       // writes <tag>.apk and checks the badging parses
        File txt = new File(scratch, tag + ".xmltree.txt");
        int rc = new ProcessBuilder(aapt2, "dump", "xmltree", "--file", "AndroidManifest.xml",
                new File(scratch, tag + ".apk").getPath()).redirectErrorStream(true).redirectOutput(txt).start().waitFor();
        String t = new String(Files.readAllBytes(txt.toPath()), "UTF-8");
        check(rc == 0, tag + ": aapt2 dump xmltree exit " + rc + ": " + t);
        return t;
    }

    static final String A = "http://schemas.android.com/apk/res/android:";

    static void renamePackage() throws Exception {
        AxmlEditor e = AxmlEditor.parse(fixture("rename"));
        java.util.List<String> changes = e.renamePackage("com.retrobug.fixture.rb");
        byte[] out = e.toBytes();
        AxmlEditor r = AxmlEditor.parse(out);
        check("com.retrobug.fixture.rb".equals(r.packageName()), "rename: packageName re-parses: " + r.packageName());
        check(changes.contains("manifest package: com.retrobug.fixture -> com.retrobug.fixture.rb"), "rename: change list: " + changes);
        String b = badging("rename", out, "rename");
        check(b.contains("package: name='com.retrobug.fixture.rb'"), "rename: badging reports the new package: " + b);
        check(b.contains("launchable-activity: name='com.retrobug.fixture.Main'"),
                "rename: launchable activity is the original package's class: " + b);
        String t = xmltree("rename", out, "rename-x");
        String[] want = {
                "package=\"com.retrobug.fixture.rb\"",
                // class names, expanded against the ORIGINAL package (the DEX is not renamed)
                A + "name(0x01010003)=\"com.retrobug.fixture.App\"",
                A + "backupAgent(0x0101027f)=\"com.retrobug.fixture.Backup\"",
                A + "name(0x01010003)=\"com.retrobug.fixture.Main\"",
                A + "parentActivityName(0x010103a7)=\"com.retrobug.fixture.Main\"",
                A + "name(0x01010003)=\"com.retrobug.fixture.Alias\"",
                A + "targetActivity(0x01010202)=\"com.retrobug.fixture.Main\"",
                A + "name(0x01010003)=\"com.retrobug.fixture.sub.Svc\"",
                A + "name(0x01010003)=\"com.retrobug.fixture.Recv\"",
                A + "name(0x01010003)=\"com.retrobug.fixture.Prov\"",
                A + "name(0x01010003)=\"com.thirdparty.AdActivity\"",
                // unique-per-device names follow the package
                A + "authorities(0x01010018)=\"com.retrobug.fixture.rb.files;com.facebook.app.FacebookContentProvider123.rb\"",
                A + "name(0x01010003)=\"com.retrobug.fixture.rb.permission.C2D_MESSAGE\"",
                A + "name(0x01010003)=\"org.other.permission.SHARED.rb\"",
                A + "permission(0x01010006)=\"org.other.permission.SHARED.rb\"",
                A + "readPermission(0x01010007)=\"com.retrobug.fixture.rb.permission.C2D_MESSAGE\"",
                A + "name(0x01010003)=\"com.retrobug.fixture.rb\"",                     // GCM category
                A + "taskAffinity(0x01010012)=\"com.retrobug.fixture.rb.main\"",
                // untouched
                A + "name(0x01010003)=\"android.permission.INTERNET\"",
                A + "name(0x01010003)=\"android.intent.category.LAUNCHER\"",
                A + "name(0x01010003)=\"com.google.android.c2dm.intent.RECEIVE\"",
        };
        for (String w : want) check(t.contains(w), "rename: aapt2 xmltree has " + w + "\n" + t);
        check(!t.contains("\".Main\"") && !t.contains("\"Backup\"") && !t.contains("\".sub.Svc\""),
                "rename: no relative class name left: " + t);
        check(!t.contains("\"com.retrobug.fixture.permission.C2D_MESSAGE\"") && !t.contains("\"com.retrobug.fixture.files"),
                "rename: no old permission or authority left: " + t);
        check(t.indexOf("C2D_MESSAGE\"") != t.lastIndexOf("C2D_MESSAGE\""), "rename: the declared permission and its uses agree");

        // The other edits still work on a renamed manifest (the repackager renames first, then raises the SDKs).
        AxmlEditor r2 = AxmlEditor.parse(out);
        r2.setTargetSdk(24);
        r2.addProvider("com.retrobug.shim.RbShimProvider", "com.retrobug.fixture.rb.rbshim", Integer.MAX_VALUE);
        String t2 = xmltree("rename", r2.toBytes(), "rename-then-edit");
        check(t2.contains("package=\"com.retrobug.fixture.rb\"") && t2.contains("targetSdkVersion(0x01010270)=24")
                && t2.contains(A + "name(0x01010003)=\"com.retrobug.fixture.Main\""), "rename then edit: " + t2);

        check("a.b.C".equals(AxmlEditor.fullClassName("p.q", "a.b.C")) && "p.q.C".equals(AxmlEditor.fullClassName("p.q", "C"))
                && "p.q.x.C".equals(AxmlEditor.fullClassName("p.q", ".x.C")), "fullClassName follows buildClassName");
        check("p.q2".equals(AxmlEditor.renamedName("p.q", "p.q2", "p.q")) && "p.qq.x.y".equals(AxmlEditor.renamedName("p.q", "x.y", "p.qq")),
                "renamedName: a prefix match is whole segments only");
        check("x.y.a".equals(AxmlEditor.renamedName("p.q", "x.y", "p.q.a")) && "o.a.x.y".equals(AxmlEditor.renamedName("p.q", "x.y", "o.a")),
                "renamedName with an unrelated new package");

        for (String bad : new String[] {"com.retrobug.fixture", "nodots", "com..x", "1com.x", "com.x-y", ""}) {
            try {
                AxmlEditor.parse(fixture("rename")).renamePackage(bad);
                check(false, "rename to '" + bad + "' should be refused");
            } catch (IOException ex) {
                checks++;
            }
        }
        // A manifest with nothing but a package: only the package changes.
        AxmlEditor m = AxmlEditor.parse(fixture("min_only"));
        java.util.List<String> c2 = m.renamePackage("org.example.other");
        String t3 = xmltree("min_only", m.toBytes(), "rename-min");
        check(t3.contains("package=\"org.example.other\"") && t3.contains("\"com.retrobug.fixture.Main\""),
                "min_only rename: package and expanded activity: " + t3);
        check(c2.size() == 2, "min_only rename: package + one activity changed: " + c2);
    }

    public static void main(String[] a) throws Exception {
        fixtures = new File(a[0]); scratch = new File(a[1]); aapt2 = a[2];
        minOnlyReadsBack();
        insertTargetSdk();
        createUsesSdk();
        minSdkEdits();
        poolEncodings();
        refusesTruncation();
        refusesLyingFields();
        rewriteTargetSdk();
        readsExtractNativeLibs();
        renamePackage();
        refusesGarbage();
        setsDebuggable();
        setsExportedDefaults();
        System.out.println(checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
