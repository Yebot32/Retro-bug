// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Entry point of the targetSdk compatibility shims in a repackaged game. The repackager adds this provider to
// the game's manifest (not exported, highest initOrder) because the platform creates providers on the main
// thread after the Application object is attached but before Application.onCreate, any activity or any
// service — early enough for every shim, with no change to the game's own classes. It reads
// assets/rb32/rbshim.properties (written by com.retrobug.launcher.ShimPlan) and runs the listed shims. It
// serves no data.
package com.retrobug.shim;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.util.Log;

import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Properties;

public final class RbShimProvider extends ContentProvider {
    static final String CONFIG = "rb32/rbshim.properties";

    @Override public boolean onCreate() {
        Properties p = new Properties();
        try (InputStream in = getContext().getAssets().open(CONFIG)) {
            p.load(in);
            int target = Integer.parseInt(p.getProperty("targetSdk", "").trim());
            List<String> ids = new ArrayList<>();
            for (String s : Arrays.asList(p.getProperty("shims", "").split(","))) if (!s.trim().isEmpty()) ids.add(s.trim());
            Log.i(Shims.TAG, "shims: " + ids + " for original targetSdk " + target + " (" + getContext().getPackageName() + ")");
            Shims.apply(getContext(), target, ids);
        } catch (Throwable t) {
            Log.i(Shims.TAG, "shims FAILED: could not read assets/" + CONFIG + ": " + t);
        }
        String settings = fetchSettings();
        if ("1".equals(setting(settings, "overlay"))) Overlay.install(getContext());
        return true;
    }

    /** The value of `key` in "key=value" settings text (GameSettings.format), null when absent. Exact key match. */
    static String setting(String text, String key) {
        for (String line : text.split("\n")) {
            int eq = line.indexOf('=');
            if (eq == key.length() && line.startsWith(key)) return line.substring(eq + 1).trim();
        }
        return null;
    }

    static final String SETTINGS_AUTHORITY = "com.retrobug.launcher.settings";
    static final String SETTINGS_FILE = "rb-settings.properties";

    /**
     * The player's settings for this game, from the RetroBug launcher (its SettingsProvider answers only this copy),
     * saved to getFilesDir()/rb-settings.properties for the native runtime, which reads it at boot. When the launcher
     * cannot be asked (uninstalled, or it refuses), the last saved settings stand; none saved means the defaults.
     */
    private String fetchSettings() {
        java.io.File file = new java.io.File(getContext().getFilesDir(), SETTINGS_FILE);
        String pkg = getContext().getPackageName();
        String original = pkg.endsWith(".rb") ? pkg.substring(0, pkg.length() - 3) : pkg;
        String text = null;
        try (Cursor c = getContext().getContentResolver().query(
                Uri.parse("content://" + SETTINGS_AUTHORITY + "/" + original), null, null, null, null)) {
            if (c != null && c.moveToFirst()) text = c.getString(0);
        } catch (Throwable t) {
            Log.i(Shims.TAG, "settings: the launcher could not be asked (" + t + "); using the last saved ones");
        }
        if (text != null) {
            try (java.io.OutputStream o = new java.io.FileOutputStream(file)) {
                o.write(text.getBytes("UTF-8"));
            } catch (Throwable t) {
                Log.i(Shims.TAG, "settings: could not save " + file + ": " + t);
            }
            Log.i(Shims.TAG, "settings: " + text.replace('\n', ' ').trim());
            return text;
        }
        try (java.io.InputStream in = new java.io.FileInputStream(file)) {
            java.io.ByteArrayOutputStream all = new java.io.ByteArrayOutputStream();
            byte[] b = new byte[4096];
            for (int n; (n = in.read(b)) > 0 && all.size() < (64 << 10); ) all.write(b, 0, n);
            return all.toString("UTF-8");
        } catch (Throwable t) {
            return "";
        }
    }

    @Override public Cursor query(Uri u, String[] proj, String sel, String[] args, String order) { return null; }
    @Override public String getType(Uri u) { return null; }
    @Override public Uri insert(Uri u, ContentValues v) { return null; }
    @Override public int delete(Uri u, String sel, String[] args) { return 0; }
    @Override public int update(Uri u, ContentValues v, String sel, String[] args) { return 0; }
}
