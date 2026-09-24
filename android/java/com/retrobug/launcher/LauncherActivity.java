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
 * Screens are built in code rather than from layouts, and there is exactly one Activity and one file,
 * because `tools/build_launcher_apk.sh` compiles this single path with `javac` and packages it with
 * `aapt2 link` against a manifest with no resources. Anything that needed a resource file, an XML layout,
 * or a second source file would need that script rewritten, and its output is what has been verified on
 * hardware (`docs/30` §Step 37, §Step 38).
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

    private static native String nativeInspect(byte[] guestSo, byte[] libmSo, byte[] libcSo, byte[] libzSo,
                                               String label, int tiers, String apkPath, int surfaceWidth,
                                               int surfaceHeight);

    private enum Screen { HOME, LIBRARY, SETTINGS, DIAGNOSTICS, REPORT }

    private Screen screen_ = Screen.HOME;
    private String reportTitle_ = "";

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        show(Screen.HOME);
        /*
         * An unattended run, so that a test does not depend on tapping the right pixel.
         *
         * `am start -n com.retrobug.launcher/.LauncherActivity -e run angry` opens the first game whose
         * file name contains "angry" and runs it, with no UI in the loop. Driving this by taps was how the
         * previous rounds were tested, and twice the taps landed on the screen the app happened to be
         * resuming on rather than the one intended — each time costing a round trip and, once, producing a
         * "nothing happened" reading of a run that never started. A named intent cannot miss.
         */
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
            case HOME: setContentView(homeScreen()); break;
            case LIBRARY: setContentView(libraryScreen()); break;
            case SETTINGS: setContentView(settingsScreen()); break;
            case DIAGNOSTICS: setContentView(diagnosticsScreen()); break;
            case REPORT: setContentView(reportScreen()); break;
        }
    }

    // ------------------------------------------------------------------ screens

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
        for (String one : report.split("\n")) {
            if (!one.isEmpty()) Log.i(TAG, one);
        }
        reportTitle_ = title;
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
        show(Screen.REPORT);
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
        new Thread(new Runnable() {
            @Override public void run() {
                String report;
                try {
                    report = inspectApk(apk, tiers, surfaceWidth, surfaceHeight);
                } catch (IOException | RuntimeException e) {
                    report = "FATAL: could not read " + apk.getName() + ": "
                            + e.getClass().getSimpleName() + ": " + e.getMessage();
                }
                final String text = report;
                runOnUiThread(new Runnable() {
                    @Override public void run() { saveAndShow(label.replaceAll("\\.apk$", ""), text); }
                });
            }
        }, "rb-run").start();
    }

    private String inspectApk(File apk, int tiers, int surfaceWidth, int surfaceHeight) throws IOException {
        byte[] libmBytes;
        try {
            libmBytes = readAll(getAssets().open(LIBM_ASSET));
        } catch (IOException e) {
            return "FATAL: the bundled guest libm asset could not be read: " + e.getMessage();
        }
        final byte[] libcBytes = readAssetOrEmpty(LIBC_ASSET);
        final byte[] libzBytes = readAssetOrEmpty(LIBZ_ASSET);

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
            return facts + nativeInspect(so, libmBytes, libcBytes, libzBytes, chosen, tiers,
                    apk.getAbsolutePath(), surfaceWidth, surfaceHeight);
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
                        show(Screen.LIBRARY);
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
