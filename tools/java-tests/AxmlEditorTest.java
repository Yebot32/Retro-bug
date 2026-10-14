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

    static void insertTargetSdk() throws IOException, InterruptedException {
        AxmlEditor e = AxmlEditor.parse(fixture("min_only"));
        e.setTargetSdk(24);
        byte[] edited = e.toBytes();
        AxmlEditor r = AxmlEditor.parse(edited);
        check(Integer.valueOf(24).equals(r.targetSdk()), "inserted targetSdk re-parses: " + r.targetSdk());
        check(Integer.valueOf(8).equals(r.minSdk()), "minSdk survives insert: " + r.minSdk());
        check("com.retrobug.fixture".equals(r.packageName()), "packageName survives insert: " + r.packageName());

        // The authority on whether the bytes are a valid manifest is aapt2, not our own parser: swap only
        // the manifest entry of the aapt2-built APK (so resources.arsc is still there) and ask it.
        File out = new File(scratch, "edited.apk");
        try (ZipFile src = new ZipFile(new File(fixtures, "min_only.apk"));
             ZipOutputStream zo = new ZipOutputStream(Files.newOutputStream(out.toPath()))) {
            for (Enumeration<? extends ZipEntry> en = src.entries(); en.hasMoreElements(); ) {
                ZipEntry se = en.nextElement();
                zo.putNextEntry(new ZipEntry(se.getName()));
                if (se.getName().equals("AndroidManifest.xml")) zo.write(edited);
                else try (InputStream in = src.getInputStream(se)) { in.transferTo(zo); }
                zo.closeEntry();
            }
        }
        File txt = new File(scratch, "badging.txt");
        Process p = new ProcessBuilder(aapt2, "dump", "badging", out.getPath())
                .redirectErrorStream(true).redirectOutput(txt).start();
        int rc = p.waitFor();
        String badging = new String(Files.readAllBytes(txt.toPath()), "UTF-8");
        check(rc == 0, "aapt2 dump badging exit " + rc + ": " + badging);
        check(badging.contains("targetSdkVersion:'24'"), "badging reports targetSdkVersion 24: " + badging);
        check(badging.contains("minSdkVersion:'8'"), "badging still reports minSdkVersion 8: " + badging);
        check(badging.contains("package: name='com.retrobug.fixture'"), "badging reports package: " + badging);
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
        rewriteTargetSdk();
        readsExtractNativeLibs();
        refusesGarbage();
        System.out.println(checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
