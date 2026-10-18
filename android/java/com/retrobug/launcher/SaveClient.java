// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The Data tab's side of save export and import: asks the converted copy's RbSaveProvider (content://<pkg>.rb.rbsaves),
// which answers only this launcher. Export: the copy zips its saves, this copies the zip to where the player chose.
// Import: this sends the chosen zip, the copy checks it, keeps the current saves for undo, swaps them and ends its own
// process. All calls block: run them off the UI thread. Each returns null when done, else the reason for the player.
package com.retrobug.launcher;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;
import android.os.Bundle;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

final class SaveClient {
    private SaveClient() {}

    static final String OLD_COPY = "This game's copy is older than saves: tap Update (or Make playable) first.";

    private static Uri uri(Game g, String path) {
        return Uri.parse("content://" + g.copyPackage() + ShimPlan.SAVES_AUTHORITY_SUFFIX + "/" + path);
    }

    /** True when the copy is installed and carries RetroBug's save/OBB helper (copies made from runtime stamp 2 on). */
    static boolean available(Context c, Game g) {
        try {
            return c.getPackageManager().resolveContentProvider(g.copyPackage() + ShimPlan.SAVES_AUTHORITY_SUFFIX, 0) != null;
        } catch (RuntimeException e) {
            return false;
        }
    }

    private static Bundle call(Context c, Game g, String method) {
        return callCopy(c, g, method, null);
    }

    /**
     * A call to the copy's helper through an *unstable* provider client. The call starts the game's process; a game
     * that crashes at start (SpongeBob's protection SDK did) must not take RetroBug down with it, which is what Android
     * does to a client holding a stable reference to a provider whose process dies. Throws IllegalArgumentException
     * when the copy has no helper, and RuntimeException (DeadObjectException wrapped) when its process died.
     */
    static Bundle callCopy(Context c, Game g, String method, String arg) {
        android.content.ContentProviderClient client =
                c.getContentResolver().acquireUnstableContentProviderClient(g.copyPackage() + ShimPlan.SAVES_AUTHORITY_SUFFIX);
        if (client == null) throw new IllegalArgumentException("Unknown authority");
        try {
            return client.call(method, arg, null);
        } catch (android.os.RemoteException e) {
            throw new IllegalStateException("The game's copy stopped while RetroBug was asking it (" + method + ")", e);
        } finally {
            client.close();
        }
    }

    /** True when the copy has saves kept from before an import; null when the copy cannot be asked. */
    static Boolean hasUndo(Context c, Game g) {
        try {
            Bundle b = call(c, g, "status");
            return b != null && b.getBoolean("undo");
        } catch (RuntimeException e) {
            return null;
        }
    }

    /** Exports to `dest`; on failure the file "Save as" already created there is removed again (no empty save left). */
    static String export(Context c, Game g, Uri dest) {
        String why = exportOrWhy(c, g, dest);
        if (why != null) {
            try {
                android.provider.DocumentsContract.deleteDocument(c.getContentResolver(), dest);
            } catch (Exception ignored) {
                // a provider that can't delete: the empty file stays, the message still says it failed
            }
        }
        return why;
    }

    private static String exportOrWhy(Context c, Game g, Uri dest) {
        ContentResolver cr = c.getContentResolver();
        try {
            Bundle b = call(c, g, "export");
            if (b == null) return OLD_COPY;
            if (!b.getBoolean("ok")) return "The game couldn't pack its saves: " + b.getString("message");
            try (InputStream in = cr.openInputStream(uri(g, "export")); OutputStream out = cr.openOutputStream(dest, "wt")) {
                if (in == null || out == null) return "Couldn't open the files to copy";
                copy(in, out);
            }
            return null;
        } catch (IllegalArgumentException e) {
            return OLD_COPY;  // "Unknown authority": a copy made before saves
        } catch (IOException | RuntimeException e) {
            return "Couldn't export: " + e.getMessage();
        }
    }

    static String importFrom(Context c, Game g, Uri src) {
        ContentResolver cr = c.getContentResolver();
        try {
            if (call(c, g, "status") == null) return OLD_COPY;
            try (InputStream in = cr.openInputStream(src); OutputStream out = cr.openOutputStream(uri(g, "import"), "w")) {
                if (in == null || out == null) return "Couldn't open the files to copy";
                copy(in, out);
            }
            Bundle b = call(c, g, "import");
            return b != null && b.getBoolean("ok") ? null : (b == null ? OLD_COPY : b.getString("message"));
        } catch (IllegalArgumentException e) {
            return OLD_COPY;
        } catch (IOException | RuntimeException e) {
            return "Couldn't import: " + e.getMessage();
        }
    }

    /**
     * Keeps a copy of the game's current saves in getFilesDir()/backups/<package>/<time>.zip (the newest three are kept)
     * before RetroBug re-makes its copy. Returns the file's path, or null when there was nothing to back up (no copy,
     * or a copy without the helper) or the backup failed (logged; the update still goes ahead).
     */
    static String backup(Context c, Game g) {
        if (!available(c, g)) return null;
        java.io.File dir = new java.io.File(c.getFilesDir(), "backups/" + g.packageName);
        if (!dir.isDirectory() && !dir.mkdirs()) return null;
        java.io.File to = new java.io.File(dir, new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.ROOT)
                .format(new java.util.Date()) + ".zip");
        String why = export(c, g, Uri.fromFile(to));
        if (why != null) {
            android.util.Log.w("RetroBug", "saves backup for " + g.packageName + " failed: " + why);
            to.delete();
            return null;
        }
        java.io.File[] all = dir.listFiles();
        if (all != null && all.length > 3) {
            java.util.Arrays.sort(all);
            for (int i = 0; i < all.length - 3; i++) all[i].delete();
        }
        return to.getAbsolutePath();
    }

    static String undo(Context c, Game g) {
        try {
            Bundle b = call(c, g, "undo");
            return b != null && b.getBoolean("ok") ? null : (b == null ? OLD_COPY : b.getString("message"));
        } catch (IllegalArgumentException e) {
            return OLD_COPY;
        } catch (RuntimeException e) {
            return "Couldn't undo: " + e.getMessage();
        }
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
    }
}
