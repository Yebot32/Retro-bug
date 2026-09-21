/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The Activity that carries the host-side launcher: RetroBug's engine, running inside an installed app,
 * launching an `armeabi-v7a` library out of an APK and reporting whether it got the hardware's answer.
 *
 * Java's whole job here is to move bytes and to make the report readable twice — once in logcat, for a
 * run watched over `adb logcat`, and once on the screen, for a run nobody is watching. Everything that
 * decides PASS or FAIL is in `android/jni/launcher.cpp`, because the numbers it compares against come
 * from the engine's own state and there is no honest way to hand them to Java and have them judged
 * somewhere else.
 *
 * Two things this file is careful not to assume:
 *
 *   - That the guest library can be found on disk. An installed app cannot point a loader at its own
 *     APK, so the bytes come out of the asset as a `byte[]` and cross JNI as an array. The asset is a zip
 *     inside a zip, and `ZipInputStream` over `getAssets().open()` reads it whichever way either level
 *     was compressed — which is worth saying out loud, because the C++ lane's own zip reader handles
 *     stored entries only (`core/tests/guest_apk_test.cpp`) and the two readers are not interchangeable.
 *   - That the work fits the main thread. Six launches, two tiers each, through an interpreter, is not a
 *     millisecond-scale job, and an ANR dialog half-way through would kill the process before the report
 *     existed. So the launches run on one worker thread and the screen is updated after each seed, which
 *     means a run that dies part-way still leaves on screen the part that did not.
 */
package com.retrobug.launcher;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Arrays;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class LauncherActivity extends Activity {
    private static final String TAG = "RBLAUNCH";
    /// The two guest images, staged by `tools/build_launcher_apk.sh`. The inner APK is the *same file*
    /// `tools/run_engine_tests.sh` pushes to `/data/local/tmp/rb_guestprobe.apk` for the C++ lane, so a
    /// disagreement between the two lanes cannot be blamed on different inputs.
    private static final String GUEST_APK_ASSET = "guest/rb_guestprobe.apk";
    private static final String GUEST_SO_ENTRY = "lib/armeabi-v7a/libmathprobe.so";
    private static final String LIBM_ASSET = "guest/libm.so";
    /// The seeds `kHardware` in `android/jni/launcher.cpp` carries answers for. A seed outside that list
    /// still runs and still prints its value; only the PASS/FAIL column is refused.
    private static final int[] SEEDS = {0, 1, 4, 7, 100, -3};
    private static final String METHOD = "mathProbe";

    static {
        System.loadLibrary("rbguest");
    }

    private static native String nativeLaunch(byte[] guestSo, byte[] libmSo, String method, int seed);

    private TextView report_;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        report_ = new TextView(this);
        report_.setTextSize(11f);
        report_.setTypeface(android.graphics.Typeface.MONOSPACE);
        report_.setTextColor(0xFF000000);
        report_.setBackgroundColor(0xFFFFFFFF);
        int pad = (int) (8 * getResources().getDisplayMetrics().density);
        report_.setPadding(pad, pad, pad, pad);
        setContentView(report_);

        final StringBuilder report = new StringBuilder();
        report.append("device=").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(" sdk=").append(Build.VERSION.SDK_INT)
                .append(" abis=").append(Arrays.toString(Build.SUPPORTED_ABIS)).append('\n');
        show(report);

        // One thread for all six launches. See the header for what an ANR would cost.
        new Thread(new Runnable() {
            @Override
            public void run() {
                runLaunches(report);
            }
        }, "rb-launch").start();
    }

    private void runLaunches(StringBuilder report) {
        byte[] guestSo;
        byte[] libm;
        try {
            guestSo = readGuestSo(report);
            libm = readAll(getAssets().open(LIBM_ASSET));
        } catch (IOException | RuntimeException e) {
            line(report, "FATAL: could not read the guest images: "
                    + e.getClass().getSimpleName() + ": " + e.getMessage());
            return;
        }
        line(report, "assets: " + GUEST_SO_ENTRY + "=" + guestSo.length
                + "B " + LIBM_ASSET + "=" + libm.length + "B");
        for (int seed : SEEDS) {
            String r;
            try {
                r = nativeLaunch(guestSo, libm, METHOD, seed);
            } catch (Throwable t) {
                // A native SIGSEGV is not catchable and will take the process; what reaches here is a
                // Java-side throw such as an allocation failure, and one bad seed should still print the
                // five that worked.
                r = "FATAL: nativeLaunch threw " + t.getClass().getName() + ": " + t.getMessage();
            }
            line(report, r == null ? "FATAL: nativeLaunch returned null" : r);
        }
        line(report, "=== launcher done ===");
    }

    /**
     * Pulls the guest library out of the APK in the assets.
     *
     * Throws rather than returning null on a missing entry: an empty `byte[]` would reach the engine,
     * which would report "not an ELF32 ARM image", and the real fault — the packaging step never put the
     * library in the asset — would read as an engine defect.
     */
    private byte[] readGuestSo(StringBuilder report) throws IOException {
        InputStream in = getAssets().open(GUEST_APK_ASSET);
        try {
            ZipInputStream zip = new ZipInputStream(in);
            try {
                for (ZipEntry e = zip.getNextEntry(); e != null; e = zip.getNextEntry()) {
                    if (GUEST_SO_ENTRY.equals(e.getName())) return readAll(zip);
                }
            } finally {
                zip.close();
            }
        } finally {
            in.close();
        }
        throw new IOException(GUEST_SO_ENTRY + " is not in " + GUEST_APK_ASSET);
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[16384];
        try {
            for (int n = in.read(buf); n > 0; n = in.read(buf)) out.write(buf, 0, n);
        } finally {
            in.close();
        }
        return out.toByteArray();
    }

    /// One entry of the report: logged line by line so `adb logcat -s RBLAUNCH` reads like the test
    /// lane's stdout, and shown whole so a run with no cable attached is still readable.
    private void line(StringBuilder report, String text) {
        for (String one : text.split("\n")) {
            if (!one.isEmpty()) Log.i(TAG, one);
        }
        report.append(text).append('\n');
        show(report);
    }

    private void show(final StringBuilder report) {
        final String text = report.toString();
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                report_.setText(text);
            }
        });
    }
}
