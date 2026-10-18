// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// A converted game's saves as one zip, for the RetroBug launcher's Data tab: the game's private files, preferences and
// databases, and its files on shared storage (Android/data/<pkg>/files). An import unpacks into a staging folder first
// and is refused whole when anything in it is wrong — a save of another game, no RetroBug manifest, or a path outside
// the four folders — so a bad file never leaves a game half-overwritten. RetroBug's own settings file is never part of
// a save. Pure Java (no Android), so the host tests pin it; RbSaveProvider is the Android side.
package com.retrobug.shim;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

final class SaveArchive {
    private SaveArchive() {}

    static final String MANIFEST = "rbsave.properties";
    static final String SETTINGS_FILE = "rb-settings.properties";
    static final String[] SECTIONS = {"files", "shared_prefs", "databases", "external"};
    static final String LAUNCHER = "com.retrobug.launcher";

    /** RetroBug's own files, which a save neither carries nor replaces. */
    static boolean ours(String section, String rel) {
        return section.equals("files") && (rel.equals(SETTINGS_FILE) || rel.equals(REPORT_FILE));
    }

    /** The native runtime's boot report (runtime_boot.cpp), rewritten at every start: not a save. */
    static final String REPORT_FILE = "rb-report.txt";

    /**
     * {@link #write} into `target` through `target`.tmp, renamed over `target` only when complete: a failed write
     * leaves the previous file whole (the undo copy must never be half a save).
     */
    static int writeTo(String pkg, Map<String, File> dirs, File target) throws IOException {
        File tmp = new File(target.getPath() + ".tmp");
        int n;
        try (OutputStream o = new FileOutputStream(tmp)) {
            n = write(pkg, dirs, o);
        } catch (IOException | RuntimeException e) {
            tmp.delete();
            throw e;
        }
        if (target.exists() && !target.delete()) { tmp.delete(); throw new IOException("could not replace " + target); }
        if (!tmp.renameTo(target)) { tmp.delete(); throw new IOException("could not finish " + target); }
        return n;
    }

    /** Writes the saves of the game `pkg` (its original package) to `out`; returns how many files went in. */
    static int write(String pkg, Map<String, File> dirs, OutputStream out) throws IOException {
        int[] n = {0};
        ZipOutputStream z = new ZipOutputStream(out);
        z.putNextEntry(new ZipEntry(MANIFEST));
        z.write(("package=" + pkg + "\nformat=1\n").getBytes("UTF-8"));
        z.closeEntry();
        for (String s : SECTIONS) {
            File d = dirs.get(s);
            if (d != null) add(z, s, d, "", n);
        }
        z.finish();
        return n[0];
    }

    private static void add(ZipOutputStream z, String section, File dir, String rel, int[] n) throws IOException {
        File[] kids = dir.listFiles();
        if (kids == null) return;
        Arrays.sort(kids);
        for (File k : kids) {
            String r = rel.isEmpty() ? k.getName() : rel + "/" + k.getName();
            if (k.isDirectory()) {
                add(z, section, k, r, n);
            } else if (!ours(section, r)) {
                z.putNextEntry(new ZipEntry(section + "/" + r));
                try (InputStream in = new FileInputStream(k)) { copy(in, z); }
                z.closeEntry();
                n[0]++;
            }
        }
    }

    /**
     * Unpacks a save zip into `staging` (section/relative path). Null when it is a save of `pkg` and every entry is
     * allowed; otherwise the reason, in the player's words, and `staging` is emptied.
     */
    static String unpack(InputStream in, File staging, String pkg) throws IOException {
        String why;
        try {
            why = unpackOrWhy(in, staging, pkg);
        } catch (java.util.zip.ZipException | java.io.EOFException e) {
            why = "This file isn't a complete save (the zip is damaged or cut short)";
        } catch (IOException | RuntimeException e) {
            delete(staging);
            throw e;
        }
        if (why != null) delete(staging);
        return why;
    }

