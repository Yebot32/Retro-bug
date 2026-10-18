// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// A game's OBB (its extra data file) for RetroBug's copy. The copy is renamed, so it looks in its own folder,
// Android/obb/<pkg>.rb/, for a file named for <pkg>.rb (ObbRules); nothing is there until RetroBug puts it there. The
// source is an OBB the player added for the game (kept in getFilesDir()/obb/<pkg>/), else the original game's own OBB
// in Android/obb/<pkg>/ when that is on the phone. Android lets an app that may install apps (this launcher) read and
// write Android/obb; the original's folder is only ever read. The copy's folder goes when the copy is uninstalled.
package com.retrobug.launcher;

import android.content.Context;
import android.net.Uri;
import android.os.Environment;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

final class ObbStore {
    private ObbStore() {}

    interface Progress {
        /** Bytes copied so far of `total`; on the copying thread. */
        void at(long done, long total);
    }

    static File obbRoot() { return new File(Environment.getExternalStorageDirectory(), "Android/obb"); }

    static File originalDir(Game g) { return new File(obbRoot(), g.packageName); }

    static File copyDir(Game g) { return new File(obbRoot(), g.copyPackage()); }

    static File addedDir(Context c, Game g) { return new File(c.getFilesDir(), "obb/" + g.packageName); }

    /** The OBBs to give the copy: the ones the player added, else the original game's. Empty when there are none. */
    static List<File> sources(Context c, Game g) {
        List<File> added = obbs(addedDir(c, g));
        return added.isEmpty() ? obbs(originalDir(g)) : added;
    }

    /** True when the sources are the player's added files (not the original game's). */
    static boolean added(Context c, Game g) { return !obbs(addedDir(c, g)).isEmpty(); }

    static List<File> installed(Game g) { return obbs(copyDir(g)); }

    static long size(List<File> files) {
        long n = 0;
        for (File f : files) n += f.length();
        return n;
    }

    private static List<File> obbs(File dir) {
        List<File> out = new ArrayList<>();
        File[] kids = dir.listFiles();
        if (kids == null) return out;
        Arrays.sort(kids);
        for (File f : kids) if (f.isFile() && ObbRules.isObb(f.getName())) out.add(f);
        return out;
    }

    /** True when every source is in the copy's folder under its copy name, at its size, and the copy's own file. */
    static boolean upToDate(Context c, Game g) {
        List<File> src = sources(c, g);
        if (src.isEmpty()) return true;
        for (File s : src) {
            File t = new File(copyDir(g), ObbRules.copyName(s.getName(), g.packageName, g.versionCode));
            if (t.length() != s.length() || ours(t)) return false;
        }
        return true;
    }

    /** True when this launcher owns `f` — an OBB the game itself cannot read (Android checks the file's owner). */
    private static boolean ours(File f) {
        try {
            return android.system.Os.stat(f.getAbsolutePath()).st_uid == android.os.Process.myUid();
        } catch (Exception e) {
            return false;
        }
    }

    private static Uri provider(Game g, String path) {
        return Uri.parse("content://" + g.copyPackage() + ShimPlan.SAVES_AUTHORITY_SUFFIX + "/" + path);
    }

    /**
     * Copies the sources into the copy's folder. The game reads its OBB as itself, so the copy writes it: its RetroBug
     * helper (RbSaveProvider) creates the folder and opens each file, and this streams the bytes in. Null when done;
     * otherwise the reason. Run off the UI thread.
     */
    static String install(Context c, Game g, Progress p) {
        // One copy per game at a time: the menu and a conversion's automatic install would write the same .part.
        synchronized (BUSY) {
            if (!BUSY.add(g.packageName)) return "This game's OBB is already being copied";
        }
        try {
            return installLocked(c, g, p);
        } finally {
            synchronized (BUSY) { BUSY.remove(g.packageName); }
        }
    }

    private static final java.util.Set<String> BUSY = new java.util.HashSet<>();

    /** Free space needed beyond the copy itself, so the phone is not filled to the last byte. */
    static final long SPACE_MARGIN = 64L << 20;

