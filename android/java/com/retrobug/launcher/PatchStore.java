// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Where a game's M6 patches come from: built-in ones shipped with RetroBug (assets/patches/<package>/<id>.rbpatch) and
// ones the player added (getFilesDir()/patches/<package>/<id>.rbpatch). An added patch with the same id as a built-in
// one replaces it. A malformed file is skipped (and logged), never fatal. Which ones are on is the game's settings
// (GameSettings.patches); ConvertJob hands the enabled ones to the repackager.
package com.retrobug.launcher;

import android.content.Context;
import android.net.Uri;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

final class PatchStore {
    private static final String TAG = "RetroBug";

    private PatchStore() {}

    static File userDir(Context c, String pkg) { return new File(c.getFilesDir(), "patches/" + pkg); }

    /** Every patch for the game, built-in first, then the player's (which replace built-ins of the same id). */
    static List<Patch> available(Context c, Game g) { return available(c, g.packageName); }

    static List<Patch> available(Context c, String pkg) {
        Map<String, Patch> byId = new LinkedHashMap<>();
        try {
            String[] names = c.getAssets().list("patches/" + pkg);
            if (names != null) {
                Arrays.sort(names);
                for (String n : names) {
                    if (!n.endsWith(".rbpatch")) continue;
                    try (InputStream in = c.getAssets().open("patches/" + pkg + "/" + n)) {
                        Patch p = parse(read(in), pkg, n);
                        if (p != null) { p.builtIn = true; byId.put(p.id, p); }
                    }
                }
            }
        } catch (IOException e) {
            Log.w(TAG, "patches: built-in list for " + pkg + ": " + e);
        }
        File[] mine = userDir(c, pkg).listFiles();
        if (mine != null) {
            Arrays.sort(mine);
            for (File f : mine) {
                if (!f.getName().endsWith(".rbpatch")) continue;
                try (InputStream in = new FileInputStream(f)) {
                    Patch p = parse(read(in), pkg, f.getName());
                    if (p != null) byId.put(p.id, p);
                } catch (IOException e) {
                    Log.w(TAG, "patches: " + f + ": " + e);
                }
            }
        }
        return new ArrayList<>(byId.values());
    }

    private static Patch parse(String text, String pkg, String name) {
        try {
            Patch p = Patch.parse(text);
            if (p.game != null && !p.game.equals(pkg)) {
                Log.w(TAG, "patches: " + name + " is for " + p.game + ", not " + pkg + " — skipped");
                return null;
            }
            return p;
        } catch (RuntimeException e) {
            Log.w(TAG, "patches: " + name + " is not a valid patch: " + e.getMessage());
            return null;
        }
    }

    /** The enabled patches for the game, as the repackager takes them. */
    static List<Patch> enabled(Context c, Game g, GameSettings s) {
        return Patch.enabled(available(c, g), s.patches);
    }

    /**
     * Adds a patch file the player picked: checked first (a valid patch, for this game), then kept as <id>.rbpatch.
     * Null when added; otherwise the reason, in the player's words.
     */
    static String add(Context c, Game g, Uri uri) {
        String pkg = g.packageName;
        String text;
        try (InputStream in = c.getContentResolver().openInputStream(uri)) {
            if (in == null) return "The picker returned a file RetroBug can't open";
            text = read(in);
        } catch (IOException | SecurityException e) {
            return "Couldn't read the file: " + e.getMessage();
        }
        Patch p;
        try {
            p = Patch.parse(text);
        } catch (RuntimeException e) {
            return "That isn't a RetroBug patch: " + e.getMessage();
        }
        if (p.game != null && !p.game.equals(pkg)) return "That patch is for another game (" + p.game + ")";
        File dir = userDir(c, pkg);
        if (!dir.isDirectory() && !dir.mkdirs()) return "Couldn't create " + dir;
        File to = new File(dir, p.id + ".rbpatch");
        try (OutputStream o = new FileOutputStream(to)) {
            o.write(text.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            return "Couldn't save the patch: " + e.getMessage();
        }
        return null;
    }

    /** Removes a patch the player added (built-in ones stay); true when a file was removed. */
    static boolean remove(Context c, Game g, Patch p) {
        String pkg = g.packageName;
        return !p.builtIn && new File(userDir(c, pkg), p.id + ".rbpatch").delete();
    }

    /** "applied" / "not applied — why" for patch `id`, from the copy's last conversion report; null when unknown. */
    static String lastResult(Context c, Game g, String id) {
        File report = new File(new File(c.getExternalFilesDir(null), "repack"), g.copyPackage() + ".txt");
        try (InputStream in = new FileInputStream(report)) {
            for (String line : read(in).split("\n")) {
                String t = line.trim();
                if (t.startsWith("patch " + id + ": ")) return t.substring(("patch " + id + ": ").length());
            }
        } catch (IOException e) {
            return null;
        }
        return null;
    }

    private static String read(InputStream in) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] b = new byte[8192];
        for (int n; (n = in.read(b)) > 0 && o.size() < (256 << 10); ) o.write(b, 0, n);
        return o.toString("UTF-8");
    }
}
