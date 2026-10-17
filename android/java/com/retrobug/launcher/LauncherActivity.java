/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The launcher app: a home screen, a library you add APKs to, settings, and a diagnostics screen that
 * keeps the engine self-test the earlier version of this file was entirely made of.
 *
 * The shape of it is a decision about honesty as much as about UI. RetroBug today executes a guest's
 * 32-bit ARM *native* code: it loads an `armeabi-v7a` library, links it against the guest `libm` it ships,
 * and can call an entry point through the emulator. It does not run a whole Android app — there is no guest
 * libc, no DEX, no JavaVM, and a game that needs any of those stops early. So the library screen says what
 * "run" does and what it found, rather than showing a spinner and a fake progress bar.
 *
 * Screens are built in code rather than from layouts, and there is exactly one Activity, because
 * `tools/build_launcher_apk.sh` compiles this package's sources with `javac` (plus apksig, for the
 * repackager's signing) and packages them with `aapt2 link` against a manifest with no layouts. The
 * repackager's pieces (ApkRepackager, AxmlEditor, RepackSigner, RepackInstaller) are separate files.
 *
 * Two things this file is careful not to assume:
 *
 *   - That a whole APK can be held in memory. It cannot, usefully: games are tens or hundreds of MB, and
 *     the only part RetroBug can execute is one `.so` inside `lib/armeabi-v7a/`. So the file is streamed
 *     through `ZipInputStream` and only the entry that will be executed is read out.
 *   - That a chosen APK has anything to run. The common failure on a modern store download is an APK that
 *     ships only `arm64-v8a`, and "it did not work" is a useless thing to be told. The report names which
 *     ABIs the APK actually carries.
 */