    private static String installLocked(Context c, Game g, Progress p) {
        List<File> src = sources(c, g);
        if (src.isEmpty()) return "This game has no OBB on this phone. Add its OBB file first.";
        File dir = copyDir(g);
        long need = 0;
        for (File s : src) {
            File t = new File(dir, ObbRules.copyName(s.getName(), g.packageName, g.versionCode));
            if (t.length() != s.length() || ours(t)) need += s.length();
        }
        long free = obbRoot().getUsableSpace();
        if (need > 0 && free < need + SPACE_MARGIN) {
            return "Not enough storage: the OBB needs " + ((need + SPACE_MARGIN) >> 20) + " MB free, the phone has "
                    + (free >> 20) + " MB";
        }
        // Anything this launcher put there itself (an earlier build did) is unreadable to the game: cleared first.
        File[] old = dir.listFiles();
        if (old != null) for (File f : old) if (ours(f)) f.delete();
        if (dir.isDirectory() && ours(dir)) dir.delete();
        android.content.ContentResolver cr = c.getContentResolver();
        try {
            android.os.Bundle b = SaveClient.callCopy(c, g, "obbPrepare", null);
            if (b == null || !b.getBoolean("ok")) return "The game's copy couldn't create its OBB folder";
        } catch (IllegalArgumentException e) {
            return SaveClient.OLD_COPY;  // a copy made before RetroBug's helper could take an OBB
        } catch (RuntimeException e) {
            return "Couldn't reach the game's copy: " + e.getMessage();
        }
        long total = size(src), done = 0;
        for (File s : src) {
            String name = ObbRules.copyName(s.getName(), g.packageName, g.versionCode);
            File t = new File(dir, name);
            if (t.length() == s.length() && !ours(t)) {
                done += s.length();
                p.at(done, total);
                continue;
            }
            try (InputStream in = new FileInputStream(s);
                 android.os.ParcelFileDescriptor pfd = cr.openFileDescriptor(provider(g, "obb/" + name + ".part"), "w");
                 OutputStream out = new FileOutputStream(pfd.getFileDescriptor())) {
                byte[] buf = new byte[1 << 20];
                long last = 0;
                for (int n; (n = in.read(buf)) > 0; ) {
                    out.write(buf, 0, n);
                    done += n;
                    if (done - last >= (8 << 20)) {
                        p.at(done, total);
                        last = done;
                    }
                }
            } catch (IOException | RuntimeException e) {
                new File(dir, name + ".part").delete();  // the launcher may delete in the folder (it can write Android/obb)
                return "Couldn't copy " + s.getName() + ": " + e.getMessage();
            }
            android.os.Bundle b = SaveClient.callCopy(c, g, "obbCommit", name);
            if (b == null || !b.getBoolean("ok")) return "The game's copy couldn't finish " + name;
            p.at(done, total);
        }
        return null;
    }

    /** Removes the copy's OBBs (never the original game's). */
    static void remove(Game g) {
        for (File f : installed(g)) f.delete();
        File[] rest = copyDir(g).listFiles();
        if (rest != null) for (File f : rest) if (f.getName().endsWith(".part")) f.delete();
    }

    /** Keeps a picked OBB file for the game (replacing any added before). Null when done; otherwise the reason. */
    static String add(Context c, Game g, Uri uri, String name, Progress p) {
        if (name == null || !ObbRules.isObb(name)) return "That isn't an .obb file";
        File dir = addedDir(c, g);
        if (!dir.isDirectory() && !dir.mkdirs()) return "Couldn't create " + dir;
        long size = sizeOf(c, uri);
        long free = dir.getUsableSpace();
        if (size > 0 && free < size + SPACE_MARGIN) {
            return "Not enough storage: this OBB needs " + ((size + SPACE_MARGIN) >> 20) + " MB free, the phone has "
                    + (free >> 20) + " MB";
        }
        File t = new File(dir, name.replaceAll("[^A-Za-z0-9._-]", "_"));
        File part = new File(dir, t.getName() + ".part");
        long done = 0;
        try (InputStream in = c.getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(part)) {
            if (in == null) return "The picker returned a file RetroBug can't open";
            byte[] buf = new byte[1 << 20];
            long last = 0;
            for (int n; (n = in.read(buf)) > 0; ) {
                out.write(buf, 0, n);
                done += n;
                if (done - last >= (8 << 20)) {
                    p.at(done, size);
                    last = done;
                }
            }
        } catch (IOException | SecurityException e) {
            part.delete();
            return "Couldn't copy the file: " + e.getMessage();
        }
        // Only now is an older OBB of the same kind (main or patch) replaced: a failed copy keeps the old one.
        String kind = t.getName().contains(".") ? t.getName().substring(0, t.getName().indexOf('.')) : t.getName();
        for (File old : obbs(dir)) if (!old.equals(t) && old.getName().startsWith(kind + ".")) old.delete();
        if (t.exists() && !t.delete()) { part.delete(); return "Couldn't replace " + t.getName(); }
        return part.renameTo(t) ? null : "Couldn't finish " + t.getName();
    }

    /** The picked file's size from its provider, or -1 when it does not say. */
    private static long sizeOf(Context c, Uri uri) {
        try (android.database.Cursor q = c.getContentResolver().query(uri,
                new String[] {android.provider.OpenableColumns.SIZE}, null, null, null)) {
            if (q != null && q.moveToFirst() && !q.isNull(0)) return q.getLong(0);
        } catch (RuntimeException ignored) {
            // no size: copied without the check
        }
        return -1;
    }

    /** Forgets the player's added OBB for the game. */
    static void forgetAdded(Context c, Game g) {
        for (File f : obbs(addedDir(c, g))) f.delete();
    }
}
