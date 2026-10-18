// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// A mod (M6): replacement or extra assets for a game, applied when its .rb copy is made (texture packs, music,
// translations). A mod is a .zip with mod.properties (id, title, optional description/game/versionCode) and the
// files under files/, each at the APK path it replaces: files/assets/sky.png -> assets/sky.png. Only assets/ and res/
// may be replaced or added; code (classes*.dex, lib/), the manifest, resources.arsc and signatures never are. The
// zip stays where it is; its files are read when the copy is made. Pure Java (host tested).
package com.retrobug.launcher;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Enumeration;
import java.util.List;
import java.util.Properties;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class Mod {
    static final String PROPS = "mod.properties";
    static final String FILES = "files/";

    String id, title, description = "", game;
    long versionCode = -1;
    final File zip;
    private final List<String> paths = new ArrayList<>();
    private long sizeSum;

    private Mod(File zip) { this.zip = zip; }

    /** Opens and checks a mod file; IllegalArgumentException (in the player's words) when it is not a usable mod. */
    static Mod open(File f) {
        Mod m = new Mod(f);
        try (ZipFile z = new ZipFile(f)) {
            ZipEntry pe = z.getEntry(PROPS);
            if (pe == null) throw new IllegalArgumentException("This isn't a RetroBug mod (it has no " + PROPS + ")");
            Properties p = new Properties();
            try (InputStream in = z.getInputStream(pe)) { p.load(new java.io.InputStreamReader(in, StandardCharsets.UTF_8)); }
            m.id = p.getProperty("id", "").trim();
            m.title = p.getProperty("title", "").trim();
            m.description = p.getProperty("description", "").trim();
            m.game = p.getProperty("game");
            try {
                m.versionCode = Long.parseLong(p.getProperty("versionCode", "-1").trim());
            } catch (NumberFormatException e) {
                m.versionCode = -1;
            }
            if (!m.id.matches("[a-z0-9][a-z0-9._-]*") || m.id.contains(".."))
                throw new IllegalArgumentException("The mod's id must be lower-case letters, digits, '-', '_', '.'");
            if (m.title.isEmpty()) throw new IllegalArgumentException("The mod has no title");
            for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement();
                String n = e.getName();
                if (e.isDirectory() || !n.startsWith(FILES)) continue;
                String path = n.substring(FILES.length());
                String why = refuse(path);
                if (why != null) throw new IllegalArgumentException("The mod would change " + path + ": " + why);
                m.paths.add(path);
                m.sizeSum += Math.max(0, e.getSize());
            }
        } catch (IOException e) {
            throw new IllegalArgumentException("This isn't a readable .zip (" + e.getMessage() + ")");
        }
        if (m.paths.isEmpty()) throw new IllegalArgumentException("The mod has no files under " + FILES);
        Collections.sort(m.paths);
        return m;
    }

    /** Why a mod may not write `path`, or null when it may. */
    static String refuse(String path) {
        if (path.isEmpty() || path.startsWith("/") || path.contains("\\") || path.contains(":")) return "not a plain path";
        for (String part : path.split("/")) if (part.equals("..") || part.equals(".") || part.isEmpty()) return "not a plain path";
        if (!path.startsWith("assets/") && !path.startsWith("res/")) return "mods can only replace assets/ and res/ files";
        return null;
    }

    /** The APK paths this mod replaces or adds, sorted. */
    List<String> files() { return Collections.unmodifiableList(paths); }

    long size() { return sizeSum; }

    /** The mod's file for APK path `path`. */
    byte[] read(String path) throws IOException {
        try (ZipFile z = new ZipFile(zip)) {
            ZipEntry e = z.getEntry(FILES + path);
            if (e == null) throw new IOException("no " + path + " in " + zip.getName());
            try (InputStream in = z.getInputStream(e)) {
                ByteArrayOutputStream o = new ByteArrayOutputStream();
                byte[] b = new byte[1 << 16];
                for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n);
                return o.toByteArray();
            }
        }
    }

    /** The mods whose ids are listed in `csv`, in id order (a later mod wins where two replace the same file). */
    static List<Mod> enabled(List<Mod> all, String csv) {
        List<String> ids = new ArrayList<>();
        for (String s : csv == null ? new String[0] : csv.split(",")) if (!s.trim().isEmpty()) ids.add(s.trim());
        List<Mod> out = new ArrayList<>();
        for (Mod m : all) if (ids.contains(m.id)) out.add(m);
        Collections.sort(out, new Comparator<Mod>() {
            @Override public int compare(Mod x, Mod y) { return x.id.compareTo(y.id); }
        });
        return out;
    }

    /** A short fingerprint of a set of mods (ids, files, sizes and zip modification times); "" for none. */
    static String fingerprint(List<Mod> set) {
        if (set.isEmpty()) return "";
        StringBuilder s = new StringBuilder();
        for (Mod m : set) s.append(m.id).append('|').append(m.paths).append('|').append(m.sizeSum).append('|')
                .append(m.zip.length()).append('|').append(m.zip.lastModified()).append('\n');
        return Patch.sha256(s.toString().getBytes(StandardCharsets.UTF_8)).substring(0, 12);
    }
}
