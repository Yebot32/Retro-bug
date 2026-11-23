// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for Mod (M6): a mod is a .zip with mod.properties and replacement files under files/, mapped to the
// game's APK paths (files/assets/x.png -> assets/x.png). Only assets/ and res/ may be replaced or added — never code
// (classes*.dex, lib/), the manifest, resources.arsc or signatures — and nothing outside them.
package com.retrobug.launcher;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public class ModTest {
    static int checks = 0, failures = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static File zip(File dir, String name, String... nameThenText) throws Exception {
        File f = new File(dir, name);
        try (ZipOutputStream z = new ZipOutputStream(new FileOutputStream(f))) {
            for (int i = 0; i < nameThenText.length; i += 2) {
                z.putNextEntry(new ZipEntry(nameThenText[i]));
                z.write(nameThenText[i + 1].getBytes(StandardCharsets.UTF_8));
                z.closeEntry();
            }
        }
        return f;
    }

    static final String PROPS = "id=hd-sky\ntitle=HD sky\ndescription=Sharper sky textures\ngame=com.example.game\n";

    public static void main(String[] a) throws Exception {
        File tmp = Files.createTempDirectory("rbmod").toFile();
        File good = zip(tmp, "good.zip", "mod.properties", PROPS, "files/assets/sky.png", "SKY", "files/res/raw/theme.ogg",
                "OGG", "readme.txt", "ignored");
        Mod m = Mod.open(good);
        check(m.id.equals("hd-sky") && m.title.equals("HD sky") && m.game.equals("com.example.game"), "fields");
        check(m.files().equals(java.util.Arrays.asList("assets/sky.png", "res/raw/theme.ogg")), "mapped paths: " + m.files());
        check(new String(m.read("assets/sky.png"), StandardCharsets.UTF_8).equals("SKY"), "reads a file by its APK path");

        String[][] bad = {
                {"mod.properties", PROPS, "files/classes.dex", "x"},
                {"mod.properties", PROPS, "files/lib/armeabi-v7a/libgame.so", "x"},
                {"mod.properties", PROPS, "files/AndroidManifest.xml", "x"},
                {"mod.properties", PROPS, "files/resources.arsc", "x"},
                {"mod.properties", PROPS, "files/META-INF/CERT.SF", "x"},
                {"mod.properties", PROPS, "files/assets/../classes.dex", "x"},
                {"mod.properties", PROPS, "files/other/thing.bin", "x"},
                {"files/assets/sky.png", "SKY"},
                {"mod.properties", "title=No id\n", "files/assets/a", "x"},
                {"mod.properties", PROPS},
        };
        int k = 0;
        for (String[] b : bad) {
            File f = zip(tmp, "bad" + (k++) + ".zip", b);
            try {
                Mod.open(f);
                check(false, "refused: " + String.join(" ", b).replace("\n", "|"));
            } catch (IllegalArgumentException e) {
                check(true, "refused");
            }
        }
        File notZip = new File(tmp, "x.zip");
        Files.write(notZip.toPath(), new byte[] {1, 2, 3});
        try {
            Mod.open(notZip);
            check(false, "a non-zip is refused");
        } catch (IllegalArgumentException e) {
            check(true, "a non-zip is refused");
        }

        Mod n = Mod.open(zip(tmp, "n.zip", "mod.properties", "id=a-first\ntitle=A\n", "files/assets/sky.png", "A-SKY"));
        java.util.List<Mod> on = Mod.enabled(java.util.Arrays.asList(m, n), "hd-sky,a-first");
        check(on.size() == 2 && on.get(0).id.equals("a-first"), "enabled in id order");
        check(!Mod.fingerprint(on).isEmpty() && Mod.fingerprint(java.util.Collections.<Mod>emptyList()).isEmpty(), "fingerprint");

        for (File f : tmp.listFiles()) f.delete();
        tmp.delete();
        System.out.println("ModTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
