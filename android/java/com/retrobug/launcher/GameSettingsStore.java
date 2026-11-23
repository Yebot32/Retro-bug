// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Where the launcher keeps each game's GameSettings: one small file per original package under
// getFilesDir()/game-settings/. (getFilesDir()/games/ already holds the APKs the player added.)
package com.retrobug.launcher;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

final class GameSettingsStore {
    private static final String TAG = "RetroBug";
    private final File dir;

    GameSettingsStore(Context ctx) {
        dir = new File(ctx.getFilesDir(), "game-settings");
    }

    GameSettings get(String packageName) {
        File f = file(packageName);
        if (!f.isFile()) return new GameSettings();
        try (InputStream in = new FileInputStream(f)) {
            byte[] b = new byte[(int) Math.min(f.length(), 64 * 1024)];
            int n = 0;
            for (int r; n < b.length && (r = in.read(b, n, b.length - n)) > 0; ) n += r;
            return GameSettings.parse(new String(b, 0, n, StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.w(TAG, "settings: could not read " + f + ": " + e);
            return new GameSettings();
        }
    }

    void put(String packageName, GameSettings s) {
        if (!dir.isDirectory() && !dir.mkdirs()) {
            Log.w(TAG, "settings: could not create " + dir);
            return;
        }
        File f = file(packageName), tmp = new File(dir, f.getName() + ".tmp");
        try (OutputStream o = new FileOutputStream(tmp)) {
            o.write(s.format().getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.w(TAG, "settings: could not write " + tmp + ": " + e);
            return;
        }
        if (!tmp.renameTo(f)) Log.w(TAG, "settings: could not replace " + f);
    }

    void remove(String packageName) {
        File f = file(packageName);
        if (f.exists() && !f.delete()) Log.w(TAG, "settings: could not delete " + f);
    }

    private File file(String packageName) {
        return new File(dir, packageName.replaceAll("[^A-Za-z0-9._-]", "_") + ".properties");
    }
}
