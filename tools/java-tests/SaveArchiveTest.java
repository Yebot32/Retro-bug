// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for SaveArchive, the converted copy's save export/import: what goes into a save zip, what an import is
// allowed to write (nothing outside the game's own folders, nothing from another game), and who may ask for it.
package com.retrobug.shim;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public class SaveArchiveTest {
    static int checks = 0, failures = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static void put(File f, String text) throws IOException {
        f.getParentFile().mkdirs();
        try (FileOutputStream o = new FileOutputStream(f)) { o.write(text.getBytes(StandardCharsets.UTF_8)); }
    }

    static String read(File f) throws IOException { return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); }

    static Map<String, File> dirs(File root) {
        Map<String, File> m = new LinkedHashMap<>();
        for (String s : SaveArchive.SECTIONS) m.put(s, new File(root, s));
        return m;
    }

    static byte[] zip(String... nameThenText) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(b)) {
            for (int i = 0; i < nameThenText.length; i += 2) {
                z.putNextEntry(new ZipEntry(nameThenText[i]));
                z.write(nameThenText[i + 1].getBytes(StandardCharsets.UTF_8));
                z.closeEntry();
            }
        }
        return b.toByteArray();
    }

    static void rm(File f) {
        File[] k = f.listFiles();
        if (k != null) for (File c : k) rm(c);
        f.delete();
    }

    public static void main(String[] a) throws Exception {
        File tmp = Files.createTempDirectory("rbsave").toFile();
        try {
            // A game's saves: files, prefs, a database, external files; RetroBug's own settings file is not a save.
            File game = new File(tmp, "game");
            Map<String, File> g = dirs(game);
            put(new File(g.get("files"), "save/slot1.dat"), "level 7");
            put(new File(g.get("files"), SaveArchive.SETTINGS_FILE), "cpu=safe");
            put(new File(g.get("shared_prefs"), "game.xml"), "<map><int name=\"coins\" value=\"900\"/></map>");
            put(new File(g.get("databases"), "progress.db"), "db");
            put(new File(g.get("external"), "cloud.json"), "{}");

            ByteArrayOutputStream out = new ByteArrayOutputStream();
            int n = SaveArchive.write("com.example.game", g, out);
            check(n == 4, "4 save files written (rb-settings left out), got " + n);

            // Import into a fresh install: same files back; the fresh copy's own settings file stays.
            File fresh = new File(tmp, "fresh");
            Map<String, File> f = dirs(fresh);
            put(new File(f.get("files"), "save/slot1.dat"), "level 1");
            put(new File(f.get("files"), "stale.tmp"), "old");
            put(new File(f.get("files"), SaveArchive.SETTINGS_FILE), "cpu=fast");
            File staging = new File(tmp, "staging");
            String why = SaveArchive.unpack(new ByteArrayInputStream(out.toByteArray()), staging, "com.example.game");
            check(why == null, "a save of this game unpacks: " + why);
            SaveArchive.replace(f, staging);
            check(read(new File(f.get("files"), "save/slot1.dat")).equals("level 7"), "slot restored");
            check(!new File(f.get("files"), "stale.tmp").exists(), "files the save did not have are gone");
            check(read(new File(f.get("files"), SaveArchive.SETTINGS_FILE)).equals("cpu=fast"), "RetroBug's settings file kept");
            check(read(new File(f.get("shared_prefs"), "game.xml")).contains("900"), "prefs restored");
            check(read(new File(f.get("databases"), "progress.db")).equals("db"), "database restored");
            check(read(new File(f.get("external"), "cloud.json")).equals("{}"), "external files restored");

            // Refusals: another game's save, no manifest, a path that climbs out, an unknown section, an absolute path.
            rm(staging);
            check(SaveArchive.unpack(new ByteArrayInputStream(out.toByteArray()), staging, "com.other.game") != null,
                    "another game's save is refused");
            rm(staging);
            check(SaveArchive.unpack(new ByteArrayInputStream(zip("files/a", "x")), staging, "com.example.game") != null,
                    "a zip with no RetroBug manifest is refused");
            String manifest = "package=com.example.game\n";
            String[] bad = {"files/../../evil", "files/a/../../../evil", "cache/x", "/files/abs", "files\\..\\evil", "files/"
                    + SaveArchive.SETTINGS_FILE};
            for (String b : bad) {
                rm(staging);
                String r = SaveArchive.unpack(new ByteArrayInputStream(zip(SaveArchive.MANIFEST, manifest, b, "x")), staging,
                        "com.example.game");
                check(r != null, "refused: " + b);
                check(!new File(tmp, "evil").exists(), "nothing written outside staging for " + b);
            }

            // OBB file names the copy will write into its own OBB folder for the launcher: a plain .obb name only.
            check(SaveArchive.obbName("main.102120000.com.sega.smbbounce.rb.obb"), "an OBB name");
            check(SaveArchive.obbName("patch.3.a.b.rb.OBB"), "case of .OBB");
            for (String b : new String[] {"../x.obb", "a/b.obb", "a\\b.obb", ".obb", "x.apk", "", null, "..obb", "x.obb\u0000"}) {
                check(!SaveArchive.obbName(b), "not an OBB name: " + b);
            }

            // Who may ask: the launcher's package, signed with the certificate the copy was made with.
            byte[] cert = "launcher-cert".getBytes(StandardCharsets.UTF_8);
            StringBuilder hex = new StringBuilder();
            for (byte x : MessageDigest.getInstance("SHA-256").digest(cert)) hex.append(String.format("%02x", x));
            check(SaveArchive.trusted("com.retrobug.launcher", Collections.singletonList(cert), hex.toString()), "the launcher");
            check(!SaveArchive.trusted("com.evil.app", Collections.singletonList(cert), hex.toString()), "another package");
            check(!SaveArchive.trusted("com.retrobug.launcher", Collections.singletonList("other".getBytes()), hex.toString()),
                    "the launcher's name with another key");
            check(!SaveArchive.trusted("com.retrobug.launcher", Collections.singletonList(cert), ""), "no recorded certificate");
            check(!SaveArchive.trusted(null, Collections.singletonList(cert), hex.toString()), "no caller");
        } finally {
            rm(tmp);
        }
        System.out.println("SaveArchiveTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
