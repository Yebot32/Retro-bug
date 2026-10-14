// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for AxmlEditor. Plain main() rather than JUnit so the suite runs on a machine that has only a
// JDK and the SDK on it. Fixtures are the APKs tools/run_java_tests.sh builds with `aapt2 link` from the
// XML under tests/corpus/manifests, so the bytes under test are what the real toolchain emits, not
// something this file hand-assembled to agree with the parser.
//
// args: <fixture dir holding min_only.apk, target22.apk> <scratch dir> <aapt2 path>
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

    static void refusesGarbage() {
        try {
            AxmlEditor.parse(new byte[]{1, 2, 3});
            check(false, "garbage should throw");
        } catch (IOException ex) {
            check(ex.getMessage() != null && ex.getMessage().contains("not a binary XML"),
                    "garbage message names the problem: " + ex.getMessage());
        }
    }

    public static void main(String[] a) throws Exception {
        fixtures = new File(a[0]); scratch = new File(a[1]); aapt2 = a[2];
        minOnlyReadsBack();
        insertTargetSdk();
        createUsesSdk();
        poolEncodings();
        refusesTruncation();
        rewriteTargetSdk();
        readsExtractNativeLibs();
        refusesGarbage();
        System.out.println(checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
