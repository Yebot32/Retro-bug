// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for ApkRepackager. The base of every fixture is the aapt2-built APK for a corpus manifest
// (so AndroidManifest.xml and resources.arsc are real toolchain output); the native libs, signature files
// and asset are added here with obviously fake bytes. No real game content is ever involved.
//
// args: <fixture dir> <scratch dir> <aapt2 path (unused)>
package com.retrobug.launcher;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

public class ApkRepackagerTest {
    static int checks = 0, failures = 0;
    static final String SETTINGS_AUTHORITY = "com.retrobug.launcher.settings";
    static File fixtures, scratch;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static byte[] b(String s) { return s.getBytes(StandardCharsets.UTF_8); }

    static final byte[] STUB = b("STUB-ELF-64");
    static final byte[] ENGINE = b("ENGINE-ELF-64");

    static Map<String, byte[]> runtime() {
        Map<String, byte[]> m = new LinkedHashMap<>();
        m.put("lib/arm64-v8a/librbguest.so", ENGINE);
        m.put("stub", STUB);
        for (String s : new String[] {"libc.so", "libm.so", "libz.so", "libstdc++.so"})
            m.put("assets/rb32sys/" + s, b("SYS-" + s));
        return m;
    }

    static long crcOf(byte[] d) { java.util.zip.CRC32 c = new java.util.zip.CRC32(); c.update(d); return c.getValue(); }

    static void putStored(ZipOutputStream zo, String name, byte[] data) throws IOException {
        ZipEntry e = new ZipEntry(name);
        e.setMethod(ZipEntry.STORED);
        e.setSize(data.length); e.setCompressedSize(data.length); e.setCrc(crcOf(data));
        zo.putNextEntry(e); zo.write(data); zo.closeEntry();
    }

    /** Copies the aapt2 APK's entries (manifest swapped if given) and appends extras in order. */
    static File build(String base, byte[] manifest, String tag, Map<String, byte[]> extras) throws IOException {
        File out = new File(scratch, tag + ".apk");
        try (ZipFile src = new ZipFile(new File(fixtures, base + ".apk"));
             ZipOutputStream zo = new ZipOutputStream(Files.newOutputStream(out.toPath()))) {
            for (Enumeration<? extends ZipEntry> en = src.entries(); en.hasMoreElements(); ) {
                ZipEntry se = en.nextElement();
                byte[] data;
                if (manifest != null && se.getName().equals("AndroidManifest.xml")) data = manifest;
                else try (InputStream in = src.getInputStream(se)) { data = in.readAllBytes(); }
                // resources.arsc is STORED in real APKs; keep the fixture honest so the STORED path runs.
                if (se.getName().equals("resources.arsc") || se.getName().equals("assets/stored.bin")) putStored(zo, se.getName(), data);
                else { zo.putNextEntry(new ZipEntry(se.getName())); zo.write(data); zo.closeEntry(); }
            }
            for (Map.Entry<String, byte[]> e : extras.entrySet()) {
                if (e.getKey().equals("assets/stored.bin")) { putStored(zo, e.getKey(), e.getValue()); continue; }
                zo.putNextEntry(new ZipEntry(e.getKey()));
                zo.write(e.getValue());
                zo.closeEntry();
            }
        }
        return out;
    }

    static Map<String, byte[]> fullExtras() {
        Map<String, byte[]> x = new LinkedHashMap<>();
        x.put("lib/armeabi/libfoo.so", b("ARM-OLD-foo"));
        x.put("lib/armeabi-v7a/libfoo.so", b("V7A-foo"));
        x.put("lib/armeabi-v7a/libbar.so", b("V7A-bar"));
        x.put("lib/armeabi-v7a/gdbserver", b("V7A-gdb"));
        x.put("META-INF/CERT.RSA", b("rsa"));
        x.put("META-INF/CERT.SF", b("sf"));
        x.put("META-INF/MANIFEST.MF", b("mf"));
        x.put("META-INF/services/kotlinx.coroutines.internal.MainDispatcherFactory", b("svc"));
        x.put("META-INF/game_release.kotlin_module", b("km"));
        x.put("META-INF/SIG-ANDROID.EC", b("ec"));
        x.put("assets/x.txt", b("hello asset"));
        x.put("assets/stored.bin", b("stored payload 00000001 bytes"));
        return x;
    }

