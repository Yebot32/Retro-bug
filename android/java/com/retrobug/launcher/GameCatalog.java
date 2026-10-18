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
                // The scan must always reach onDone (C22): an exception used to escape this runnable and the library
                // sat on "Looking for your games…" forever. One unreadable package now skips itself, and the final
                // list is posted from a finally.
                try {
                    PackageManager pm = ctx.getPackageManager();
                    Intent launcher = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
                    String shows = SettingsScreen.libraryShows(ctx);  // Settings > Library shows
                    java.util.List<ResolveInfo> activities;
                    try {
                        activities = shows.equals("added") ? java.util.Collections.<ResolveInfo>emptyList()
                                : pm.queryIntentActivities(launcher, 0);
                    } catch (RuntimeException e) {
                        Log.w(TAG, "catalog: could not list activities: " + e);
                        activities = java.util.Collections.emptyList();
                    }
                    for (ResolveInfo ri : activities) {
                        String pkg = ri.activityInfo.packageName;
                        // RetroBug's own apps (this launcher, test probes) and converted copies are not games to offer.
                        if (seen.containsKey(pkg) || pkg.startsWith("com.retrobug.") || pkg.endsWith(ApkRepackager.RENAME_SUFFIX))
                            continue;
                        Game g;
                        try {
                            g = installed(pm, pkg, ri.activityInfo.name);
                        } catch (RuntimeException e) {
                            Log.w(TAG, "catalog: skipped " + pkg + ": " + e);
                            continue;
                        }
                        if (g == null) continue;
                        seen.put(pkg, g);
                        post(l, g);
                    }
                    File[] files = shows.equals("installed") ? null : gamesDir(ctx).listFiles();
                    if (files != null) {
                        java.util.Arrays.sort(files);
                        for (File f : files) {
                            if (!f.getName().toLowerCase().endsWith(".apk")) continue;
                            Game g;
                            try {
                                g = added(pm, f, seen);
                            } catch (RuntimeException e) {
                                Log.w(TAG, "catalog: skipped " + f.getName() + ": " + e);
                                continue;
                            }
                            if (g == null) continue;
                            boolean joined = seen.containsKey(g.key);
                            seen.put(g.key, g);
                            if (!joined) post(l, g);  // an installed game's tile is already up; the final list carries the merge
                        }
                    }
                } catch (RuntimeException e) {
                    Log.e(TAG, "catalog scan failed: " + e);
                } finally {
                    final java.util.List<Game> all = new java.util.ArrayList<>(seen.values());
                    main.post(new Runnable() {
                        @Override public void run() { l.onDone(all); }
                    });
                }
            }
        });
    }

    /**
     * An installed original built for 32-bit ARM only. One whose 32-bit code sits in a split APK gets a tile that says
     * so, as does one that starts itself from native code (a NativeActivity, C31); a 64-bit or Java-only app, one whose
     * APK cannot be read, or one that vanished mid-scan, is null.
     */
    private Game installed(PackageManager pm, String pkg, String launcher) {
        ApplicationInfo ai;
        PackageInfo pi;
        try {
            ai = pm.getApplicationInfo(pkg, 0);
            pi = pm.getPackageInfo(pkg, 0);
        } catch (Exception e) {
            return null;  // vanished mid-scan
        }
        if ((ai.flags & ApplicationInfo.FLAG_SYSTEM) != 0) return null;
        File apk = new File(ai.sourceDir);
        String label = String.valueOf(ai.loadLabel(pm));
        Game g = new Game(pkg, pkg, label, versionCode(pi), apk, true, false, Game.State.NOT_CONVERTED);
        g.hasCode = (ai.flags & ApplicationInfo.FLAG_HAS_CODE) != 0;
        java.util.Set<String> base, splits = new java.util.TreeSet<>();
        try {
            base = ApkScan.abis(apk);
            if (ai.splitSourceDirs != null) for (String s : ai.splitSourceDirs) splits.addAll(ApkScan.abis(new File(s)));
        } catch (Exception e) {
            // Unreadable, so not known to be a game at all: a tile would put ordinary apps in the library. Logged.
            Log.w(TAG, "library: cannot read " + pkg + "'s APK, not listed: " + e);
            return null;
        }
        boolean hasSplits = ai.splitSourceDirs != null && ai.splitSourceDirs.length > 0;
        switch (LibraryRules.classify(base, splits, hasSplits)) {
            case GAME:
                g.launcher = launcher;
                if (startsAsNativeActivity(apk, pkg, launcher, base)) return g.unplayable(LibraryRules.NATIVE_ACTIVITY_WHY);
                g.state = stateOf(pm, pi);
                return g;
            case SPLIT:
                return g.unplayable("This game was installed in parts (split APKs); RetroBug can only convert a single "
                        + "APK for now. Add the game's full APK instead.");
            default:
                return null;
        }
    }

    /** C31's question, asked off the scan's own thread of control: an unreadable DEX leaves the tile as it was. */
    private boolean startsAsNativeActivity(File apk, String pkg, String launcher, java.util.Set<String> abis) {
        try {
            return ApkScan.isNativeActivity(apk, launcher, abis);
        } catch (Exception e) {
            Log.w(TAG, "library: cannot read " + pkg + "'s APK for its start, not checked: " + e);
            return false;
        }
    }

    /** An added APK; joins the installed game of the same package when there is one. */
    private Game added(PackageManager pm, File f, Map<String, Game> seen) {
        try {
            java.util.Set<String> abis = ApkScan.abis(f);
            PackageInfo pi = pm.getPackageArchiveInfo(f.getAbsolutePath(), 0);
            if (pi == null || pi.applicationInfo == null) throw new IllegalStateException("not a package");
            pi.applicationInfo.sourceDir = f.getAbsolutePath();
            pi.applicationInfo.publicSourceDir = f.getAbsolutePath();
            String pkg = pi.packageName;
            if (pkg.startsWith("com.retrobug.")) return null;  // RetroBug's own test APKs (the JNI bridge app) are not games
            if (!ApkScan.is32BitOnly(abis)) {
                if (seen.containsKey(pkg)) return null;  // the installed 32-bit game keeps its tile
                // The player chose this file: say why it is not playable here rather than drop it.
                String label = String.valueOf(pi.applicationInfo.loadLabel(pm));
                Game g = new Game(pkg, pkg, label, versionCode(pi), f, false, true, Game.State.NOT_CONVERTED);
                g.addedFile = f;
                return g.unplayable(abis.contains("arm64-v8a")
                                ? "This APK has 64-bit code, so this phone runs it as it is: install it normally."
                                : "This APK has no 32-bit ARM code for RetroBug to run: install it normally.");
            }
            boolean hasCode = (pi.applicationInfo.flags & ApplicationInfo.FLAG_HAS_CODE) != 0;
            Game have = seen.get(pkg);
            Game g;
            if (have != null && have.state == Game.State.UNREADABLE) {
                // A split install's tile: the added full APK is what a conversion reads. The C31 question is asked of
                // that APK in turn, because an added file cannot change what the launcher activity is.
                g = new Game(pkg, pkg, have.label, have.versionCode, f, true, true, stateOf(pm, pi));
                if (startsAsNativeActivity(f, pkg, have.launcher, abis)) g.unplayable(LibraryRules.NATIVE_ACTIVITY_WHY);
            } else if (have != null) {
                g = new Game(pkg, pkg, have.label, have.versionCode, have.apk, true, true, have.state);
                hasCode = have.hasCode;
                g.launcher = have.launcher;
            } else {
                String label = String.valueOf(pi.applicationInfo.loadLabel(pm));
                g = new Game(pkg, pkg, label, versionCode(pi), f, false, true, stateOf(pm, pi));
            }
            g.addedFile = f;
            g.hasCode = hasCode;
            return g;
        } catch (Exception e) {
            String name = f.getName().replaceAll("\\.apk$", "");
            Game g = new Game("file:" + f.getName(), name, name, 0, f, false, true, Game.State.UNREADABLE);
            g.addedFile = f;
            return g;
        }
    }

    /**
     * NOT_CONVERTED, READY, or UPDATE (made by an older runtime, or for another target than a conversion would give it
     * now: the game's setting, raised by the repackager's floors and never below the original's own target).
     */
    Game.State stateOf(PackageManager pm, PackageInfo original) {
        try {
            String pkg = original.packageName;
            PackageInfo copy = pm.getPackageInfo(pkg + ApkRepackager.RENAME_SUFFIX, PackageManager.GET_META_DATA);
            int stamp = copy.applicationInfo == null || copy.applicationInfo.metaData == null ? -1
                    : copy.applicationInfo.metaData.getInt(ApkRepackager.RUNTIME_META, -1);
            ApplicationInfo oa = original.applicationInfo;
            // A copy kept at 33 for its receivers (LibraryRules) is judged by the same rule, not as "out of date".
            boolean receivers = copy.applicationInfo != null && copy.applicationInfo.metaData != null
                    && "1".equals(copy.applicationInfo.metaData.getString(ApkRepackager.RECEIVERS_META));
            int wantTarget = oa == null ? settings.get(pkg).target
                    : LibraryRules.outputTarget(oa.targetSdkVersion, oa.minSdkVersion, settings.get(pkg).target, receivers);
            // M6: the copy was made with the patches the player has on now (fingerprints match; both "" when none).
            String hasPatches = copy.applicationInfo == null || copy.applicationInfo.metaData == null ? ""
                    : copy.applicationInfo.metaData.getString(ApkRepackager.PATCHES_META, "");
            String wantPatches = Patch.fingerprint(Patch.enabled(PatchStore.available(ctx, pkg), settings.get(pkg).patches));
            String hasMods = copy.applicationInfo == null || copy.applicationInfo.metaData == null ? ""
                    : copy.applicationInfo.metaData.getString(ApkRepackager.MODS_META, "");
            String wantMods = Mod.fingerprint(Mod.enabled(ModStore.available(ctx, pkg), settings.get(pkg).mods));
            String hasAspect = copy.applicationInfo == null || copy.applicationInfo.metaData == null ? "stretch"
                    : copy.applicationInfo.metaData.getString(ApkRepackager.ASPECT_META, "stretch");
            boolean current = stamp == ApkRepackager.RUNTIME_STAMP && copy.applicationInfo != null
                    && copy.applicationInfo.targetSdkVersion == wantTarget && hasPatches.equals(wantPatches)
                    && hasMods.equals(wantMods) && hasAspect.equals(settings.get(pkg).aspect);
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