    private static String unpackOrWhy(InputStream in, File staging, String pkg) throws IOException {
        if (!staging.isDirectory() && !staging.mkdirs()) throw new IOException("could not create " + staging);
        String root = staging.getCanonicalPath() + File.separator;
        boolean manifest = false;
        ZipInputStream z = new ZipInputStream(in);
        for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
            String name = e.getName();
            if (name.equals(MANIFEST)) {
                Properties p = new Properties();
                p.load(z);
                if (!pkg.equals(p.getProperty("package"))) return "This save is from another game (" + p.getProperty("package") + ")";
                manifest = true;
                continue;
            }
            String section = allowed(name);
            if (section == null) return "This save has a file RetroBug won't write: " + name;
            File to = new File(staging, name);
            if (!to.getCanonicalPath().startsWith(root)) return "This save has a file RetroBug won't write: " + name;
            if (e.isDirectory()) {
                if (!to.isDirectory() && !to.mkdirs()) throw new IOException("could not create " + to);
                continue;
            }
            File parent = to.getParentFile();
            if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("could not create " + parent);
            try (OutputStream o = new FileOutputStream(to)) { copy(z, o); }
        }
        return manifest ? null : "This isn't a RetroBug save (it has no " + MANIFEST + ")";
    }

    /** The section of an allowed entry name ("section/relative/path"), or null. */
    private static String allowed(String name) {
        if (name.isEmpty() || name.startsWith("/") || name.indexOf('\\') >= 0 || name.indexOf(':') >= 0) return null;
        String[] parts = name.split("/", -1);
        if (parts.length < 2) return null;
        String section = parts[0];
        if (!Arrays.asList(SECTIONS).contains(section)) return null;
        for (int i = 1; i < parts.length; i++) {
            boolean last = i == parts.length - 1;
            if (parts[i].equals(".") || parts[i].equals("..") || (parts[i].isEmpty() && !last)) return null;
        }
        String rel = name.substring(section.length() + 1);
        if (rel.isEmpty() || ours(section, rel)) return null;
        return section;
    }

    /** Replaces the game's saves with an unpacked save: each folder emptied (but for RetroBug's own), then filled. */
    static void replace(Map<String, File> dirs, File staging) throws IOException {
        for (String s : SECTIONS) {
            File d = dirs.get(s);
            if (d == null) continue;
            clear(s, d, "");
            File from = new File(staging, s);
            if (from.isDirectory()) copyTree(from, d);
        }
    }

    private static void clear(String section, File dir, String rel) {
        File[] kids = dir.listFiles();
        if (kids == null) return;
        for (File k : kids) {
            String r = rel.isEmpty() ? k.getName() : rel + "/" + k.getName();
            if (k.isDirectory()) {
                clear(section, k, r);
                k.delete();  // stays when it still holds one of ours
            } else if (!ours(section, r)) {
                k.delete();
            }
        }
    }

    private static void copyTree(File from, File to) throws IOException {
        if (!to.isDirectory() && !to.mkdirs()) throw new IOException("could not create " + to);
        File[] kids = from.listFiles();
        if (kids == null) return;
        for (File k : kids) {
            File t = new File(to, k.getName());
            if (k.isDirectory()) {
                copyTree(k, t);
            } else {
                try (InputStream in = new FileInputStream(k); OutputStream o = new FileOutputStream(t)) { copy(in, o); }
            }
        }
    }

    /**
     * Whether a caller may export or import this game's saves: the RetroBug launcher, signed with the one certificate
     * whose SHA-256 (lower-case hex) was recorded in this copy when it was made.
     */
    static boolean trusted(String caller, List<byte[]> signers, String launcherCertSha256) {
        if (caller == null || !caller.equals(LAUNCHER) || launcherCertSha256 == null || launcherCertSha256.isEmpty()) return false;
        if (signers == null || signers.size() != 1) return false;
        try {
            StringBuilder hex = new StringBuilder();
            for (byte b : MessageDigest.getInstance("SHA-256").digest(signers.get(0))) hex.append(String.format("%02x", b));
            return hex.toString().equalsIgnoreCase(launcherCertSha256.trim());
        } catch (Exception e) {
            return false;
        }
    }

    /**
     * A name the copy will write into its own OBB folder for the launcher: a plain file name ending ".obb" (any case),
     * with something before it, no path separators, no "..", no control characters.
     */
    static boolean obbName(String n) {
        if (n == null || n.length() <= 4 || !n.toLowerCase(java.util.Locale.ROOT).endsWith(".obb")) return false;
        if (n.indexOf('/') >= 0 || n.indexOf('\\') >= 0 || n.contains("..") || n.startsWith(".")) return false;
        for (int i = 0; i < n.length(); i++) if (n.charAt(i) < 0x20) return false;
        return true;
    }

    static void delete(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) delete(k);
        f.delete();
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
    }
}