    static Map<String, byte[]> readAll(File apk) throws IOException {
        Map<String, byte[]> m = new LinkedHashMap<>();
        try (ZipFile z = new ZipFile(apk)) {
            for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement();
                try (InputStream in = z.getInputStream(e)) { m.put(e.getName(), in.readAllBytes()); }
            }
        }
        return m;
    }

    static boolean anyStartsWith(Map<String, byte[]> m, String p) {
        for (String k : m.keySet()) if (k.startsWith(p)) return true;
        return false;
    }

    static boolean reportHas(ApkRepackager.Result r, String a, String b2) {
        for (String l : r.report) if (l.contains(a) && l.contains(b2)) return true;
        return false;
    }

    static int methodOf(File apk, String name) throws IOException {
        try (ZipFile z = new ZipFile(apk)) { return z.getEntry(name).getMethod(); }
    }

    static byte[] minOnlyManifestWithTarget(int target) throws IOException {
        AxmlEditor ed = AxmlEditor.parse(AxmlEditorTest.manifestOf(new File(fixtures, "min_only.apk")));
        ed.setTargetSdk(target);
        return ed.toBytes();
    }

    static void movesV7aAndStubs() throws IOException {
        File in = build("min_only", null, "full", fullExtras());
        File out = new File(scratch, "full.out.apk");
        ApkRepackager.Result r = ApkRepackager.transform(in, runtime(), out);
        Map<String, byte[]> o = readAll(out);
        check(Arrays.equals(o.get("assets/rb32/libfoo.so"), b("V7A-foo")), "assets/rb32/libfoo.so is the v7a bytes");
        check(Arrays.equals(o.get("assets/rb32/libbar.so"), b("V7A-bar")), "assets/rb32/libbar.so");
        check(Arrays.equals(o.get("assets/rb32/gdbserver"), b("V7A-gdb")), "assets/rb32/gdbserver");
        check(Arrays.equals(o.get("lib/arm64-v8a/libfoo.so"), STUB), "libfoo stub bytes");
        check(Arrays.equals(o.get("lib/arm64-v8a/libbar.so"), STUB), "libbar stub bytes");
        check(!o.containsKey("lib/arm64-v8a/gdbserver"), "no stub for gdbserver");
        check(Arrays.equals(o.get("lib/arm64-v8a/librbguest.so"), ENGINE), "engine present");
        check(Arrays.equals(o.get("assets/rb32sys/libstdc++.so"), b("SYS-libstdc++.so")), "rb32sys libstdc++");
        check(!o.containsKey("stub"), "the stub key is not itself an entry");
        check(!anyStartsWith(o, "lib/armeabi"), "no lib/armeabi* entries");
        check(!o.containsKey("META-INF/CERT.RSA") && !o.containsKey("META-INF/CERT.SF") && !o.containsKey("META-INF/MANIFEST.MF")
                && !o.containsKey("META-INF/SIG-ANDROID.EC"), "the old signature files are dropped");
        check(Arrays.equals(o.get("META-INF/services/kotlinx.coroutines.internal.MainDispatcherFactory"), b("svc")),
                "META-INF/services (ServiceLoader) kept");
        check(Arrays.equals(o.get("META-INF/game_release.kotlin_module"), b("km")), "other META-INF files kept");
        check(Arrays.equals(o.get("assets/x.txt"), b("hello asset")), "assets/x.txt unchanged");
        check(o.containsKey("resources.arsc"), "resources.arsc kept");
        check(r.stubs.equals(Arrays.asList("libbar.so", "libfoo.so")), "stubs sorted: " + r.stubs);
        check(r.guestLibs.size() == 3 && r.guestLibs.contains("gdbserver"), "guestLibs: " + r.guestLibs);
        check("com.retrobug.fixture".equals(r.packageName), "packageName: " + r.packageName);
        try (ZipFile z = new ZipFile(out)) {
            check(z.getEntry("assets/rb32/libfoo.so").getMethod() == ZipEntry.DEFLATED, ".so is DEFLATED");
            check(z.getEntry("lib/arm64-v8a/libfoo.so").getMethod() == ZipEntry.DEFLATED, "stub is DEFLATED");
            check(z.getEntry("resources.arsc").getMethod() == methodOf(in, "resources.arsc"), "resources.arsc method preserved");
            check(methodOf(in, "assets/stored.bin") == ZipEntry.STORED, "fixture precondition: stored.bin is STORED in the input");
            check(methodOf(in, "resources.arsc") == ZipEntry.STORED, "fixture precondition: resources.arsc is STORED in the input");
            ZipEntry se = z.getEntry("assets/stored.bin");
            check(se.getMethod() == ZipEntry.STORED, "stored.bin stays STORED");
            check(se.getCrc() == crcOf(b("stored payload 00000001 bytes")), "stored.bin CRC equals independently computed CRC");
            check(z.getEntry("resources.arsc").getMethod() == ZipEntry.STORED, "resources.arsc stays STORED");
            check(Arrays.equals(o.get("assets/stored.bin"), b("stored payload 00000001 bytes")), "stored.bin bytes equal");
            check(z.getEntry("assets/x.txt").getTime() == z.getEntry("assets/rb32/libfoo.so").getTime(), "uniform entry times");
        }
        File out2 = new File(scratch, "full.out2.apk");
        ApkRepackager.transform(in, runtime(), out2);
        check(Arrays.equals(Files.readAllBytes(out.toPath()), Files.readAllBytes(out2.toPath())), "reproducible output");
    }

    static void reportsDroppedAndUnstubbed() throws IOException {
        File in = build("min_only", null, "rep", fullExtras());
        ApkRepackager.Result r = ApkRepackager.transform(in, runtime(), new File(scratch, "rep.out.apk"));
        check(reportHas(r, "lib/armeabi/libfoo.so", "dropped"), "report names dropped armeabi lib: " + r.report);
        check(reportHas(r, "gdbserver", "no stub"), "report names unstubbed gdbserver: " + r.report);
        check(reportHas(r, "META-INF", "dropped"), "report names dropped signature files: " + r.report);
    }

    static void bumpsTarget() throws IOException {
        File in = build("min_only", null, "bump", fullExtras());
        File out = new File(scratch, "bump.out.apk");
        ApkRepackager.Result r = ApkRepackager.transform(in, runtime(), out);
        AxmlEditor e = AxmlEditor.parse(readAll(out).get("AndroidManifest.xml"));
        check(Integer.valueOf(24).equals(e.targetSdk()), "targetSdk 24: " + e.targetSdk());
        check(r.oldTargetSdk == -1 && r.newTargetSdk == 24, "old/new: " + r.oldTargetSdk + "/" + r.newTargetSdk);
        check(Integer.valueOf(24).equals(e.minSdk()), "minSdk raised 8 -> 24: " + e.minSdk());
        check(r.oldMinSdk == 8 && r.newMinSdk == 24, "min old/new: " + r.oldMinSdk + "/" + r.newMinSdk);
        check(reportHas(r, "minSdk 8", "-> 24"), "report names the minSdk raise: " + r.report);
    }

    static void raisesAbsentMin() throws IOException {
        Map<String, byte[]> x = new LinkedHashMap<>();
        x.put("lib/armeabi-v7a/libfoo.so", b("V7A"));
        File in = build("no_uses_sdk", null, "nomin", x);
        File out = new File(scratch, "nomin.out.apk");
        ApkRepackager.Result r = ApkRepackager.transform(in, runtime(), out);
        AxmlEditor e = AxmlEditor.parse(readAll(out).get("AndroidManifest.xml"));
        check(r.oldMinSdk == -1 && r.newMinSdk == 24, "absent min -> 24: " + r.oldMinSdk + "/" + r.newMinSdk);
        check(Integer.valueOf(24).equals(e.minSdk()) && Integer.valueOf(24).equals(e.targetSdk()),
                "manifest min/target 24/24: " + e.minSdk() + "/" + e.targetSdk());
        check(reportHas(r, "minSdk absent", "-> 24"), "report names absent min raised: " + r.report);
    }

    static void neverLowersMin() throws IOException {
        AxmlEditor ed = AxmlEditor.parse(AxmlEditorTest.manifestOf(new File(fixtures, "min_only.apk")));
        ed.setMinSdk(28);
        File in = build("min_only", ed.toBytes(), "min28", fullExtras());
        File out = new File(scratch, "min28.out.apk");
        ApkRepackager.Result r = ApkRepackager.transform(in, runtime(), out);
        AxmlEditor e = AxmlEditor.parse(readAll(out).get("AndroidManifest.xml"));
        check(r.oldMinSdk == 28 && r.newMinSdk == 28, "min 28 stays 28: " + r.oldMinSdk + "/" + r.newMinSdk);
        check(Integer.valueOf(28).equals(e.minSdk()), "manifest minSdk still 28: " + e.minSdk());
        // targetSdk absent means = minSdk (28): writing the floor 24 would lower it.
        check(e.targetSdk() == null && r.newTargetSdk == 28, "absent target not lowered: " + e.targetSdk() + "/" + r.newTargetSdk);
        check(reportHas(r, "minSdk 28", "unchanged"), "report says min left unchanged: " + r.report);
    }

    static void raisesTarget22() throws IOException {
        // target22 itself is extractNativeLibs=false (refused), so the 22 manifest is min_only's, edited.
        File in = build("min_only", minOnlyManifestWithTarget(22), "t22", fullExtras());
        ApkRepackager.Result r = ApkRepackager.transform(in, runtime(), new File(scratch, "t22.out.apk"));
        check(r.oldTargetSdk == 22 && r.newTargetSdk == 24, "22 -> 24: " + r.oldTargetSdk + "/" + r.newTargetSdk);
    }

    static void neverLowersTarget() throws IOException {
        File in = build("min_only", minOnlyManifestWithTarget(30), "t30", fullExtras());
        File out = new File(scratch, "t30.out.apk");
        ApkRepackager.Result r = ApkRepackager.transform(in, runtime(), out);
        check(r.oldTargetSdk == 30 && r.newTargetSdk == 30, "30 stays 30: " + r.oldTargetSdk + "/" + r.newTargetSdk);
        check(Integer.valueOf(30).equals(AxmlEditor.parse(readAll(out).get("AndroidManifest.xml")).targetSdk()),
                "manifest targetSdk still 30");
    }

    static void armeabiOnlyWorks() throws IOException {
        Map<String, byte[]> x = new LinkedHashMap<>();
        x.put("lib/armeabi/libgumball.so", b("ARM-gum"));
        File in = build("min_only", null, "armonly", x);
        File out = new File(scratch, "armonly.out.apk");
        ApkRepackager.Result r = ApkRepackager.transform(in, runtime(), out);
        Map<String, byte[]> o = readAll(out);
        check(Arrays.equals(o.get("assets/rb32/libgumball.so"), b("ARM-gum")), "armeabi lib moved to assets/rb32");
        check(Arrays.equals(o.get("lib/arm64-v8a/libgumball.so"), STUB), "armeabi lib stubbed");
        check(!anyStartsWith(o, "lib/armeabi"), "no lib/armeabi in output");
        check(!reportHas(r, "libgumball.so", "dropped"), "armeabi-only lib is not reported dropped: " + r.report);
    }

    /** A real 32-bit library and a DEX declaring three of its natives: a generated stub exporting exactly those. */
    static void generatesStubs() throws IOException {
        File so = new File(fixtures, "libjnibridge_fixture.so");
        if (!so.isFile()) { System.out.println("ApkRepackagerTest: no " + so + " — generated-stub row skipped"); return; }
        Map<String, byte[]> x = new LinkedHashMap<>();
        x.put("classes.dex", Files.readAllBytes(new File(fixtures, "JniFixture.dex").toPath()));
        x.put("lib/armeabi-v7a/libjnibridge_fixture.so", Files.readAllBytes(so.toPath()));
        x.put("lib/armeabi-v7a/libplain.so", b("not an ELF"));
        File in = build("min_only", null, "genstub", x);
        File out = new File(scratch, "genstub.out.apk");
        ApkRepackager.Result r = ApkRepackager.transform(in, runtime(), out);
        Map<String, byte[]> o = readAll(out);
        byte[] stub = o.get("lib/arm64-v8a/libjnibridge_fixture.so");
        check(stub != null && !Arrays.equals(stub, STUB) && stub[4] == 2 && stub[18] == (byte) 183,
              "the ELF library gets a generated AArch64 stub");
        String s = stub == null ? "" : new String(stub, java.nio.charset.StandardCharsets.ISO_8859_1);
        check(s.contains("Java_rb_Fixture_mix\0") && s.contains("Java_rb_Fixture_half\0")
              && s.contains("Java_rb_Fixture_echoObject\0") && s.contains("JIJFDIJFDIIIIFFDDFD\0"), "declared natives exported");
        check(!s.contains("Java_rb_Fixture_third") && !s.contains("Java_rb_Fixture_fails"), "undeclared Java_* left out");
        check(reportHas(r, "libjnibridge_fixture.so", "generated stub, 3 Java_*"), "report: " + r.report);
        check(Arrays.equals(o.get("lib/arm64-v8a/libplain.so"), STUB) && reportHas(r, "libplain.so", "prebuilt stub"),
              "an unreadable library keeps the prebuilt stub: " + r.report);
    }

    interface Throwing { void run() throws IOException; }

    static void refuses(String what, Throwing t) {
        try { t.run(); check(false, what + " should have been refused"); }
        catch (IOException e) {
            check(e.getMessage() != null && e.getMessage().startsWith("refused: "), what + " message: " + e.getMessage());
        }
    }

    static void refusals() throws IOException {
        File noLibs = build("min_only", null, "nolibs", new LinkedHashMap<>());
        refuses("no libs", () -> ApkRepackager.transform(noLibs, runtime(), new File(scratch, "nolibs.out.apk")));
        Map<String, byte[]> x = new LinkedHashMap<>();
        x.put("lib/armeabi-v7a/libfoo.so", b("V7A"));
        File ef = build("target22", null, "extractfalse", x);
        refuses("extractNativeLibs=false", () -> ApkRepackager.transform(ef, runtime(), new File(scratch, "ef.out.apk")));
        Map<String, byte[]> y = new LinkedHashMap<>(x);
        y.put("lib/arm64-v8a/libfoo.so", b("already 64"));
        File a64 = build("min_only", null, "has64", y);
        refuses("existing arm64-v8a", () -> ApkRepackager.transform(a64, runtime(), new File(scratch, "a64.out.apk")));
        Map<String, byte[]> z = new LinkedHashMap<>();
        z.put("lib/x86/libfoo.so", b("x86"));
        File x86 = build("min_only", null, "x86only", z);
        refuses("x86-only", () -> ApkRepackager.transform(x86, runtime(), new File(scratch, "x86.out.apk")));
        check(!new File(scratch, "nolibs.out.apk").exists(), "a refusal leaves no output file behind");
    }

    // ---- targetSdk compatibility shims

    static final byte[] SHIMDEX = b("dex\n035\0SHIM-DEX-BYTES");

    static Map<String, byte[]> runtimeWithShims() {
        Map<String, byte[]> m = runtime();
        m.put(ShimPlan.SHIM_DEX_KEY, SHIMDEX);
        return m;
    }

    static String xmltree(File apk) throws IOException, InterruptedException {
        File txt = new File(scratch, apk.getName() + ".xmltree.txt");
        int rc = new ProcessBuilder(aapt2, "dump", "xmltree", "--file", "AndroidManifest.xml", apk.getPath())
                .redirectErrorStream(true).redirectOutput(txt).start().waitFor();
        String t = new String(Files.readAllBytes(txt.toPath()), StandardCharsets.UTF_8);
        check(rc == 0, apk.getName() + ": aapt2 dump xmltree exit " + rc + ": " + t);
        return t;
    }

    static void addsShims() throws Exception {
        byte[] risky = Files.readAllBytes(new File(fixtures, "RiskyFixture.dex").toPath());
        Map<String, byte[]> x = new LinkedHashMap<>();
        x.put("classes.dex", risky);
        x.put("lib/armeabi/libgumball.so", b("ARM-gum"));
        File in = build("min_only", null, "shims", x);
        File out = new File(scratch, "shims.out.apk");
        ApkRepackager.Result r = ApkRepackager.transform(in, runtimeWithShims(), out);
        Map<String, byte[]> o = readAll(out);
        check(Arrays.equals(o.get("classes.dex"), risky), "the game's classes.dex is unchanged");
        check(Arrays.equals(o.get("classes2.dex"), SHIMDEX), "shim dex added as classes2.dex");
        check(!o.containsKey(ShimPlan.SHIM_DEX_KEY), "the shimdex key is not itself an entry");
        check("classes2.dex".equals(r.shimDex), "r.shimDex: " + r.shimDex);
        check(r.shims.equals(Arrays.asList("target-sdk", "strict-network", "file-uri", "permissions")), "r.shims: " + r.shims);
        String cfg = o.containsKey(ShimPlan.CONFIG_ENTRY) ? new String(o.get(ShimPlan.CONFIG_ENTRY), StandardCharsets.UTF_8) : null;
        check(("targetSdk=8\nshims=target-sdk,strict-network,file-uri,permissions\n").equals(cfg), "config: " + cfg);
        AxmlEditor e = AxmlEditor.parse(o.get("AndroidManifest.xml"));
        check("com.retrobug.fixture.rbshim".equals(e.stringAttrOf("provider", ShimPlan.PROVIDER_CLASS, "authorities")),
                "provider authorities: " + e.stringAttrOf("provider", ShimPlan.PROVIDER_CLASS, "authorities"));
        check(e.attrNamesOf("provider", ShimPlan.PROVIDER_CLASS).equals(Arrays.asList("name", "exported", "authorities", "initOrder")),
                "provider attributes in resource-id order: " + e.attrNamesOf("provider", ShimPlan.PROVIDER_CLASS));
        check(Integer.valueOf(24).equals(e.targetSdk()) && Integer.valueOf(24).equals(e.minSdk()), "sdk still raised");
        // aapt2 is the judge of the bytes: the provider must read back with the platform's attribute ids.
        String t = xmltree(out);
        check(t.contains("E: provider"), "aapt2 sees <provider>");
        check(t.contains("name(0x01010003)=\"com.retrobug.shim.RbShimProvider\""), "aapt2: provider name");
        check(t.contains("exported(0x01010010)=false"), "aapt2: exported=false");
        check(t.contains("authorities(0x01010018)=\"com.retrobug.fixture.rbshim\""), "aapt2: authorities");
        check(t.contains("initOrder(0x0101001a)=2147483647"), "aapt2: initOrder");
        check(t.lastIndexOf("E: provider") > t.indexOf("E: activity"), "the shim provider is a child after the activity");
        check(t.contains("name(0x01010003)=\".Main\""), "aapt2: the existing activity name survives the pool shift");
        check(reportHas(r, "shims: [target-sdk", "classes2.dex"), "report names the shims: " + r.report);
        check(reportHas(r, "risk: implicit service", "covered by shim target-sdk"), "report names bindService risk: " + r.report);
        check(r.risks.size() == 3, "three risks from the fixture dex: " + r.risks);

        // A game with classes2.dex already: the shim continues the run as classes3.dex.
        Map<String, byte[]> y = new LinkedHashMap<>(x);
        y.put("classes2.dex", risky);
        File in2 = build("min_only", null, "shims2", y);
        ApkRepackager.Result r2 = ApkRepackager.transform(in2, runtimeWithShims(), new File(scratch, "shims2.out.apk"));
        Map<String, byte[]> o2 = readAll(new File(scratch, "shims2.out.apk"));
        check("classes3.dex".equals(r2.shimDex) && Arrays.equals(o2.get("classes3.dex"), SHIMDEX)
                && Arrays.equals(o2.get("classes2.dex"), risky), "shim dex goes to classes3.dex: " + r2.shimDex);
    }

    /** The Data tab's save provider: exported (the launcher asks it), beside the unexported shim provider. */
    static void addsSaveProvider() throws Exception {
        Map<String, byte[]> x = new LinkedHashMap<>();
        x.put("classes.dex", Files.readAllBytes(new File(fixtures, "RiskyFixture.dex").toPath()));
        x.put("lib/armeabi/libgumball.so", b("ARM-gum"));
        File in = build("min_only", null, "saves", x);
        File out = new File(scratch, "saves.out.apk");
        Map<String, byte[]> rt = runtimeWithShims();
        rt.put(ShimPlan.LAUNCHER_CERT_KEY, b("ab12cd"));
        ApkRepackager.transform(in, rt, out);
        Map<String, byte[]> o = readAll(out);
        String cfg = new String(o.get(ShimPlan.CONFIG_ENTRY), StandardCharsets.UTF_8);
        check(cfg.endsWith("\nlauncherCert=ab12cd\n"), "config records the launcher's certificate: " + cfg);
        check(!o.containsKey(ShimPlan.LAUNCHER_CERT_KEY), "the launchercert key is not itself an entry");
        AxmlEditor e = AxmlEditor.parse(o.get("AndroidManifest.xml"));
        check("com.retrobug.fixture.rbsaves".equals(e.stringAttrOf("provider", ShimPlan.SAVES_PROVIDER_CLASS, "authorities")),
                "save provider authorities: " + e.stringAttrOf("provider", ShimPlan.SAVES_PROVIDER_CLASS, "authorities"));
        String t = xmltree(out);
        check(t.contains("name(0x01010003)=\"com.retrobug.shim.RbSaveProvider\""), "aapt2: save provider name");
        int save = t.indexOf("com.retrobug.shim.RbSaveProvider");
        check(save > 0 && t.indexOf("exported(0x01010010)=true", save) > save, "aapt2: the save provider is exported");
        check(t.contains("exported(0x01010010)=false"), "aapt2: the shim provider stays unexported");
        // Without the launcher's certificate (a launcher build that could not read it) no save provider is added.
        File out2 = new File(scratch, "saves2.out.apk");
        ApkRepackager.transform(in, runtimeWithShims(), out2);
        check(AxmlEditor.parse(readAll(out2).get("AndroidManifest.xml"))
                .stringAttrOf("provider", ShimPlan.SAVES_PROVIDER_CLASS, "authorities") == null, "no certificate: no save provider");
    }

    /** A protection wrapper's ELF shipped as an asset (SpongeBob's assets/ATG_E.sec, dlopen'd from files/) is repaired too. */
    static void repairsElfAssets() throws Exception {
        byte[] good = ElfRepairTest.elf(), broken = good.clone();
        broken[46] = 0; broken[47] = 0; broken[50] = 0; broken[51] = 0;
        Map<String, byte[]> x = new LinkedHashMap<>();
        x.put("lib/armeabi-v7a/libfoo.so", b("V7A"));
        x.put("assets/ATG_E.sec", broken);
        x.put("assets/notelf.bin", b("plain data"));
        File in = build("min_only", null, "elfassets", x);
        File out = new File(scratch, "elfassets.out.apk");
        ApkRepackager.Result r = ApkRepackager.transform(in, runtime(), out);
        Map<String, byte[]> o = readAll(out);
        check(Arrays.equals(o.get("assets/ATG_E.sec"), good), "the ELF asset's header fields are restored");
        check(Arrays.equals(o.get("assets/notelf.bin"), b("plain data")), "other assets are untouched");
        check(reportHas(r, "elf: assets/ATG_E.sec", "restored"), "the report names the repaired asset: " + r.report);
    }

    static void shimsWhenNotNeededOrImpossible() throws Exception {
        Map<String, byte[]> x = new LinkedHashMap<>();
        x.put("classes.dex", Files.readAllBytes(new File(fixtures, "RiskyFixture.dex").toPath()));
        x.put("lib/armeabi-v7a/libfoo.so", b("V7A"));
        // Target 30 is left at 30: no behaviour change, so no shim and no risk lines — but the shim provider still
        // comes, with an empty shim list: it is also how the game reads its RetroBug settings at start.
        File t30 = build("min_only", minOnlyManifestWithTarget(30), "shims30", x);
        ApkRepackager.Result r = ApkRepackager.transform(t30, runtimeWithShims(), new File(scratch, "shims30.out.apk"));
        Map<String, byte[]> o = readAll(new File(scratch, "shims30.out.apk"));
        String cfg30 = o.containsKey(ShimPlan.CONFIG_ENTRY) ? new String(o.get(ShimPlan.CONFIG_ENTRY), StandardCharsets.UTF_8) : null;
        check(Arrays.equals(o.get("classes2.dex"), SHIMDEX) && cfg30 != null && cfg30.contains("shims=\n"),
                "target 30: the shim dex and an empty shim list: " + cfg30);
        AxmlEditor m30 = AxmlEditor.parse(o.get("AndroidManifest.xml"));
        check(ShimPlan.PROVIDER_CLASS.equals(m30.stringAttrOf("provider", ShimPlan.PROVIDER_CLASS, "name")), "target 30: the provider");
        check(!m30.addQueriesProvider(SETTINGS_AUTHORITY), "target 30: the launcher's settings provider is declared under <queries>");
        check(r.shims.isEmpty() && r.risks.isEmpty() && reportHas(r, "shims: none needed", "30"), "target 30 report: " + r.report);
        // A launcher build without the shim dex: named, nothing half-added.
        File m = build("min_only", null, "noshimdex", x);
        ApkRepackager.Result r2 = ApkRepackager.transform(m, runtime(), new File(scratch, "noshimdex.out.apk"));
        Map<String, byte[]> o2 = readAll(new File(scratch, "noshimdex.out.apk"));
        check(reportHas(r2, "shims: NOT added", "no shim dex"), "missing shim dex reported: " + r2.report);
        check(!o2.containsKey("classes2.dex") && !o2.containsKey(ShimPlan.CONFIG_ENTRY)
                && AxmlEditor.parse(o2.get("AndroidManifest.xml")).attrNames("provider").isEmpty(), "missing shim dex: nothing added");
        check(reportHas(r2, "risk: implicit service", "NOT covered"), "risk says not covered without shims: " + r2.report);
        // hasCode="false": no Java runs in the process, so a provider would never load.
        Map<String, byte[]> z = new LinkedHashMap<>();
        z.put("lib/armeabi-v7a/libnative.so", b("V7A"));
        File nc = build("no_code", null, "nocode", z);
        ApkRepackager.Result r3 = ApkRepackager.transform(nc, runtimeWithShims(), new File(scratch, "nocode.out.apk"));
        Map<String, byte[]> o3 = readAll(new File(scratch, "nocode.out.apk"));
        check(reportHas(r3, "shims: NOT added", "hasCode"), "hasCode=false reported: " + r3.report);
        check(!o3.containsKey("classes.dex") && !o3.containsKey(ShimPlan.CONFIG_ENTRY), "hasCode=false: no dex added");
    }

    static String dumpResources(File apk) throws IOException, InterruptedException {
        File txt = new File(scratch, apk.getName() + ".resources.txt");
        int rc = new ProcessBuilder(aapt2, "dump", "resources", apk.getPath())
                .redirectErrorStream(true).redirectOutput(txt).start().waitFor();
        String t = new String(Files.readAllBytes(txt.toPath()), StandardCharsets.UTF_8);
        check(rc == 0, apk.getName() + ": aapt2 dump resources exit " + rc + ": " + t);
        return t;
    }

    static void renamesPackage() throws Exception {
        Map<String, byte[]> x = new LinkedHashMap<>();
        x.put("classes.dex", Files.readAllBytes(new File(fixtures, "RiskyFixture.dex").toPath()));
        x.put("lib/armeabi-v7a/libfoo.so", b("V7A-foo"));
        File in = build("rename", null, "rename", x);
        File out = new File(scratch, "rename.out.apk");
        ApkRepackager.Result r = ApkRepackager.transform(in, runtimeWithShims(), out, ApkRepackager.RENAME_SUFFIX);
        check("com.retrobug.fixture.rb".equals(r.packageName) && "com.retrobug.fixture".equals(r.renamedFrom),
                "rename: result names both packages: " + r.packageName + " / " + r.renamedFrom);
        check(reportHas(r, "package com.retrobug.fixture -> com.retrobug.fixture.rb", "beside the original"), "rename reported: " + r.report);
        check(reportHas(r, "rename: activity name: .Main", "com.retrobug.fixture.Main"), "class expansion reported: " + r.report);
        check(reportHas(r, "resources.arsc", "package name -> com.retrobug.fixture.rb"), "arsc rename reported: " + r.report);
        String t = xmltree(out);
        check(t.contains("package=\"com.retrobug.fixture.rb\""), "aapt2: output package is renamed: " + t);
        check(t.contains("name(0x01010003)=\"com.retrobug.fixture.Main\"") && !t.contains("\".Main\""),
                "aapt2: activity expanded against the original package");
        check(t.contains("authorities(0x01010018)=\"com.retrobug.fixture.rb.files;com.facebook.app.FacebookContentProvider123.rb\""),
                "aapt2: game provider authorities renamed");
        // The shim provider is added after the rename, so its authority is under the new package.
        check(t.contains("authorities(0x01010018)=\"com.retrobug.fixture.rb.rbshim\""), "aapt2: shim authority under the new package");
        check(t.contains("name(0x01010003)=\"com.retrobug.fixture.rb.permission.C2D_MESSAGE\""), "aapt2: declared permission renamed");
        check(t.contains("targetSdkVersion(0x01010270)=24"), "aapt2: targetSdk still raised");
        String res = dumpResources(out);
        check(res.contains("Package name=com.retrobug.fixture.rb id=7f") && res.contains("string/app_name"),
                "aapt2: resources.arsc package renamed: " + res);
        check(methodOf(out, "resources.arsc") == ZipEntry.STORED, "renamed resources.arsc stays STORED");

        // Without a rename: arsc byte-identical, package unchanged, no rename lines.
        File plain = new File(scratch, "rename.plain.apk");
        ApkRepackager.Result p = ApkRepackager.transform(in, runtimeWithShims(), plain);
        check(p.renamedFrom == null && "com.retrobug.fixture".equals(p.packageName), "no rename: package kept");
        check(Arrays.equals(readAll(plain).get("resources.arsc"), readAll(in).get("resources.arsc")), "no rename: arsc untouched");
        check(!reportHas(p, "rename", "") && !reportHas(p, "resources.arsc", ""), "no rename: no rename lines: " + p.report);

        // A full new name; a bad one is refused and leaves no output behind.
        File full = new File(scratch, "rename.full.apk");
        ApkRepackager.Result f = ApkRepackager.transform(in, runtimeWithShims(), full, "org.example.side");
        check("org.example.side".equals(f.packageName) && xmltree(full).contains("package=\"org.example.side\""), "full rename");
        File bad = new File(scratch, "rename.bad.apk");
        try {
            ApkRepackager.transform(in, runtimeWithShims(), bad, "not a package");
            check(false, "bad rename should throw");
        } catch (IOException ex) {
            check(!bad.exists(), "bad rename leaves no output");
        }

        // renameArscPackage: only a package chunk with exactly the old name; a non-table is left alone.
        byte[] arsc = readAll(in).get("resources.arsc");
        check(ApkRepackager.renameArscPackage(arsc.clone(), "com.retrobug.fix", "x.y") == 0, "arsc: no prefix match");
        check(ApkRepackager.renameArscPackage(b("not a resource table"), "a.b", "c.d") == 0, "arsc: garbage left alone");
    }

    static String aapt2;

    /** Targeting API 30+ refuses a compressed resources.arsc (Where's My Mickey ships one): the output stores it. */
    static void storesCompressedArsc() throws IOException {
        File plain = build("min_only", null, "arsc-src", fullExtras());
        File in = new File(scratch, "arsc-deflated.apk");
        try (ZipFile src = new ZipFile(plain); ZipOutputStream zo = new ZipOutputStream(Files.newOutputStream(in.toPath()))) {
            for (Enumeration<? extends ZipEntry> en = src.entries(); en.hasMoreElements(); ) {
                ZipEntry se = en.nextElement();
                byte[] data;
                try (InputStream s = src.getInputStream(se)) { data = s.readAllBytes(); }
                zo.putNextEntry(new ZipEntry(se.getName()));  // everything DEFLATED, resources.arsc included
                zo.write(data);
                zo.closeEntry();
            }
        }
        check(methodOf(in, "resources.arsc") == ZipEntry.DEFLATED, "fixture precondition: resources.arsc is DEFLATED");
        File out = new File(scratch, "arsc-deflated.out.apk");
        ApkRepackager.transform(in, runtime(), out);
        try (ZipFile z = new ZipFile(out); ZipFile p = new ZipFile(plain)) {
            check(z.getEntry("resources.arsc").getMethod() == ZipEntry.STORED, "compressed resources.arsc is written STORED");
            byte[] want, got;
            try (InputStream s = p.getInputStream(p.getEntry("resources.arsc"))) { want = s.readAllBytes(); }
            try (InputStream s = z.getInputStream(z.getEntry("resources.arsc"))) { got = s.readAllBytes(); }
            check(Arrays.equals(want, got), "resources.arsc bytes unchanged");
        }
    }

    /** The aapt2-built fixture's binary manifest. */
    static byte[] fixture(String base) throws IOException {
        try (ZipFile z = new ZipFile(new File(fixtures, base + ".apk"));
             InputStream in = z.getInputStream(z.getEntry("AndroidManifest.xml"))) {
            return in.readAllBytes();
        }
    }

    /**
     * A converted copy carries the runtime stamp the library reads back (Ready vs Update available) as
     * <meta-data android:name="com.retrobug.runtime" android:value=N/> — never in its versionName, which games read
     * (SMB Bounce loads assets/dex/main-<versionName>.jar).
     */
    static void stampsRuntime() throws Exception {
        File in = build("min_only", null, "stamp-none", fullExtras());
        File out = new File(scratch, "stamp-none.out.apk");
        ApkRepackager.transform(in, runtime(), out);
        AxmlEditor none = AxmlEditor.parse(readAll(out).get("AndroidManifest.xml"));
        check(none.versionName() == null, "no versionName stays absent: " + none.versionName());
        check(Integer.valueOf(ApkRepackager.RUNTIME_STAMP).equals(none.metaDataInt(ApkRepackager.RUNTIME_META)),
                "runtime meta-data: " + none.metaDataInt(ApkRepackager.RUNTIME_META));
        AxmlEditor ed = AxmlEditor.parse(fixture("min_only"));
        ed.setVersionName("1.2.12");
        File in2 = build("min_only", ed.toBytes(), "stamp-25", fullExtras());
        File out2 = new File(scratch, "stamp-25.out.apk");
        ApkRepackager.transform(in2, runtime(), out2);
        AxmlEditor v = AxmlEditor.parse(readAll(out2).get("AndroidManifest.xml"));
        check("1.2.12".equals(v.versionName()), "versionName unchanged: " + v.versionName());
        check(Integer.valueOf(ApkRepackager.RUNTIME_STAMP).equals(v.metaDataInt(ApkRepackager.RUNTIME_META)), "stamped");
        String t = xmltree(out2);
        check(t.contains("E: meta-data") && t.contains("name(0x01010003)=\"com.retrobug.runtime\"")
                && t.contains("value(0x01010024)=" + ApkRepackager.RUNTIME_STAMP), "aapt2 reads the meta-data: " + t);
        check(ApkRepackager.RUNTIME_STAMP >= 2, "stamp raised past the versionName-stamped copies (rb1)");
    }

    public static void main(String[] a) throws Exception {
        fixtures = new File(a[0]); scratch = new File(a[1]); aapt2 = a[2];
        addsShims();
        addsSaveProvider();
        repairsElfAssets();
        shimsWhenNotNeededOrImpossible();
        renamesPackage();
        movesV7aAndStubs();
        reportsDroppedAndUnstubbed();
        bumpsTarget();
        raisesAbsentMin();
        neverLowersMin();
        raisesTarget22();
        neverLowersTarget();
        armeabiOnlyWorks();
        generatesStubs();
        refusals();
        storesCompressedArsc();
        stampsRuntime();
        System.out.println(checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
