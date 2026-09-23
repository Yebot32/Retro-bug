/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The Activity that carries the host-side launcher: RetroBug's engine, running inside an installed app,
 * launching an `armeabi-v7a` library out of an APK and reporting whether it got the hardware's answer.
 *
 * Java's whole job here is to move bytes and to put the report somewhere a person can read it — a screen
 * for a run nobody is watching, and logcat for a run watched over `adb` (the C++ lane's own harness reads
 * `adb logcat -s RBLAUNCH`, so the log lines are part of the interface and not a debugging aid). Everything
 * that decides PASS or FAIL is in `android/jni/launcher.cpp`, because the numbers it compares against come
 * from the engine's own state and there is no honest way to hand them to Java and have them judged
 * somewhere else.
 *
 * The one thing the *screen* decides, and it decides it because only the platform can answer it, is the
 * line about 32-bit support. `Build.SUPPORTED_32_BIT_ABIS` is empty exactly when the OS cannot execute
 * `armeabi-v7a` code at all, and that is the difference between "the guest ARMv7 bytes ran" and "the guest
 * ARMv7 bytes ran *in this process*". On a 32-bit-capable phone a PASS is the engine agreeing with the
 * device; on a 64-bit-only phone it is the only way the instructions could have run at all. The banner
 * says which phone is which rather than leaving the reader to know.
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
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

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
    private TextView verdict_;
    private TextView facts_;
    private Button run_;
    private Button copy_;
    private final StringBuilder report = new StringBuilder();
    private volatile boolean running_;
    private int pass_, fail_;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        setContentView(buildUi());
        facts_.setText(deviceFacts());
        setVerdict("not run yet", Color.DKGRAY);
        // Runs on launch as well as from the button: an attended look at the phone and an unattended
        // `adb shell am start` + logcat read are the same run, and requiring a tap would break the second.
        startRun();
    }

    /// A label, the device facts, a verdict banner, a Run button, and the full report underneath. Built in
    /// code rather than from a layout file so this app keeps the same `aapt` shape the probe app has —
    /// one manifest, no resources, and nothing between the installed APK and the engine's own output.
    private View buildUi() {
        int pad = dp(10);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.WHITE);

        TextView title = new TextView(this);
        title.setText("RetroBug");
        title.setTextSize(22f);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setPadding(pad, pad, pad, 0);
        root.addView(title);

        TextView subtitle = new TextView(this);
        subtitle.setText("32-bit ARM (armeabi-v7a) guest code, executed by RetroBug inside this app");
        subtitle.setTextSize(12f);
        subtitle.setPadding(pad, 0, pad, dp(6));
        root.addView(subtitle);

        facts_ = new TextView(this);
        facts_.setTextSize(12f);
        facts_.setTypeface(Typeface.MONOSPACE);
        facts_.setPadding(pad, 0, pad, dp(6));
        root.addView(facts_);

        verdict_ = new TextView(this);
        verdict_.setTextSize(18f);
        verdict_.setTypeface(Typeface.DEFAULT_BOLD);
        verdict_.setPadding(pad, dp(4), pad, dp(4));
        root.addView(verdict_);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setPadding(pad, 0, pad, dp(4));
        run_ = new Button(this);
        run_.setText("Run test");
        run_.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startRun();
            }
        });
        buttons.addView(run_, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        copy_ = new Button(this);
        copy_.setText("Copy report");
        copy_.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                ClipboardManager cb = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
                if (cb != null) cb.setPrimaryClip(ClipData.newPlainText("RetroBug", report.toString()));
                Toast.makeText(LauncherActivity.this, "Report copied", Toast.LENGTH_SHORT).show();
            }
        });
        buttons.addView(copy_, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        root.addView(buttons);

        ScrollView scroll = new ScrollView(this);
        report_ = new TextView(this);
        report_.setTextSize(11f);
        report_.setTypeface(Typeface.MONOSPACE);
        report_.setTextColor(Color.BLACK);
        report_.setTextIsSelectable(true);
        report_.setPadding(pad, 0, pad, pad);
        scroll.addView(report_);
        root.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        return root;
    }

    /// The platform's own answer to "can this phone run 32-bit ARM code", which is what makes the verdict
    /// mean something rather than only look green.
    private String deviceFacts() {
        String[] abis32 = Build.SUPPORTED_32_BIT_ABIS;
        boolean canRunNative32 = abis32 != null && abis32.length > 0;
        String note = canRunNative32
                ? "this device CAN run 32-bit ARM natively, so a PASS below shows the engine agrees with"
                        + " the phone -- it is not proof of emulation. A 64-bit-only phone is."
                : "this device CANNOT run 32-bit ARM code at all, so every ARMv7 instruction that produced"
                        + " the values below was executed by RetroBug in this process.";
        return Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE
                + " (sdk " + Build.VERSION.SDK_INT + ")\n"
                + "page size   : " + pageSize() + "\n"
                + "supported   : " + Arrays.toString(Build.SUPPORTED_ABIS) + "\n"
                + "32-bit abis : " + (canRunNative32 ? Arrays.toString(abis32) : "none") + "\n"
                + "=> " + note;
    }

    private int pageSize() {
        // Not Build.*: the page size is queried from the OS because a wrong one changes every memory
        // constant the engine seeds, and the C++ lanes print the same number for the same reason.
        return (int) android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE);
    }

    private void startRun() {
        if (running_) return;
        running_ = true;
        run_.setEnabled(false);
        report.setLength(0);
        pass_ = 0;
        fail_ = 0;
        report_.setText("");
        setVerdict("running...", Color.rgb(0xB0, 0x60, 0x00));
        line("device=" + Build.MANUFACTURER + " " + Build.MODEL + " sdk=" + Build.VERSION.SDK_INT
                + " abis=" + Arrays.toString(Build.SUPPORTED_ABIS));
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    runLaunches();
                } finally {
                    running_ = false;
                    runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            run_.setEnabled(true);
                        }
                    });
                }
            }
        }, "rb-launch").start();
    }

    private void runLaunches() {
        byte[] guestSo;
        byte[] libm;
        try {
            guestSo = readGuestSo();
            libm = readAll(getAssets().open(LIBM_ASSET));
        } catch (IOException | RuntimeException e) {
            line("FATAL: could not read the guest images: "
                    + e.getClass().getSimpleName() + ": " + e.getMessage());
            setVerdict("FAILED to start", Color.rgb(0xC0, 0x20, 0x20));
            return;
        }
        line("assets: " + GUEST_SO_ENTRY + "=" + guestSo.length
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
            for (String one : (r == null ? "FATAL: nativeLaunch returned null" : r).split("\n")) {
                if (one.endsWith("PASS")) ++pass_;
                if (one.endsWith("FAIL")) ++fail_;
            }
            line(r == null ? "FATAL: nativeLaunch returned null" : r);
        }
        line("=== launcher done ===");
        if (fail_ == 0 && pass_ > 0) {
            setVerdict(pass_ + " PASS, 0 FAIL", Color.rgb(0x18, 0x80, 0x18));
        } else {
            setVerdict(pass_ + " PASS, " + fail_ + " FAIL", Color.rgb(0xC0, 0x20, 0x20));
        }
    }

    /**
     * Pulls the guest library out of the APK in the assets.
     *
     * Throws rather than returning null on a missing entry: an empty `byte[]` would reach the engine,
     * which would report "not an ELF32 ARM image", and the real fault — the packaging step never put the
     * library in the asset — would read as an engine defect.
     */
    private byte[] readGuestSo() throws IOException {
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
    private void line(String text) {
        String body = text == null ? "null" : text;
        for (String one : body.split("\n")) {
            if (!one.isEmpty()) Log.i(TAG, one);
        }
        report.append(body).append('\n');
        final String all = report.toString();
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                report_.setText(all);
            }
        });
    }

    private void setVerdict(final String text, final int colour) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                verdict_.setText(text);
                verdict_.setTextColor(colour);
            }
        });
    }

    private int dp(int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
                getResources().getDisplayMetrics());
    }
}
