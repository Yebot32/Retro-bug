// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The targetSdk compatibility shims, executed inside a repackaged game's own process (RbShimProvider) and by
// the launcher's on-device check (`-e shimcheck 1`). Which shims run, and the target to restore, is decided
// at repackage time by com.retrobug.launcher.ShimPlan — see that file for why each exists. This side only
// carries them out and says what it did: every shim logs one line on tag RBSHIM, "shim <id>: ..." when it is
// applied and "shim <id> FAILED: ..." when the platform refused it, so a game that still breaks names the
// shim that did not hold.
//
// Compiled into two places: the launcher's classes.dex, and on its own (d8 --min-api 24) into
// assets/rbshim.dex, which the repackager adds to a game as an extra classesN.dex. Public API only, API <= 24.
package com.retrobug.shim;

import android.app.Activity;
import android.app.Application;
import android.content.ComponentCallbacks;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.StrictMode;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

public final class Shims {
    public static final String TAG = "RBSHIM";
    public static final String TARGET_SDK = "target-sdk";
    public static final String STRICT_NETWORK = "strict-network";
    public static final String FILE_URI = "file-uri";
    public static final String PERMISSIONS = "permissions";
    static final int PERMISSION_REQUEST = 0x5242;

    private Shims() {}

    private static void say(List<String> out, String line) {
        Log.i(TAG, line);
        if (out != null) out.add(line);
    }

    /**
     * Runs the listed shims (unknown ids are named and skipped). Must be called on the main thread: the
     * StrictMode thread policy it relaxes is the main thread's own.
     */
    public static List<String> apply(Context ctx, int targetSdk, List<String> ids) {
        List<String> out = new ArrayList<>();
        for (String id : ids) {
            try {
                switch (id) {
                    case TARGET_SDK: targetSdk(ctx, targetSdk, out); break;
                    case STRICT_NETWORK: strictNetwork(out); break;
                    case FILE_URI: fileUri(out); break;
                    case PERMISSIONS: permissions(ctx, out); break;
                    default: say(out, "shim " + id + " FAILED: unknown shim id (newer repackager than runtime?)");
                }
            } catch (Throwable t) {
                say(out, "shim " + id + " FAILED: " + t);
            }
        }
        return out;
    }

    // ---------------------------------------------------------------- target-sdk

    /**
     * Sets the process's ApplicationInfo.targetSdkVersion. Context.getApplicationInfo() of the application,
     * its activities and services is one shared object (LoadedApk's), which is what the in-process checks read.
     * LoadedApk swaps in a fresh copy when the system pushes an application-info update (overlay or theme
     * changes), so the value is re-asserted on every configuration change and activity creation.
     */
    static void targetSdk(Context ctx, final int target, List<String> out) {
        final Context app = ctx.getApplicationContext() != null ? ctx.getApplicationContext() : ctx;
        ApplicationInfo ai = app.getApplicationInfo();
        int was = ai.targetSdkVersion;
        ai.targetSdkVersion = target;
        say(out, "shim " + TARGET_SDK + ": in-process targetSdk " + was + " -> " + target
                + " (implicit service intents, MODE_WORLD_*, legacy compat paths)");
        if (app instanceof Application) {
            final Runnable reassert = new Runnable() {
                @Override public void run() {
                    ApplicationInfo now = app.getApplicationInfo();
                    if (now.targetSdkVersion != target) {
                        Log.i(TAG, "shim " + TARGET_SDK + ": re-applied (" + now.targetSdkVersion + " -> " + target
                                + ", ApplicationInfo was replaced)");
                        now.targetSdkVersion = target;
                    }
                }
            };
            ((Application) app).registerComponentCallbacks(new ComponentCallbacks() {
                @Override public void onConfigurationChanged(Configuration c) { reassert.run(); }
                @Override public void onLowMemory() {}
            });
            ((Application) app).registerActivityLifecycleCallbacks(new Lifecycle() {
                @Override public void onActivityCreated(Activity a, Bundle b) { reassert.run(); }
                @Override public void onActivityResumed(Activity a) { reassert.run(); }
            });
        }
    }