package com.retrobug.launcher;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.FrameLayout;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class LauncherActivity extends Activity {
    private static final String TAG = "RBLAUNCH";
    private static final String PREFS = "rb";
    private static final String PREF_TIER = "tier";               // 0 = both, 1 = interpreter, 2 = JIT
    private static final String PREF_FULL_REPORT = "full_report"; // show every report line, or the head+tail
    private static final String PREF_AUTORUN = "autorun";         // run the self-test when its screen opens

    /// The guest images the launcher ships. The inner APK is the *same file*
    /// `tools/run_engine_tests.sh` pushes to `/data/local/tmp/rb_guestprobe.apk` for the C++ lane, so a
    /// disagreement between the two lanes cannot be blamed on different inputs. `libc.so` is the device's
    /// own bionic and may be absent from a build — the native side then reports the run without it, which
    /// is why `readAssetOrEmpty` exists rather than a fatal read.
    private static final String GUEST_APK_ASSET = "guest/rb_guestprobe.apk";
    private static final String GUEST_SO_ENTRY = "lib/armeabi-v7a/libmathprobe.so";
    private static final String LIBM_ASSET = "guest/libm.so";
    private static final String LIBC_ASSET = "guest/libc.so";
    /// The device's own 32-bit `libstdc++.so` (13 exports: operator new/delete, the guard functions,
    /// `__cxa_pure_virtual`), which pre-r8 NDK games such as Gumball 2013 name in DT_NEEDED. Optional like libc.
    private static final String LIBSTDCXX_ASSET = "guest/libstdc++.so";
    private static final String LIBZ_ASSET = "guest/libz.so";
    private static final int[] SEEDS = {0, 1, 4, 7, 100, -3};
    private static final String METHOD = "mathProbe";
    /// An entry this size or larger is refused with a message rather than an OutOfMemoryError: a guest
    /// library that big is not a phone library, and the failure should name itself.
    private static final long MAX_SO_BYTES = 64L * 1024 * 1024;

    static {
        System.loadLibrary("rbguest");
    }

    private static native String nativeLaunch(byte[] guestSo, byte[] libmSo, byte[] libcSo, byte[] libzSo,
                                              String method, int seed);

    /// The running report's live status: the line count, a newline, then the latest line. Polled while a run is going.
    private static native String nativeProgress();

    /// One touch on the run screen, raw (getAction, getX, getY, the view's width, the time in ms): the native
    /// side maps it exactly as the game's own `onTouchEvent` would and queues it for the frame loop.
    private static native boolean nativeTouch(int action, float x, float y, int viewWidth, long timeMs);

    private static native String nativeInspect(byte[] guestSo, byte[] libmSo, byte[] libcSo, byte[] libzSo,
                                               byte[] libstdcxxSo,
                                               String label, int tiers, String apkPath, int surfaceWidth,
                                               int surfaceHeight, Surface surface);

    /// M1's in-process boot, checked from here: calls `rb_runtime_boot` (librbguest.so) on an APK laid out
    /// as the repackager lays one out (`assets/rb32/<lib>`, `assets/rb32sys/*`) and returns what it did.
    /// The report itself goes to logcat tag RBRUNTIME and to files/rb-report.txt.
    private static native String nativeBootCheck(String apkPath, String libName);
    /// `-e stubcheck`: dlopen one generated stub here and check each of `symbols` reaches rb_stub_capture.
    private static native String nativeStubCheck(String stubPath, String[] symbols);

    private enum Screen { HOME, PREFS, DEV, LIBRARY, SETTINGS, DIAGNOSTICS, REPORT, RUN }

    private Screen screen_ = Screen.HOME;
    private String reportTitle_ = "";

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().setStatusBarColor(HoloTheme.PANEL);
        getWindow().setNavigationBarColor(0xFF000000);
        boolean scripted = getIntent() != null && getIntent().getExtras() != null && !getIntent().getExtras().isEmpty();
        if (saved == null && !scripted) {
            // A player's start: the boot animation, then the library. Scripted starts (`-e ...`) skip it.
            setContentView(new BootScreen(this, new Runnable() {
                @Override public void run() { show(Screen.HOME); }
            }));
        } else {
            show(Screen.HOME);
        }
        /*
         * An unattended run, so that a test does not depend on tapping the right pixel.
         *
         * `am start -n com.retrobug.launcher/.LauncherActivity -e run angry` opens the first game whose
         * file name contains "angry" and runs it, with no UI in the loop. Driving this by taps was how the
         * previous rounds were tested, and twice the taps landed on the screen the app happened to be
         * resuming on rather than the one intended — each time costing a round trip and, once, producing a
         * "nothing happened" reading of a run that never started. A named intent cannot miss.
         */
        /*
         * `-e bootcheck <name>` resolves a staged game like `-e run` does, but runs M1's in-process boot
         * (`rb_runtime_boot`) on it instead of the launcher's own path. The APK must carry the repackaged
         * layout (`tools/make_bootcheck_apk.sh` makes one); `-e lib <libX.so>` picks the library, else the
         * largest `assets/rb32/*.so`.
         */
        /*
         * `-e repack <name>` runs the library row's "Make installable" on the first matching game: rewrite,
         * sign, keep the result under getExternalFilesDir(null)/repack/, then hand it to the installer.
         * `-e install 0` stops after signing (scripted checks of the artifact without an installer dialog).
         * `-e rename 1` renames the package to `<original>.rb` (`-e rename <pkg>`: that name) so the result installs
         * beside a natively installed original; it is kept as `<name>.rb.apk` (or `<name>.<pkg>.apk`).
         */
        /*
         * `-e shimcheck 1` checks the targetSdk compatibility shims in this process (ShimCheck; driven by
         * tools/run_shimcheck.sh). The shims stay applied to the process afterwards, so the script restarts
         * the launcher for it and force-stops it after.
         */
        if (getIntent() != null && getIntent().getStringExtra("shimcheck") != null) {
            saveAndShow("targetSdk shim check", ShimCheck.run(this).trim());
            return;
        }
        /*
         * `-e stage <file.apk>` adds getExternalFilesDir(null)/<file.apk> (where `adb push` can put it) to the
         * library before anything else in this intent runs: the scripted twin of "Add game". It copies on this
         * thread, which is fine for a scripted run and would not be for the picker.
         */
        final String stage = getIntent() == null ? null : getIntent().getStringExtra("stage");
        if (stage != null && !stage.isEmpty()) {
            File src = new File(getExternalFilesDir(null), new File(stage).getName());
            File dest = new File(gamesDir(), src.getName().replaceAll("[^A-Za-z0-9._-]", "_"));
            try (InputStream in = new java.io.FileInputStream(src); OutputStream out = new FileOutputStream(dest)) {
                byte[] buf = new byte[1 << 16];
                for (int n = in.read(buf); n > 0; n = in.read(buf)) out.write(buf, 0, n);
                Log.i(TAG, "stage: added " + dest + " (" + dest.length() + " bytes)");
            } catch (IOException e) {
                Log.e(TAG, "stage: could not copy " + src + ": " + e.getMessage());
            }
        }
        final String repack = getIntent() == null ? null : getIntent().getStringExtra("repack");
        if (repack != null && !repack.isEmpty()) {
            for (File f : games()) {
                if (f.getName().toLowerCase().contains(repack.toLowerCase())) {
                    ApkRepackager.debuggable = "1".equals(getIntent().getStringExtra("debuggable"));
                    ApkRepackager.nativeControl = "1".equals(getIntent().getStringExtra("native"));
                    String target = getIntent().getStringExtra("target");
                    ApkRepackager.targetFloor = target == null ? 0 : Integer.parseInt(target);
                    String rename = getIntent().getStringExtra("rename");
                    repackGame(f, !"0".equals(getIntent().getStringExtra("install")),
                            rename == null || rename.equals("0") ? null
                                    : rename.equals("1") ? ApkRepackager.RENAME_SUFFIX : rename);
                    return;
                }
            }
            Log.w(TAG, "repack: no game matching '" + repack + "'");
            Toast.makeText(this, "no game matching '" + repack + "'", Toast.LENGTH_LONG).show();
            return;
        }
        // `-e stubcheck <name>`: repack the game (no install, nothing signed), then dlopen each generated stub in
        // this process and check every export resolves to the dispatch entry (nativeStubCheck).
        final String stubcheck = getIntent() == null ? null : getIntent().getStringExtra("stubcheck");
        if (stubcheck != null && !stubcheck.isEmpty()) {
            for (File f : games()) {
                if (f.getName().toLowerCase().contains(stubcheck.toLowerCase())) {
                    stubCheck(f);
                    return;
                }
            }
            Log.w(TAG, "stubcheck: no game matching '" + stubcheck + "'");
            return;
        }
        final String bootcheck = getIntent() == null ? null : getIntent().getStringExtra("bootcheck");
        if (bootcheck != null && !bootcheck.isEmpty()) {
            // `-e bootcheck2 <name>`: a second boot right after the first, in this same process — the
            // in-process runtime's guest process is process-lifetime, so the second must reuse its scope.
            final String second = getIntent().getStringExtra("bootcheck2");
            File secondApk = null;
            if (second != null && !second.isEmpty()) {
                for (File f : games()) {
                    if (f.getName().toLowerCase().contains(second.toLowerCase())) {
                        secondApk = f;
                        break;
                    }
                }
                if (secondApk == null) Log.w(TAG, "bootcheck2: no game matching '" + second + "'");
            }
            for (File f : games()) {
                if (f.getName().toLowerCase().contains(bootcheck.toLowerCase())) {
                    bootCheck(f, getIntent().getStringExtra("lib"), secondApk);
                    return;
                }
            }
            Log.w(TAG, "bootcheck: no game matching '" + bootcheck + "'");
            Toast.makeText(this, "no game matching '" + bootcheck + "'", Toast.LENGTH_LONG).show();
            return;
        }
        final String run = getIntent() == null ? null : getIntent().getStringExtra("run");
        if (run == null || run.isEmpty()) return;
        /*
         * `-e tier <0|1|2>` overrides the Settings choice for this run only (0 both, 1 interpreter, 2 JIT).
         * It exists because the tier is the one variable whose *cost* differs by orders of magnitude and
         * the report's step budget is spent very differently by the two: a constructor that looks stuck on
         * the interpreter can be a few milliseconds of work on the JIT, and telling "slow" from "looping"
         * needs the second measurement rather than a bigger budget on the first.
         */
        final String tier = getIntent().getStringExtra("tier");
        int chosen = getSharedPreferences(PREFS, MODE_PRIVATE).getInt(PREF_TIER, 0);
        if (tier != null) {
            try {
                chosen = Integer.parseInt(tier.trim());
            } catch (NumberFormatException ignored) {
                // A malformed extra falls back to the stored preference rather than failing the run.
            }
        }
        for (File f : games()) {
            if (f.getName().toLowerCase().contains(run.toLowerCase())) {
                runGame(f, chosen);
                return;
            }
        }
        Toast.makeText(this, "no game matching '" + run + "'", Toast.LENGTH_LONG).show();
    }

    @Override
    public void onBackPressed() {
        if (screen_ == Screen.HOME) {
            super.onBackPressed();
        } else {
            show(Screen.HOME);
        }
    }

    // ------------------------------------------------------------------ navigation

    private void show(Screen which) {
        screen_ = which;
        switch (which) {
            case HOME: setContentView(library()); break;
            case DEV: setContentView(homeScreen()); break;
            case PREFS: setContentView(prefs()); break;
            case LIBRARY: setContentView(libraryScreen()); break;
            case SETTINGS: setContentView(settingsScreen()); break;
            case DIAGNOSTICS: setContentView(diagnosticsScreen()); break;
            case REPORT: setContentView(reportScreen()); break;
            case RUN: setContentView(runScreen()); break;
        }
    }

    // ------------------------------------------------------------------ screens

    /// Background work for the player's screens (the catalog scan, icon and art decoding): never the UI thread.
    private final java.util.concurrent.ExecutorService bg_ = java.util.concurrent.Executors.newFixedThreadPool(2);
    private LibraryScreen library_;

    private View library() {
        library_ = new LibraryScreen(this, bg_, new LibraryScreen.Host() {
            @Override public void addApk() { pickApk(); }
            @Override public void openSettings() { show(Screen.PREFS); }
        });
        return library_;
    }

    private View prefs() {
        return new SettingsScreen(this, new SettingsScreen.Host() {
            @Override public void back() { show(Screen.HOME); }
            @Override public void openDeveloper() { show(Screen.DEV); }
            @Override public void restyle() { show(Screen.PREFS); }
        });
    }

    /// The developer corner (Settings > Developer tools, revealed by seven taps on the version): the engine self-test,
    /// the file-picker library with its scripted run modes, reports and diagnostics, as they were before the redesign.
    private View homeScreen() {
        LinearLayout root = column();
        root.addView(title("RetroBug", 26f, true));
        root.addView(text("Runs 32-bit ARM (armeabi-v7a) code on a 64-bit phone, by emulating the CPU"
                + " rather than translating API calls.", 13f, false));
        root.addView(space(8));

        root.addView(text(buildFacts(), 12f, true));
        root.addView(space(12));

        root.addView(button("My games" + (games().isEmpty() ? "" : "  (" + games().size() + ")"), new Runnable() {
            @Override public void run() { show(Screen.LIBRARY); }
        }));
        root.addView(button("Settings", new Runnable() {
            @Override public void run() { show(Screen.SETTINGS); }
        }));
        root.addView(button("Engine self-test", new Runnable() {
            @Override public void run() { show(Screen.DIAGNOSTICS); }
        }));
        root.addView(space(12));
        root.addView(text("The self-test runs the same 32-bit probe library that RetroBug's own test lanes"
                + " run on this machine, on both execution tiers, and compares the values against what the"
                + " bytes produce on real hardware.", 11f, false));
        return scroll(root);
    }

    private View libraryScreen() {
        LinearLayout root = column();
        root.addView(title("My games", 22f, true));
        root.addView(text("Add an APK. RetroBug reads its lib/armeabi-v7a library, loads it into the"
                + " emulator, and calls the library's JNI entry point if it exports one.", 12f, false));
        root.addView(space(8));
        root.addView(button("Add a game (.apk)", new Runnable() {
            @Override public void run() { pickApk(); }
        }));
        root.addView(space(8));

        List<File> list = games();
        if (list.isEmpty()) {
            root.addView(text("Nothing added yet.\n\nWhat happens when you add one: RetroBug opens the APK"
                    + " as a zip, finds lib/armeabi-v7a/*.so, and runs it. It cannot launch a whole app yet"
                    + " — there is no guest libc or DEX loader in this build — so most games will stop at a"
                    + " named reason rather than starting. That reason is what the report shows.", 12f,
                    false));
        } else {
            for (final File f : list) {
                root.addView(gameRow(f));
            }
        }
        root.addView(space(6));
        root.addView(button("Home", new Runnable() {
            @Override public void run() { show(Screen.HOME); }
        }));
        return scroll(root);
    }

    private View gameRow(final File apk) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(dp(4), dp(10), dp(4), dp(10));

        String name = apk.getName().replaceAll("\\.apk$", "");
        row.addView(text(name, 16f, true));
        row.addView(text(String.format("%.1f MB · added %s", apk.length() / 1048576.0,
                android.text.format.DateFormat.format("yyyy-MM-dd HH:mm", apk.lastModified())), 11f, false));

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.addView(button("Run", new Runnable() {
            @Override public void run() {
                runGame(apk, getSharedPreferences(PREFS, MODE_PRIVATE).getInt(PREF_TIER, 0));
            }
        }), weight(1f));
        buttons.addView(button("Remove", new Runnable() {
            @Override public void run() { removeGame(apk); }
        }), weight(1f));
        row.addView(buttons);
        row.addView(button("Make installable", new Runnable() {
            @Override public void run() { repackGame(apk, true, null); }
        }));

        View divider = new View(this);
        divider.setBackgroundColor(0xFFDDDDDD);
        LinearLayout wrap = column();
        wrap.addView(row);
        wrap.addView(divider, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1)));
        return wrap;
    }

    private View settingsScreen() {
        final SharedPreferences p = getSharedPreferences(PREFS, MODE_PRIVATE);
        LinearLayout root = column();
        root.addView(title("Settings", 22f, true));

        root.addView(text("Execution tier — which of RetroBug's two engines a run uses. Tier I interprets"
                + " the engine's internal operations; Tier J compiles them to host code. They are held to"
                + " the same answers, so this is a speed and trust setting, not a compatibility one.", 12f,
                false));
        final int tier = p.getInt(PREF_TIER, 0);
        String[] names = {"Both tiers (compare them)", "Interpreter only (Tier I)", "Compiler only (Tier J)"};
        for (int i = 0; i < names.length; ++i) {
            final int value = i;
            final boolean on = tier == i;
            root.addView(button((on ? "●  " : "○  ") + names[i], new Runnable() {
                @Override public void run() {
                    p.edit().putInt(PREF_TIER, value).apply();
                    show(Screen.SETTINGS);
                }
            }));
        }
        root.addView(space(12));

        final boolean full = p.getBoolean(PREF_FULL_REPORT, false);
        root.addView(button((full ? "✓  " : "    ") + "Show the full report", new Runnable() {
            @Override public void run() {
                p.edit().putBoolean(PREF_FULL_REPORT, !full).apply();
                show(Screen.SETTINGS);
            }
        }));
        final boolean autorun = p.getBoolean(PREF_AUTORUN, true);
        root.addView(button((autorun ? "✓  " : "    ") + "Run the self-test when the screen opens", new Runnable() {
            @Override public void run() {
                p.edit().putBoolean(PREF_AUTORUN, !autorun).apply();
                show(Screen.SETTINGS);
            }
        }));
        root.addView(space(12));

        root.addView(text(buildFacts(), 12f, true));
        root.addView(space(8));
        root.addView(text("Where settings are kept: this app's private preferences file, readable by"
                + " nothing else on the phone. Nothing here is per-game yet, and nothing is sent anywhere.",
                11f, false));
        root.addView(space(6));
        root.addView(button("Home", new Runnable() {
            @Override public void run() { show(Screen.HOME); }
        }));
        return scroll(root);
    }

    private View diagnosticsScreen() {
        LinearLayout root = column();
        root.addView(title("Engine self-test", 22f, true));
        root.addView(text("Six seeds through the probe library on both tiers, each value compared against"
                + " what the same bytes produced natively on real hardware. A seed with no captured answer"
                + " is reported as such instead of being compared against zero.", 12f, false));
        root.addView(space(8));
        root.addView(button("Run", new Runnable() {
            @Override public void run() { saveAndShow("self-test", runSelfTest()); }
        }));
        root.addView(space(6));
        root.addView(button("Home", new Runnable() {
            @Override public void run() { show(Screen.HOME); }
        }));
        if (getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(PREF_AUTORUN, true)) {
            // Runs once when the screen is opened, on the same worker thread the report screen reads from.
            runSelfTestAsync();
        }
        return scroll(root);
    }

    private View reportScreen() {
        LinearLayout root = column();
        root.addView(title(reportTitle_, 18f, true));
        final TextView body = text(currentReport(), 11f, true);
        reportBody_ = body;
        body.setTextIsSelectable(true);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(body);
        root.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        root.addView(button("Copy report", new Runnable() {
            @Override public void run() {
                ClipboardManager cb = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
                if (cb != null) cb.setPrimaryClip(ClipData.newPlainText("RetroBug", currentReport()));
                Toast.makeText(LauncherActivity.this, "Report copied", Toast.LENGTH_SHORT).show();
            }
        }));
        root.addView(button("Home", new Runnable() {
            @Override public void run() { show(Screen.HOME); }
        }));
        return root;
    }

    // ------------------------------------------------------------------ work

    private String currentReport_ = "";
    /// The report screen's text, so a running game can show where it has got to; and whether one is running.
    private TextView reportBody_;
    private volatile boolean running_ = false;

    private String currentReport() {
        return currentReport_;
    }

    /// Keeps the report for the report screen and trims it when the setting says so. The trim keeps the
    /// head and the tail, because the two things a reader needs are what was attempted and what stopped it.
    ///
    /// Every line is logged before it is trimmed. The screen is for a person holding the phone; `logcat -s
    /// RBLAUNCH` is the interface everything else reads — `tools/run_engine_tests.sh` and every unattended
    /// check in this project — and the first version of this screen dropped the logging while adding the
    /// trimming, which turned "the self-test passed" into "the log is empty" for anyone not looking at the
    /// screen. The report goes out untrimmed for the same reason: a reader who can grep is not the reader
    /// the trim was for.
    private void saveAndShow(String title, String report) {
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
        for (String one : report.split("\n")) {
            if (!one.isEmpty()) Log.i(TAG, one);
        }
        reportTitle_ = title;
        /*
         * The verdict first. A run that prints a `RESULT:` line ends with it, below the fold the trim
         * leaves and after a screenful of setup lines — so a successful run read as a failure on screen.
         * The line is repeated at the top; the full report below it is unchanged.
         */
        for (String one : report.split("\n")) {
            if (one.startsWith("RESULT:")) {
                report = one + "\n\n" + report;
                break;
            }
        }
        SharedPreferences p = getSharedPreferences(PREFS, MODE_PRIVATE);
        if (p.getBoolean(PREF_FULL_REPORT, false)) {
            currentReport_ = report;
        } else {
            String[] lines = report.split("\n");
            if (lines.length <= 40) {
                currentReport_ = report;
            } else {
                StringBuilder sb = new StringBuilder();
                for (int i = 0; i < 24; ++i) sb.append(lines[i]).append('\n');
                sb.append("... ").append(lines.length - 34).append(" lines hidden — 'Show the full report' in"
                        + " Settings keeps all of them ...\n");
                for (int i = lines.length - 10; i < lines.length; ++i) sb.append(lines[i]).append('\n');
                currentReport_ = sb.toString();
            }
        }
        show(Screen.REPORT);
    }

    private void runSelfTestAsync() {
        // One worker thread, as before: six launches, two tiers each, through an interpreter is not a
        // millisecond job, and an ANR dialog would kill the process before the report existed.
        new Thread(new Runnable() {
            @Override public void run() {
                final String report = runSelfTest();
                runOnUiThread(new Runnable() {
                    @Override public void run() { saveAndShow("self-test", report); }
                });
            }
        }, "rb-selftest").start();
    }

    private String runSelfTest() {
        final StringBuilder out = new StringBuilder();
        out.append("device=").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(" sdk=").append(Build.VERSION.SDK_INT)
                .append(" abis=").append(Arrays.toString(Build.SUPPORTED_ABIS)).append('\n');
        byte[] guestSo;
        byte[] libm;
        try {
            guestSo = extractAssetEntry(GUEST_APK_ASSET, GUEST_SO_ENTRY);
            libm = readAll(getAssets().open(LIBM_ASSET));
        } catch (IOException | RuntimeException e) {
            return out.append("FATAL: could not read the bundled probe images: ")
                    .append(e.getClass().getSimpleName()).append(": ").append(e.getMessage()).toString();
        }
        final byte[] libc = readAssetOrEmpty(LIBC_ASSET);
        final byte[] libz = readAssetOrEmpty(LIBZ_ASSET);
        out.append("assets: ").append(GUEST_SO_ENTRY).append('=').append(guestSo.length)
                .append("B ").append(LIBM_ASSET).append('=').append(libm.length).append("B\n");
        for (int seed : SEEDS) {
            String r;
            try {
                r = nativeLaunch(guestSo, libm, libc, libz, METHOD, seed);
            } catch (Throwable t) {
                // A native SIGSEGV is not catchable and takes the process; what reaches here is a Java-side
                // throw such as an allocation failure, and one bad seed should still print the five that did.
                r = "FATAL: nativeLaunch threw " + t.getClass().getName() + ": " + t.getMessage();
            }
            out.append(r == null ? "FATAL: nativeLaunch returned null" : r).append('\n');
        }
        out.append("=== self-test done ===");
        return out.toString();
    }

    /// The library screen's Run. Streams the APK, executes the one entry RetroBug can execute, and reports
    /// what it found — including the common case where there is nothing to run.
    private void runGame(final File apk, final int tiers) {
        final String label = apk.getName();
        reportTitle_ = apk.getName().replaceAll("\\.apk$", "");
        currentReport_ = "reading " + apk.getName() + " ...";
        /*
         * The two ints the game's JNI entry point is given, taken where a real Java side would have taken
         * them: the Activity's own display, in pixels, which is exactly what GLSurfaceView.Renderer's
         * onSurfaceChanged hands a game. This app has no GLSurfaceView and no renderer — the game runs
         * headless inside the engine — so the honest number is the display RetroBug itself occupies, and
         * it is read here, on the UI thread, rather than guessed at in native code. The native report
         * says these were supplied on the game's behalf.
         */
        final android.util.DisplayMetrics metrics = getResources().getDisplayMetrics();
        final int surfaceWidth = metrics.widthPixels;
        final int surfaceHeight = metrics.heightPixels;
        running_ = true;
        /*
         * Live progress while the run works on its own thread. The report used to appear only at the end,
         * so a long run — Gumball's frame loop is tens of seconds — showed "reading <apk> ..." and nothing
         * else, indistinguishable from a hang. The native side publishes its latest line (and the frame loop
         * a "frame N, scene X" status); this polls it a few times a second until the report replaces it.
         */
        final long started = android.os.SystemClock.elapsedRealtime();
        final android.os.Handler ui = new android.os.Handler(android.os.Looper.getMainLooper());
        ui.post(new Runnable() {
            @Override public void run() {
                if (!running_) return;
                final String p = nativeProgress();
                final int nl = p.indexOf('\n');
                final String lines = nl < 0 ? "0" : p.substring(0, nl);
                final String latest = nl < 0 ? p : p.substring(nl + 1);
                final long secs = (android.os.SystemClock.elapsedRealtime() - started) / 1000;
                if (reportBody_ != null && (screen_ == Screen.REPORT || screen_ == Screen.RUN)) {
                    reportBody_.setText("running " + label + " — " + secs + " s, " + lines
                            + " report line(s) so far\n\n" + latest);
                }
                ui.postDelayed(this, 300);
            }
        });
        /*
         * The run starts once the run screen's SurfaceView has a surface: the native side makes its GLES1
         * context on that window, the game's GL calls draw into it, and each frame is presented there. The
         * report replaces the screen when the run returns.
         */
        pendingRun_ = new java.util.function.Consumer<Surface>() {
            @Override public void accept(final Surface surface) {
        new Thread(new Runnable() {
            @Override public void run() {
                String report;
                try {
                    report = inspectApk(apk, tiers, surfaceWidth, surfaceHeight, surface);
                } catch (IOException | RuntimeException e) {
                    report = "FATAL: could not read " + apk.getName() + ": "
                            + e.getClass().getSimpleName() + ": " + e.getMessage();
                }
                final String text = report;
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        running_ = false;
                        saveAndShow(label.replaceAll("\\.apk$", ""), text);
                    }
                });
            }
        }, "rb-run").start();
            }
        };
        /*
         * The game's own orientation, from its own manifest: the first activity, which is the launch activity
         * in these APKs (Gumball's GumballLiteActivity is screenOrientation=portrait, and its renderer
         * rotates its content for a phone held sideways). A landscape surface instead made it draw rotated
         * and zoomed. The run then starts on the first surface of that shape.
         */
        wantPortrait_ = false;
        try {
            android.content.pm.PackageInfo info = getPackageManager().getPackageArchiveInfo(
                    apk.getAbsolutePath(), android.content.pm.PackageManager.GET_ACTIVITIES);
            if (info != null && info.activities != null && info.activities.length > 0) {
                final int o = info.activities[0].screenOrientation;
                wantPortrait_ = o == android.content.pm.ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                        || o == android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT;
                setRequestedOrientation(o);
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "could not read the game's orientation: " + e);
        }
        show(Screen.RUN);
    }

    private boolean wantPortrait_ = false;

    /// The `-e bootcheck` run: `nativeBootCheck` on a worker thread, its answer shown as a report. With a
    /// `second` APK (`-e bootcheck2`), a second boot follows on the same thread, in the same process.
    private void bootCheck(final File apk, final String lib, final File second) {
        reportTitle_ = "bootcheck " + apk.getName();
        currentReport_ = "bootcheck " + apk.getName() + " ...";
        show(Screen.REPORT);
        new Thread(new Runnable() {
            @Override public void run() {
                String report = bootCheckOne(apk, lib);
                if (second != null) report += "\n" + bootCheckOne(second, null);
                final String text = report;
                runOnUiThread(new Runnable() {
                    @Override public void run() { saveAndShow("bootcheck", text); }
                });
            }
        }, "rb-bootcheck").start();
    }

    /// One `nativeBootCheck` on `apk` (its largest `assets/rb32/*.so` unless `lib` names one); never throws.
    /// A staged original has no `assets/rb32/` layout; its repack output (`-e repack <name> -e install 0`,
    /// under getExternalFilesDir(null)/repack/, same file name) does, and is booted in its place.
    private String bootCheckOne(File apk, String lib) {
        String note = "";
        try {
            if (largestRb32Lib(apk) == null) {
                File repacked = new File(new File(getExternalFilesDir(null), "repack"), apk.getName());
                if (repacked.isFile() && largestRb32Lib(repacked) != null) {
                    note = "bootcheck: " + apk.getName() + " has no assets/rb32/ — booted the M1 repack output "
                            + repacked + " instead of the staged original\n";
                    Log.i(TAG, note.trim());
                    apk = repacked;
                }
            }
            String name = lib;
            if (name == null || name.isEmpty()) name = largestRb32Lib(apk);
            if (name == null) {
                return "FATAL: bootcheck: " + apk.getName() + " has no assets/rb32/*.so entry —"
                        + " make it with tools/make_bootcheck_apk.sh";
            }
            Log.i(TAG, "bootcheck: " + apk.getName() + " lib=" + name);
            return note + nativeBootCheck(apk.getAbsolutePath(), name);
        } catch (IOException | RuntimeException | UnsatisfiedLinkError e) {
            String report = "FATAL: bootcheck: " + e.getClass().getSimpleName() + ": " + e.getMessage();
            Log.e(TAG, report);
            return note + report;
        }
    }

    private static String largestRb32Lib(File apk) throws IOException {
        String best = null;
        long bestSize = -1;
        try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(apk)) {
            java.util.Enumeration<? extends ZipEntry> it = zip.entries();
            while (it.hasMoreElements()) {
                ZipEntry e = it.nextElement();
                String n = e.getName();
                if (n.startsWith("assets/rb32/") && n.endsWith(".so") && n.indexOf('/', 12) < 0
                        && e.getSize() > bestSize) {
                    best = n.substring(12);
                    bestSize = e.getSize();
                }
            }
        }
        return best;
    }

    // ------------------------------------------------------------------ make installable (repack)

    /// "Make installable" (the scripted `-e repack` and the developer screens): rewrite the game for a 64-bit-only
    /// phone, sign it with the phone's repack key, keep it (and its report) under getExternalFilesDir(null)/repack/,
    /// then offer it to the installer. The work is ConvertJob's; one conversion at a time, shared with the library.
    private void repackGame(final File apk, final boolean install, final String renameTo) {
        if (!ConvertJob.claim()) {
            Toast.makeText(this, "A repack is already running", Toast.LENGTH_SHORT).show();
            return;
        }
        // A renamed repack is kept apart from the same-package one, which bootcheck boots in a staged original's place.
        final String label = apk.getName().replaceAll("\\.apk$", "")
                + (renameTo == null ? "" : renameTo.startsWith(".") ? renameTo : "." + renameTo);
        reportTitle_ = "make installable: " + label;
        currentReport_ = "repacking " + apk.getName() + " ...";
        show(Screen.REPORT);
        new Thread(new Runnable() {
            @Override public void run() {
                final StringBuilder out = new StringBuilder();
                final File signed = ConvertJob.convert(LauncherActivity.this, apk, renameTo, label, out);
                final File txt = new File(new File(getExternalFilesDir(null), "repack"), label + ".txt");
                writeText(txt, out.toString());
                final File toInstall = signed;
                final File report = txt;
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        ConvertJob.release();
                        saveAndShow("make installable: " + label, out.toString().trim());
                        if (toInstall != null && install) {
                            installRepacked(toInstall, report);
                        } else if (toInstall != null) {
                            Log.i(TAG, "install: skipped (-e install 0)");
                            appendText(report, "install: skipped (-e install 0)\n");
                        }
                    }
                });
            }
        }, "rb-repack").start();
    }

    /// Hands the signed APK to the installer; each status line goes to logcat, the report screen and the
    /// `<label>.txt` kept next to the APK.
    private void installRepacked(File apk, final File report) {
        RepackInstaller.install(this, apk, new RepackInstaller.Callback() {
            @Override public void onStatus(final String line, boolean done) {
                Log.i(TAG, line);
                appendText(report, line + "\n");
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        currentReport_ = currentReport_ + "\n" + line;
                        if (reportBody_ != null && screen_ == Screen.REPORT) reportBody_.setText(currentReport_);
                    }
                });
            }
        });
    }

    /// The stub, the engine and the guest libraries, read out of this launcher's own installed APK.
    /// The `-e stubcheck` run, on a worker thread: ApkRepackager.transform into the cache, each generated stub
    /// written to the code cache (an exec-allowed app directory) and checked by nativeStubCheck; one line per stub.
    private void stubCheck(final File apk) {
        reportTitle_ = "stubcheck " + apk.getName();
        currentReport_ = "stubcheck " + apk.getName() + " ...";
        show(Screen.REPORT);
        new Thread(new Runnable() {
            @Override public void run() {
                final StringBuilder out = new StringBuilder();
                File unsigned = new File(getCacheDir(), "stubcheck.unsigned.apk");
                try {
                    ApkRepackager.Result r = ApkRepackager.transform(apk, readRepackRuntime(out), unsigned);
                    for (String line : r.report) if (line.contains("stub")) out.append(line).append('\n');
                    File dir = new File(getCodeCacheDir(), "stubcheck");
                    if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("could not create " + dir);
                    try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(unsigned)) {
                        for (java.util.Map.Entry<String, List<String>> e : r.stubExports.entrySet()) {
                            File so = new File(dir, e.getKey());
                            try (InputStream in = z.getInputStream(z.getEntry("lib/arm64-v8a/" + e.getKey()));
                                 OutputStream os = new FileOutputStream(so)) {
                                byte[] buf = new byte[65536];
                                for (int k; (k = in.read(buf)) > 0; ) os.write(buf, 0, k);
                            }
                            out.append(nativeStubCheck(so.getAbsolutePath(), e.getValue().toArray(new String[0])))
                               .append('\n');
                        }
                    }
                    if (r.stubExports.isEmpty()) out.append("stubcheck: no generated stub (every library kept the prebuilt one)\n");
                } catch (Throwable t) {
                    out.append("stubcheck: FAILED — ").append(t).append('\n');
                    Log.e(TAG, "stubcheck failed", t);
                } finally {
                    if (unsigned.exists() && !unsigned.delete()) Log.w(TAG, "stubcheck: could not delete " + unsigned);
                }
                final String text = out.toString();
                Log.i(TAG, text);
                runOnUiThread(new Runnable() {
                    @Override public void run() { saveAndShow("stubcheck", text); }
                });
            }
        }, "rb-stubcheck").start();
    }

    private java.util.Map<String, byte[]> readRepackRuntime(StringBuilder out) throws IOException {
        return ConvertJob.readRuntime(this, out);
    }

    private static void writeText(File f, String s) {
        if (f == null) return;
        try (OutputStream o = new FileOutputStream(f)) {
            o.write(s.getBytes("UTF-8"));
        } catch (IOException e) {
            Log.w(TAG, "could not write " + f + ": " + e);
        }
    }

    /// Synchronized: install status lines arrive from the install worker and from the main thread.
    private static synchronized void appendText(File f, String s) {
        if (f == null) return;
        try (OutputStream o = new FileOutputStream(f, true)) {
            o.write(s.getBytes("UTF-8"));
        } catch (IOException e) {
            Log.w(TAG, "could not append to " + f + ": " + e);
        }
    }

    /// The run waiting for the run screen's surface; taken (and cleared) by `surfaceCreated`.
    private java.util.function.Consumer<Surface> pendingRun_;

    /// The game's window: a SurfaceView filling the screen, with the live status line over it. The run
    /// starts when the surface exists, and draws into it from its own thread through EGL.
    private View runScreen() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {}
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
                if (wantPortrait_ && w > h) return;  // still rotating; wait for the game's shape
                final java.util.function.Consumer<Surface> run = pendingRun_;
                pendingRun_ = null;
                if (run != null) run.accept(holder.getSurface());
            }
            @Override public void surfaceDestroyed(SurfaceHolder holder) {}
        });
        // The game's input: every MotionEvent on the surface, handed over raw. Consumed while a frame loop is
        // taking touches (the game's onTouchEvent returns true for all of them), passed on otherwise.
        view.setOnTouchListener(new View.OnTouchListener() {
            @Override public boolean onTouch(View v, android.view.MotionEvent e) {
                return nativeTouch(e.getAction(), e.getX(), e.getY(), v.getWidth(), System.currentTimeMillis());
            }
        });
        root.addView(view, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        TextView status = text(currentReport(), 11f, true);
        status.setTextColor(Color.WHITE);
        status.setBackgroundColor(0x88000000);
        status.setPadding(16, 8, 16, 8);
        reportBody_ = status;
        root.addView(status, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START));
        return root;
    }

    /**
     * Called from native code, on the run thread, with the EGL context current there: decodes one image
     * file and uploads it as a GL texture exactly the way Gumball's own `JNIGLSurface.textureLoaded` does —
     * BitmapFactory, scaled to powers of two when it is not, LINEAR filtering, CLAMP_TO_EDGE wrapping,
     * GLUtils.texImage2D. Returns {texture name, bitmap width, bitmap height}, or null if it cannot decode.
     */
    static int[] uploadTexture(byte[] encoded) {
        android.graphics.Bitmap bmp = android.graphics.BitmapFactory.decodeByteArray(encoded, 0, encoded.length);
        if (bmp == null) return null;
        final int w = bmp.getWidth();
        final int h = bmp.getHeight();
        int pw = 1;
        while (pw < w) pw <<= 1;
        int ph = 1;
        while (ph < h) ph <<= 1;
        if (pw != w || ph != h) {
            android.graphics.Bitmap scaled = android.graphics.Bitmap.createScaledBitmap(bmp, pw, ph, true);
            bmp.recycle();
            bmp = scaled;
        }
        int[] id = new int[1];
        android.opengl.GLES10.glGenTextures(1, id, 0);
        android.opengl.GLES10.glBindTexture(android.opengl.GLES10.GL_TEXTURE_2D, id[0]);
        android.opengl.GLES10.glTexParameterf(android.opengl.GLES10.GL_TEXTURE_2D,
                android.opengl.GLES10.GL_TEXTURE_MIN_FILTER, android.opengl.GLES10.GL_LINEAR);
        android.opengl.GLES10.glTexParameterf(android.opengl.GLES10.GL_TEXTURE_2D,
                android.opengl.GLES10.GL_TEXTURE_MAG_FILTER, android.opengl.GLES10.GL_LINEAR);
        android.opengl.GLES10.glTexParameterf(android.opengl.GLES10.GL_TEXTURE_2D,
                android.opengl.GLES10.GL_TEXTURE_WRAP_S, android.opengl.GLES10.GL_CLAMP_TO_EDGE);
        android.opengl.GLES10.glTexParameterf(android.opengl.GLES10.GL_TEXTURE_2D,
                android.opengl.GLES10.GL_TEXTURE_WRAP_T, android.opengl.GLES10.GL_CLAMP_TO_EDGE);
        android.opengl.GLUtils.texImage2D(android.opengl.GLES10.GL_TEXTURE_2D, 0, bmp, 0);
        bmp.recycle();
        return new int[] {id[0], w, h};
    }

    private String inspectApk(File apk, int tiers, int surfaceWidth, int surfaceHeight, Surface surface)
            throws IOException {
        byte[] libmBytes;
        try {
            libmBytes = readAll(getAssets().open(LIBM_ASSET));
        } catch (IOException e) {
            return "FATAL: the bundled guest libm asset could not be read: " + e.getMessage();
        }
        final byte[] libcBytes = readAssetOrEmpty(LIBC_ASSET);
        final byte[] libzBytes = readAssetOrEmpty(LIBZ_ASSET);
        final byte[] libstdcxxBytes = readAssetOrEmpty(LIBSTDCXX_ASSET);

        /*
         * One listing pass and one read, both through `ZipFile` rather than `ZipInputStream`.
         *
         * The library to run is chosen by the central directory: prefer an `armeabi-v7a` build over
         * `armeabi` (the Angry Birds APK on the test phone carries both, and the first version of this ran
         * the ARMv5 build while the ARMv7A one sat right there — a device would have picked v7a, and what
         * RetroBug tests should be what the device would have run), and within that ABI the largest `.so`,
         * because a game's code is never the smallest file in its own APK.
         *
         * `ZipInputStream` cannot do this: it is a stream, so `ZipEntry.getSize()` is -1 for anything whose
         * size is not in the local header, and the comparison silently selected nothing — which the phone
         * then reported as "this APK carries no library" about an APK with two of them. `ZipFile` reads the
         * central directory, where the sizes are.
         */
        StringBuilder facts = new StringBuilder();
        facts.append("apk: ").append(apk.getName()).append(" (").append(apk.length()).append(" bytes)\n");
        List<String> v7a = new ArrayList<>();
        List<String> armv5 = new ArrayList<>();
        List<String> otherAbis = new ArrayList<>();
        boolean hasDex = false;
        String chosen = null;
        long chosenSize = 0;
        try (java.util.zip.ZipFile zf = new java.util.zip.ZipFile(apk)) {
            for (java.util.Enumeration<? extends ZipEntry> en = zf.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement();
                String n = e.getName();
                if (n.startsWith("lib/") && n.endsWith(".so")) {
                    int slash = n.indexOf('/', 4);
                    if (slash < 0) continue;
                    String abi = n.substring(4, slash);
                    if ("armeabi-v7a".equals(abi)) {
                        v7a.add(n);
                    } else if ("armeabi".equals(abi)) {
                        armv5.add(n);
                    } else {
                        otherAbis.add(abi);
                    }
                } else if (n.equals("classes.dex")) {
                    hasDex = true;
                }
            }
            // ARMv7A when present, ARMv5 only when it is not; largest within the chosen ABI.
            for (String candidate : (v7a.isEmpty() ? armv5 : v7a)) {
                ZipEntry e = zf.getEntry(candidate);
                long size = e == null ? 0 : e.getSize();
                if (chosen == null || size > chosenSize) { chosen = candidate; chosenSize = size; }
            }
            if (chosen != null) {
                facts.append("running: ").append(chosen).append(" (").append(chosenSize).append(" bytes")
                        .append(v7a.size() + armv5.size() > 1
                                ? ", the largest of " + (v7a.size() + armv5.size()) : "")
                        .append(v7a.isEmpty() ? ", no armeabi-v7a build in this APK" : "")
                        .append(")\n");
            }
            int arm32Count = v7a.size() + armv5.size();
            facts.append("32-bit ARM libraries: ")
                    .append(arm32Count == 0 ? "none" : String.valueOf(arm32Count));
            if (arm32Count > 0) {
                facts.append(" (").append(v7a.size()).append(" armeabi-v7a, ").append(armv5.size())
                        .append(" armeabi)");
            }
            facts.append('\n');
            if (!otherAbis.isEmpty()) {
                List<String> uniq = new ArrayList<>(new java.util.LinkedHashSet<>(otherAbis));
                Collections.sort(uniq);
                facts.append("other ABIs present: ").append(uniq).append('\n');
            }
            facts.append("classes.dex: ").append(hasDex ? "present" : "absent").append('\n');
            if (chosen == null) {
                facts.append("\nNothing for RetroBug to run: this APK carries no lib/armeabi-v7a or")
                        .append(" lib/armeabi library.\n");
                if (hasDex) {
                    facts.append("It has Java code, and running Java/DEX is not something this build does —")
                            .append(" it executes 32-bit ARM native code.\n");
                }
                return facts.toString();
            }
            byte[] so;
            StringBuilder refused = new StringBuilder();
            try (InputStream in = zf.getInputStream(zf.getEntry(chosen))) {
                so = readAllCapped(in, MAX_SO_BYTES, chosen, refused);
            }
            if (so == null) return facts.append(refused).toString();
            facts.append('\n');
            /*
             * `apk.getAbsolutePath()` is the guest's whole view of the filesystem. It travels as the entry
             * point's path argument, and it is the only path the native side's file provider answers, so
             * `nativeInit` can open the APK it arrived in and read the `assets/` inside it. Nothing else
             * becomes reachable: every other path is still a named refusal — see the provider in
             * `android/jni/launcher.cpp`.
             */
            return facts + nativeInspect(so, libmBytes, libcBytes, libzBytes, libstdcxxBytes, chosen, tiers,
                    apk.getAbsolutePath(), surfaceWidth, surfaceHeight, surface);
        }
    }

    // ------------------------------------------------------------------ games on disk

    private File gamesDir() {
        File dir = new File(getFilesDir(), "games");
        if (!dir.exists() && !dir.mkdirs()) Log.w(TAG, "could not create " + dir);
        return dir;
    }

    private List<File> games() {
        File[] files = gamesDir().listFiles();
        if (files == null) return new ArrayList<>();
        List<File> out = new ArrayList<>(Arrays.asList(files));
        Collections.sort(out, new java.util.Comparator<File>() {
            @Override public int compare(File a, File b) { return a.getName().compareToIgnoreCase(b.getName()); }
        });
        return out;
    }

    private static final int PICK_APK = 1;

    private void pickApk() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/vnd.android.package-archive");
        // Some file providers hand back a generic type for an APK; without this the picker greys everything out.
        intent.putExtra(Intent.EXTRA_MIME_TYPES,
                new String[]{"application/vnd.android.package-archive", "application/octet-stream",
                        "application/zip"});
        try {
            startActivityForResult(intent, PICK_APK);
        } catch (android.content.ActivityNotFoundException e) {
            Toast.makeText(this, "No file picker on this phone", Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_APK || result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        String name = queryName(uri);
        if (name == null) name = "game.apk";
        if (!name.toLowerCase().endsWith(".apk")) name = name + ".apk";
        final File dest = new File(gamesDir(), name.replaceAll("[^A-Za-z0-9._-]", "_"));
        Toast.makeText(this, "Adding " + dest.getName() + " ...", Toast.LENGTH_SHORT).show();
        // Copied off the main thread: a game's APK is tens or hundreds of megabytes and copying one on the
        // UI thread is an ANR with a toast attached to it.
        new Thread(new Runnable() {
            @Override public void run() {
                String problem = null;
                try (InputStream in = getContentResolver().openInputStream(uri);
                     OutputStream out = new FileOutputStream(dest)) {
                    if (in == null) throw new IOException("the picker returned a stream this app cannot open");
                    byte[] buf = new byte[1 << 16];
                    for (int n = in.read(buf); n > 0; n = in.read(buf)) out.write(buf, 0, n);
                } catch (IOException | SecurityException e) {
                    problem = e.getClass().getSimpleName() + ": " + e.getMessage();
                }
                final String message = problem == null
                        ? "Added " + dest.getName() + " (" + (dest.length() >> 20) + " MB)"
                        : "Could not copy the file — " + problem;
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        Toast.makeText(LauncherActivity.this, message, Toast.LENGTH_LONG).show();
                        show(Screen.HOME);  // the library rescans and shows the new game
                    }
                });
            }
        }, "rb-add").start();
    }

    private String queryName(Uri uri) {
        android.database.Cursor c = null;
        try {
            c = getContentResolver().query(uri, null, null, null, null);
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) return c.getString(idx);
            }
        } catch (RuntimeException ignored) {
            // Fall through to the path's last segment.
        } finally {
            if (c != null) c.close();
        }
        String path = uri.getPath();
        return path == null ? null : path.substring(path.lastIndexOf('/') + 1);
    }

    private void removeGame(File apk) {
        if (apk.delete()) {
            Toast.makeText(this, "Removed " + apk.getName(), Toast.LENGTH_SHORT).show();
        } else {
            Toast.makeText(this, "Could not remove " + apk.getName(), Toast.LENGTH_LONG).show();
        }
        show(Screen.LIBRARY);
    }

    // ------------------------------------------------------------------ io helpers

    /// Reads one entry out of a zip *inside the APK's assets* — the probe APK is one level down.
    private byte[] extractAssetEntry(String asset, String entry) throws IOException {
        try (InputStream in = getAssets().open(asset)) {
            try (ZipInputStream zip = new ZipInputStream(in)) {
                for (ZipEntry e = zip.getNextEntry(); e != null; e = zip.getNextEntry()) {
                    if (entry.equals(e.getName())) return readAll(zip);
                }
            }
        }
        throw new IOException(entry + " is not in " + asset);
    }

    /// Reads the current zip entry, refusing rather than allocating when it is larger than `cap`. The cap
    /// exists because the alternative to a message is an `OutOfMemoryError` on a phone with a game's
    /// native library in hand.
    private byte[] readAllCapped(InputStream in, long cap, String what, StringBuilder facts) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 16];
        long total = 0;
        for (int n = in.read(buf); n > 0; n = in.read(buf)) {
            total += n;
            if (total > cap) {
                facts.append("refusing ").append(what).append(": larger than ").append(cap >> 20)
                        .append(" MB\n");
                return null;
            }
            out.write(buf, 0, n);
        }
        return out.toByteArray();
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

    /// An asset's bytes, or an empty array when this APK does not carry that asset. A build can legitimately
    /// ship without a guest libc (there is no script here that produces one), and the run that results is
    /// the native half's to report by name — an IOException out of a missing asset would instead replace
    /// that report with nothing at all.
    private byte[] readAssetOrEmpty(String asset) {
        try {
            return readAll(getAssets().open(asset));
        } catch (IOException e) {
            return new byte[0];
        }
    }

    // ------------------------------------------------------------------ view helpers

    private String buildFacts() {
        String[] abis32 = Build.SUPPORTED_32_BIT_ABIS;
        boolean native32 = abis32 != null && abis32.length > 0;
        return Build.MANUFACTURER + " " + Build.MODEL + " · Android " + Build.VERSION.RELEASE
                + " (sdk " + Build.VERSION.SDK_INT + ") · page " + pageSize() + " B\n"
                + (native32
                ? "This phone can also run 32-bit ARM natively, so results here show RetroBug agrees with"
                        + " the phone. Only a 64-bit-only phone shows emulation."
                : "This phone cannot run 32-bit ARM at all, so every ARMv7 instruction behind these"
                        + " results was executed by RetroBug inside this app.");
    }

    private int pageSize() {
        return (int) android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE);
    }

    private LinearLayout column() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.WHITE);
        root.setPadding(dp(12), dp(12), dp(12), dp(12));
        return root;
    }

    private View scroll(View content) {
        ScrollView s = new ScrollView(this);
        s.setBackgroundColor(Color.WHITE);
        s.addView(content);
        return s;
    }

    private TextView title(String text, float size, boolean bold) {
        TextView v = new TextView(this);
        v.setText(text);
        v.setTextSize(size);
        v.setTextColor(Color.BLACK);
        if (bold) v.setTypeface(Typeface.DEFAULT_BOLD);
        v.setPadding(0, 0, 0, dp(6));
        return v;
    }

    private TextView text(String body, float size, boolean mono) {
        TextView v = new TextView(this);
        v.setText(body);
        v.setTextSize(size);
        v.setTextColor(0xFF333333);
        if (mono) v.setTypeface(Typeface.MONOSPACE);
        v.setPadding(0, dp(2), 0, dp(6));
        return v;
    }

    private Button button(String label, final Runnable action) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setGravity(Gravity.CENTER_VERTICAL);
        b.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { action.run(); }
        });
        b.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        return b;
    }

    private LinearLayout.LayoutParams weight(float w) {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, w);
    }

    private View space(int dp) {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(dp)));
        return v;
    }

    private int dp(int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
                getResources().getDisplayMetrics());
    }
}
