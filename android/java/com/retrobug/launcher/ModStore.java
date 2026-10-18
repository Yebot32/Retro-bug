// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// A game's mods (M6): the .zip files the player added, kept as getFilesDir()/mods/<package>/<id>.zip. Adding one checks
// it first (Mod.open: a valid mod, for this game, only assets/res) and checks there is room. Which ones are on is the
// game's settings (GameSettings.mods); ConvertJob hands the enabled ones to the repackager.
package com.retrobug.launcher;

import android.content.Context;
import android.net.Uri;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

final class ModStore {
    private static final String TAG = "RetroBug";

    private ModStore() {}

    static File dir(Context c, String pkg) { return new File(c.getFilesDir(), "mods/" + pkg); }

    static List<Mod> available(Context c, String pkg) {
        List<Mod> out = new ArrayList<>();
        File[] files = dir(c, pkg).listFiles();
        if (files == null) return out;
        Arrays.sort(files);
        for (File f : files) {
            if (!f.getName().endsWith(".zip")) continue;
            try {
                Mod m = Mod.open(f);
                if (m.game == null || m.game.equals(pkg)) out.add(m);
            } catch (IllegalArgumentException e) {
                Log.w(TAG, "mods: " + f + ": " + e.getMessage());
            }
        }
        return out;
    }

    static List<Mod> enabled(Context c, Game g, GameSettings s) { return Mod.enabled(available(c, g.packageName), s.mods); }

    /** Adds a mod the player picked. Null when added; otherwise the reason, in the player's words. Off the UI thread. */
    static String add(Context c, Game g, Uri uri) {
        File d = dir(c, g.packageName);
        if (!d.isDirectory() && !d.mkdirs()) return "Couldn't create " + d;
        File part = new File(d, "incoming.part");
        try (InputStream in = c.getContentResolver().openInputStream(uri); OutputStream o = new FileOutputStream(part)) {
            if (in == null) return "The picker returned a file RetroBug can't open";
            byte[] b = new byte[1 << 16];
            long n = 0;
            for (int k; (k = in.read(b)) > 0; ) {
                o.write(b, 0, k);
                n += k;
                if (n > d.getUsableSpace() - ObbStore.SPACE_MARGIN) throw new IOException("the phone is out of storage space");
            }
        } catch (IOException | SecurityException e) {
            part.delete();
            return "Couldn't copy the mod: " + e.getMessage();
        }
        Mod m;
        try {
            m = Mod.open(part);
        } catch (IllegalArgumentException e) {
            part.delete();
            return e.getMessage();
        }
        if (m.game != null && !m.game.equals(g.packageName)) {
            part.delete();
            return "That mod is for another game (" + m.game + ")";
        }
        File to = new File(d, m.id + ".zip");
        if (to.exists() && !to.delete()) { part.delete(); return "Couldn't replace the older copy of this mod"; }
        if (!part.renameTo(to)) { part.delete(); return "Couldn't finish adding the mod"; }
        return null;
    }

    static boolean remove(Context c, Game g, Mod m) { return m.zip.delete(); }
}