    // ---------------------------------------------------------------- strict-network / file-uri

    static void strictNetwork(List<String> out) {
        StrictMode.setThreadPolicy(new StrictMode.ThreadPolicy.Builder(StrictMode.getThreadPolicy())
                .permitNetwork().build());
        say(out, "shim " + STRICT_NETWORK + ": main-thread network allowed (no NetworkOnMainThreadException)");
    }

    static void fileUri(List<String> out) {
        // VmPolicy.Builder has no public way to drop one detection, and file-URI exposure is the only VM
        // detection the platform turns on for a release app at this target, so LAX loses nothing else.
        StrictMode.setVmPolicy(StrictMode.VmPolicy.LAX);
        say(out, "shim " + FILE_URI + ": file:// Uris may leave the process (no FileUriExposedException)");
    }

    // ---------------------------------------------------------------- permissions

    /** The dangerous permissions among {@code requested} that this process does not hold. */
    public static List<String> missingDangerous(Context ctx, String[] requested) {
        List<String> missing = new ArrayList<>();
        if (requested == null) return missing;
        PackageManager pm = ctx.getPackageManager();
        for (String p : requested) {
            try {
                PermissionInfo pi = pm.getPermissionInfo(p, 0);
                if ((pi.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE) != PermissionInfo.PROTECTION_DANGEROUS)
                    continue;
            } catch (PackageManager.NameNotFoundException e) {
                continue;                                  // a permission no one defines on this device
            }
            if (ctx.checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) missing.add(p);
        }
        return missing;
    }

    /**
     * Asks for the declared dangerous permissions the original target got at install time. Asked from the
     * first activity of each process, as soon as it is created; the platform itself stops showing the dialog
     * for a permission the user has refused for good.
     */
    static void permissions(Context ctx, List<String> out) throws PackageManager.NameNotFoundException {
        final Context app = ctx.getApplicationContext() != null ? ctx.getApplicationContext() : ctx;
        PackageInfo pi = app.getPackageManager().getPackageInfo(app.getPackageName(), PackageManager.GET_PERMISSIONS);
        final List<String> missing = missingDangerous(app, pi.requestedPermissions);
        if (missing.isEmpty()) {
            say(out, "shim " + PERMISSIONS + ": no dangerous permission missing");
            return;
        }
        if (!(app instanceof Application)) {
            say(out, "shim " + PERMISSIONS + " FAILED: no Application to watch for the first activity; missing " + missing);
            return;
        }
        say(out, "shim " + PERMISSIONS + ": will ask at the first activity for " + missing);
        ((Application) app).registerActivityLifecycleCallbacks(new Lifecycle() {
            boolean asked;
            @Override public void onActivityCreated(Activity a, Bundle b) {
                if (asked) return;
                asked = true;
                try {
                    List<String> still = missingDangerous(a, missing.toArray(new String[0]));
                    if (still.isEmpty()) return;
                    a.requestPermissions(still.toArray(new String[0]), PERMISSION_REQUEST);
                    Log.i(TAG, "shim " + PERMISSIONS + ": asked " + still + " from " + a.getClass().getName());
                } catch (Throwable t) {
                    Log.i(TAG, "shim " + PERMISSIONS + " FAILED: " + t);
                }
            }
        });
    }

    /** Empty ActivityLifecycleCallbacks to override from. */
    static class Lifecycle implements Application.ActivityLifecycleCallbacks {
        @Override public void onActivityCreated(Activity a, Bundle b) {}
        @Override public void onActivityStarted(Activity a) {}
        @Override public void onActivityResumed(Activity a) {}
        @Override public void onActivityPaused(Activity a) {}
        @Override public void onActivityStopped(Activity a) {}
        @Override public void onActivitySaveInstanceState(Activity a, Bundle b) {}
        @Override public void onActivityDestroyed(Activity a) {}
    }
}
