// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The library's contents: the installed launcher apps built for 32-bit ARM only (found through the public launcher
// query the manifest's <queries> allows), plus the APKs the player added to getFilesDir()/games/. One game per package;
// an added APK of an installed game joins it. Runs on the executor it is given; listener calls arrive on the main
// thread, one per game as it is found, so the grid fills while the scan goes on.
package com.retrobug.launcher;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.File;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.Executor;

final class GameCatalog {
    private static final String TAG = "RetroBug";

    interface Listener {
        /** A game as it is found (the first sighting of each package). */
        void onGame(Game g);
        /** The whole library, one entry per package, after the scan: an added APK of an installed game is merged in. */
        void onDone(java.util.List<Game> all);
    }

    private final Context ctx;
    private final Executor bg;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final GameSettingsStore settings;

    GameCatalog(Context ctx, Executor bg) {
        this.ctx = ctx.getApplicationContext();
        this.bg = bg;
        this.settings = new GameSettingsStore(this.ctx);
    }

    static File gamesDir(Context ctx) {
        File dir = new File(ctx.getFilesDir(), "games");
        if (!dir.exists() && !dir.mkdirs()) Log.w(TAG, "could not create " + dir);
        return dir;
    }

    void refresh(final Listener l) {
        bg.execute(new Runnable() {
            @Override public void run() {
                Map<String, Game> seen = new LinkedHashMap<>();
                PackageManager pm = ctx.getPackageManager();
                Intent launcher = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
                for (ResolveInfo ri : pm.queryIntentActivities(launcher, 0)) {
                    String pkg = ri.activityInfo.packageName;
                    // RetroBug's own apps (this launcher, test probes) and converted copies are not games to offer.
                    if (seen.containsKey(pkg) || pkg.startsWith("com.retrobug.") || pkg.endsWith(ApkRepackager.RENAME_SUFFIX))
                        continue;
                    Game g = installed(pm, pkg);
                    if (g == null) continue;
                    seen.put(pkg, g);
                    post(l, g);
                }
                File[] files = gamesDir(ctx).listFiles();
                if (files != null) {
                    java.util.Arrays.sort(files);
                    for (File f : files) {
                        if (!f.getName().toLowerCase().endsWith(".apk")) continue;
                        Game g = added(pm, f, seen);
                        if (g == null) continue;
                        boolean joined = seen.containsKey(g.key);
                        seen.put(g.key, g);
                        if (!joined) post(l, g);  // an installed game's tile is already up; the final list carries the merge
                    }
                }
                final java.util.List<Game> all = new java.util.ArrayList<>(seen.values());
                main.post(new Runnable() {
                    @Override public void run() { l.onDone(all); }
                });
            }
        });
    }

    /** An installed original, when its base APK is built for 32-bit ARM only; null otherwise. */
    private Game installed(PackageManager pm, String pkg) {
        try {
            ApplicationInfo ai = pm.getApplicationInfo(pkg, 0);
            if ((ai.flags & ApplicationInfo.FLAG_SYSTEM) != 0) return null;
            File apk = new File(ai.sourceDir);
            if (!ApkScan.is32BitOnly(ApkScan.abis(apk))) return null;
            PackageInfo pi = pm.getPackageInfo(pkg, 0);
            String label = String.valueOf(ai.loadLabel(pm));
            return new Game(pkg, pkg, label, versionCode(pi), apk, true, false, stateOf(pm, pkg));
        } catch (Exception e) {
            return null;  // a package that vanished mid-scan, or an APK this app cannot read: not a game we can offer
        }
    }

    /** An added APK; joins the installed game of the same package when there is one. */
    private Game added(PackageManager pm, File f, Map<String, Game> seen) {
        try {
            if (!ApkScan.is32BitOnly(ApkScan.abis(f))) return null;
            PackageInfo pi = pm.getPackageArchiveInfo(f.getAbsolutePath(), 0);
            if (pi == null || pi.applicationInfo == null) throw new IllegalStateException("not a package");
            pi.applicationInfo.sourceDir = f.getAbsolutePath();
            pi.applicationInfo.publicSourceDir = f.getAbsolutePath();
            String pkg = pi.packageName;
            if (pkg.startsWith("com.retrobug.")) return null;  // RetroBug's own test APKs (the JNI bridge app) are not games
            Game have = seen.get(pkg);
            if (have != null) {
                return new Game(pkg, pkg, have.label, have.versionCode, have.apk, true, true, have.state);
            }
            String label = String.valueOf(pi.applicationInfo.loadLabel(pm));
            return new Game(pkg, pkg, label, versionCode(pi), f, false, true, stateOf(pm, pkg));
        } catch (Exception e) {
            String name = f.getName().replaceAll("\\.apk$", "");
            return new Game("file:" + f.getName(), name, name, 0, f, false, true, Game.State.UNREADABLE);
        }
    }

    /** NOT_CONVERTED, READY, or UPDATE (made by an older runtime, or for another target than the game's setting). */
    Game.State stateOf(PackageManager pm, String pkg) {
        try {
            PackageInfo copy = pm.getPackageInfo(pkg + ApkRepackager.RENAME_SUFFIX, 0);
            int stamp = ApkRepackager.stampOf(copy.versionName);
            int wantTarget = settings.get(pkg).target;
            boolean current = stamp == ApkRepackager.RUNTIME_STAMP && copy.applicationInfo != null
                    && copy.applicationInfo.targetSdkVersion == wantTarget;
            return current ? Game.State.READY : Game.State.UPDATE;
        } catch (PackageManager.NameNotFoundException e) {
            return Game.State.NOT_CONVERTED;
        }
    }

    @SuppressWarnings("deprecation")
    private static long versionCode(PackageInfo pi) {
        return Build.VERSION.SDK_INT >= 28 ? pi.getLongVersionCode() : pi.versionCode;
    }

    private void post(final Listener l, final Game g) {
        main.post(new Runnable() {
            @Override public void run() { l.onGame(g); }
        });
    }
}
