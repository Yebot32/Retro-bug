// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// On-device check of the targetSdk compatibility shims (`am start -S -n com.retrobug.launcher/.LauncherActivity
// -e shimcheck 1`, driven by tools/run_shimcheck.sh). It runs in the launcher's own process, which targets 34,
// so every behaviour change the shims undo is live here. Each behaviour gets two rows: one before the shims,
// which must see the platform's exception (so the check cannot pass on a platform that never enforced it), and
// one after com.retrobug.shim.Shims.apply with original target 8, which must not. The shims are process-wide
// and stay applied, which is why the script restarts the launcher (-S) and force-stops it afterwards.
//
// Every row logs one line on tag RBSHIM ("shimcheck: PASS|FAIL <row> — <detail>"), then a RESULT line.
package com.retrobug.launcher;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.net.Uri;
import android.os.IBinder;
import android.util.Log;

import com.retrobug.shim.Shims;

import java.io.File;
import java.net.InetAddress;
import java.util.Arrays;
import java.util.List;

final class ShimCheck {
    private ShimCheck() {}

    private static final int ORIGINAL_TARGET = 8;

    private interface Probe { Object run() throws Throwable; }

    /** What a probe ended in: the thrown exception's class name, or "returned". */
    private static String outcome(Probe p) {
        try {
            Object v = p.run();
            return "returned" + (v == null ? "" : " " + v);
        } catch (Throwable t) {
            return t.getClass().getName();
        }
    }

    private static int rows, failures;
    private static StringBuilder out;

    private static void row(String name, boolean ok, String detail) {
        rows++;
        if (!ok) failures++;
        String line = "shimcheck: " + (ok ? "PASS " : "FAIL ") + name + " — " + detail;
        Log.i(Shims.TAG, line);
        out.append(line).append('\n');
    }

    /** Must run on the main thread (StrictMode's thread policy is the main thread's). */
    static String run(final Activity a) {
        rows = 0; failures = 0; out = new StringBuilder();
        final ServiceConnection conn = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName n, IBinder b) {}
            @Override public void onServiceDisconnected(ComponentName n) {}
        };
        Probe bind = new Probe() { @Override public Object run() {
            boolean bound = a.bindService(new Intent("com.retrobug.shimcheck.NO_SUCH_SERVICE"), conn, Context.BIND_AUTO_CREATE);
            if (bound) a.unbindService(conn);
            return bound;
        } };
        Probe world = new Probe() { @Override public Object run() {
            @SuppressWarnings("deprecation") int mode = Context.MODE_WORLD_READABLE;
            a.getSharedPreferences("rbshimcheck", mode).edit().putInt("x", 1).commit();
            a.deleteSharedPreferences("rbshimcheck");
            return null;
        } };
        // A name lookup, not a socket: the launcher has no INTERNET permission, so a socket is refused (EACCES)
        // before StrictMode looks, while libcore's lookup asks StrictMode first. `.invalid` never resolves.
        Probe net = new Probe() { @Override public Object run() throws Throwable {
            return InetAddress.getByName("rbshimcheck.invalid");
        } };
        Probe fileUri = new Probe() { @Override public Object run() {
            a.startActivity(new Intent("com.retrobug.shimcheck.NO_SUCH_ACTIVITY")
                    .setDataAndType(Uri.fromFile(new File(a.getCacheDir(), "shimcheck.bin")), "application/x-rbshimcheck"));
            return null;
        } };

        // ---- before: the platform enforces each change at this process's target.
        int t0 = a.getApplicationInfo().targetSdkVersion;
        row("precondition: launcher targets >= 24", t0 >= 24, "targetSdkVersion " + t0);
        String o;
        o = outcome(bind);
        row("before: implicit bindService throws", o.equals("java.lang.IllegalArgumentException"), o);
        o = outcome(world);
        row("before: MODE_WORLD_READABLE throws", o.equals("java.lang.SecurityException"), o);
        o = outcome(net);
        row("before: main-thread network throws", o.equals("android.os.NetworkOnMainThreadException"), o);
        o = outcome(fileUri);
        row("before: file:// Uri in an Intent throws", o.equals("android.os.FileUriExposedException"), o);

        List<String> missing = Shims.missingDangerous(a, new String[] {
                "android.permission.CAMERA", "android.permission.INTERNET", "com.retrobug.NO_SUCH_PERMISSION"});
        row("permissions: only the ungranted dangerous one is missing",
                missing.equals(Arrays.asList("android.permission.CAMERA")), "missing " + missing);

        // ---- the shims, as a game targeting 8 gets them.
        List<String> lines = Shims.apply(a, ORIGINAL_TARGET, Arrays.asList(
                Shims.TARGET_SDK, Shims.STRICT_NETWORK, Shims.FILE_URI, Shims.PERMISSIONS));
        boolean noFail = true;
        for (String l : lines) { out.append("  ").append(l).append('\n'); if (l.contains("FAILED")) noFail = false; }
        row("shims applied without a FAILED line", noFail && lines.size() == 4, lines.size() + " lines");

        // ---- after: none of the four throws any more.
        int t1 = a.getApplicationInfo().targetSdkVersion;
        row("after: the activity's ApplicationInfo reads the original target", t1 == ORIGINAL_TARGET, "targetSdkVersion " + t1);
        o = outcome(bind);
        row("after: implicit bindService returns", o.startsWith("returned"), o);
        o = outcome(world);
        row("after: MODE_WORLD_READABLE works", o.startsWith("returned"), o);
        o = outcome(net);
        row("after: main-thread network reaches the lookup", !o.equals("android.os.NetworkOnMainThreadException")
                && (o.startsWith("returned") || o.startsWith("java.net.") || o.equals("java.lang.SecurityException")), o);
        o = outcome(fileUri);
        row("after: file:// Uri reaches the activity lookup", o.equals("android.content.ActivityNotFoundException"), o);

        String result = "RESULT: shimcheck " + rows + " rows, " + failures + " failures";
        Log.i(Shims.TAG, result);
        out.append(result).append('\n');
        return out.toString();
    }
}
