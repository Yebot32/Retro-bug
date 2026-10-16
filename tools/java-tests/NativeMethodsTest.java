// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for NativeMethods: the JNI names of a d8-built fixture's natives (every mangling case), shorties,
// and — when the human's game files are present (build/games/gumball-lite.apk, angry-birds-go-1.10.1.apk; never committed: copyrighted) — the natives of Gumball Lite and
// Angry Birds Go matched against the Java_* names their 32-bit libraries export (12 and 92, as RBRUNTIME reported).
//
// args: <fixture dir holding NativeFixture.dex> [<dir holding the two game APKs>]
package com.retrobug.launcher;

import java.io.File;
import java.io.InputStream;
import java.nio.file.Files;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class NativeMethodsTest {
    static int checks = 0, failures = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static void eq(Map<String, String> m, String name, String desc) {
        check(desc.equals(m.get(name)), name + " -> " + m.get(name) + " (want " + desc + ")");
    }

    static void fixture(File dir) throws Exception {
        byte[] dex = Files.readAllBytes(new File(dir, "NativeFixture.dex").toPath());
        Map<String, String> m = NativeMethods.of(dex);
        final String p = "Java_rb_fx_NativeFixture_";
        eq(m, p + "a", "()V");
        eq(m, p + "a__", "()V");
        eq(m, p + "b", "(IJ)I");
        eq(m, p + "b__IJ", "(IJ)I");
        eq(m, p + "c_1d", "(Ljava/lang/String;[I[[Ljava/lang/Object;)Ljava/lang/String;");
        eq(m, p + "c_1d__Ljava_lang_String_2_3I_3_3Ljava_lang_Object_2",
           "(Ljava/lang/String;[I[[Ljava/lang/Object;)Ljava/lang/String;");
        check(!m.containsKey(p + "over"), "an overloaded native has no short name: " + m.keySet());
        eq(m, p + "over__I", "(I)V");
        eq(m, p + "over__Ljava_lang_String_2", "(Ljava/lang/String;)V");
        eq(m, p + "caf_000e9", "()V");
        eq(m, "Java_rb_fx_NativeFixture_00024Inner_z", "()Z");
        check(!m.containsKey(p + "notNative"), "a non-native method is not listed");
        check(m.size() == 14, "14 names, got " + m.size() + ": " + m.keySet());
        Map<String, NativeMethods.Native> n = NativeMethods.natives(dex);
        check(n.get(p + "a").isStatic && !n.get(p + "b").isStatic, "static from the access flags");

        check(NativeMethods.shortyOf("(IJFDZBCS)V").equals("VIJFDZBCS"), "primitive shorty");
        check(NativeMethods.shortyOf("(Ljava/lang/String;[I[[Ljava/lang/Object;)Ljava/lang/String;").equals("LLLL"),
              "reference and array shorty: " + NativeMethods.shortyOf("(Ljava/lang/String;[I[[Ljava/lang/Object;)Ljava/lang/String;"));
        check(NativeMethods.shortyOf("()[J").equals("L"), "array return is L");
        try { NativeMethods.of(new byte[0x80]); check(false, "non-DEX accepted"); }
        catch (java.io.IOException e) { check(e.getMessage().contains("not a DEX"), "non-DEX refused: " + e.getMessage()); }
    }

    /** The natives the DEX declares that the library exports, by name. */
    static Set<String> bound(Map<String, String> natives, byte[] lib) throws Exception {
        Set<String> out = new TreeSet<>(StubElfWriter.jniExportsOf(lib));
        out.retainAll(natives.keySet());
        return out;
    }

    static byte[] entry(ZipFile z, String name) throws Exception {
        ZipEntry e = z.getEntry(name);
        if (e == null) throw new java.io.IOException(z.getName() + " has no " + name);
        try (InputStream in = z.getInputStream(e)) { return in.readAllBytes(); }
    }

    /** Read in memory from the APKs, as the repackager does. */
    static void games(File dir) throws Exception {
        File gf = new File(dir, "gumball-lite.apk"), af = new File(dir, "angry-birds-go-1.10.1.apk");
        if (!gf.isFile() || !af.isFile()) {
            System.out.println("NativeMethodsTest: game APKs not in " + dir + " — game rows skipped");
            return;
        }
        try (ZipFile gz = new ZipFile(gf); ZipFile az = new ZipFile(af)) {
            Map<String, String> g = NativeMethods.of(entry(gz, "classes.dex"));
            eq(g, "Java_com_twentysixdigital_gumballjava_JNIGLSurface_jniClearTextures", "()V");
            Map<String, String> a = NativeMethods.of(entry(az, "classes.dex"));
            eq(a, "Java_com_rovio_angrybirdsgo_AngryBirdsGoActivity_isDeviceAmazon", "()Z");
            Set<String> gb = bound(g, entry(gz, "lib/armeabi/libgumball.so"));
            Set<String> ab = bound(a, entry(az, "lib/armeabi-v7a/libABK.so"));
            check(gb.size() == 12, "Gumball: " + gb.size() + " bound natives");
            check(ab.size() == 92, "ABGo: " + ab.size() + " bound natives");
        }
    }

    public static void main(String[] a) throws Exception {
        fixture(new File(a[0]));
        if (a.length > 1) games(new File(a[1]));
        System.out.println("NativeMethodsTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
