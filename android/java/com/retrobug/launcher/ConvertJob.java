// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Making a game playable: rewrite its APK for this 64-bit phone (ApkRepackager), sign it with the phone's repack key
// (RepackSigner), and hand it to Android's installer (RepackInstaller). One at a time: a second start while one runs is
// refused. The library's tap and the scripted `-e repack` both come here; the scripted path keeps its own file names
// and report so the device scripts read what they always read.
package com.retrobug.launcher;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.FileOutputStream;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class ConvertJob {
    private static final String TAG = "RetroBug";
    private static final AtomicBoolean RUNNING = new AtomicBoolean(false);

    interface Callback {
        void onProgress(String line);
        /** On the main thread. `ok` is true when the copy was installed. */
        void onDone(boolean ok, String report);
    }

    /** The stub, the engine and the guest libraries in this launcher's own APK, and where they go in a repack. */
    static final String[][] RUNTIME = {
            {"lib/arm64-v8a/librbstub.so", ApkRepackager.STUB_KEY},
            {"lib/arm64-v8a/librbguest.so", "lib/arm64-v8a/librbguest.so"},
            {"assets/rbshim.dex", ShimPlan.SHIM_DEX_KEY},
            {"assets/guest/libc.so", "assets/rb32sys/libc.so"},
            {"assets/guest/libm.so", "assets/rb32sys/libm.so"},
            {"assets/guest/libz.so", "assets/rb32sys/libz.so"},
            {"assets/guest/libstdc++.so", "assets/rb32sys/libstdc++.so"},
    };

    private ConvertJob() {}

    static boolean running() { return RUNNING.get(); }

    /** Claims the single conversion slot; false when one is already running. Paired with {@link #release()}. */
    static boolean claim() { return RUNNING.compareAndSet(false, true); }
    static void release() { RUNNING.set(false); }

    static Map<String, byte[]> readRuntime(Context ctx, StringBuilder out) throws IOException {
        Map<String, byte[]> files = new LinkedHashMap<>();
        try (ZipFile self = new ZipFile(ctx.getApplicationInfo().sourceDir)) {
            for (String[] m : RUNTIME) {
                ZipEntry e = self.getEntry(m[0]);
                if (e == null) {
                    // The stub and the engine are the repack; a guest library this build did not ship is reported and
                    // left out (the runtime then names it when a game needs it).
                    if (m[0].startsWith("lib/")) throw new IOException("this launcher's APK has no " + m[0]);
                    out.append("runtime: ").append(m[0]).append(" not in this launcher build — not added\n");
                    continue;
                }
                try (InputStream in = self.getInputStream(e)) { files.put(m[1], readAll(in)); }
            }
        }
        String cert = launcherCertSha256(ctx);
        if (cert != null) files.put(ShimPlan.LAUNCHER_CERT_KEY, cert.getBytes("UTF-8"));
        else out.append("runtime: this launcher's certificate could not be read — the copy gets no save export/import\n");
        return files;
    }

    /** SHA-256 of this launcher's one signing certificate, lower-case hex: what a copy's RbSaveProvider trusts. */
    @SuppressWarnings("deprecation")
    static String launcherCertSha256(Context ctx) {
        try {
            android.content.pm.PackageManager pm = ctx.getPackageManager();
            android.content.pm.Signature[] sigs;
            if (android.os.Build.VERSION.SDK_INT >= 28) {
                android.content.pm.PackageInfo pi = pm.getPackageInfo(ctx.getPackageName(),
                        android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES);
                sigs = pi.signingInfo == null ? null : pi.signingInfo.getApkContentsSigners();
            } else {
                sigs = pm.getPackageInfo(ctx.getPackageName(), android.content.pm.PackageManager.GET_SIGNATURES).signatures;
            }
            if (sigs == null || sigs.length != 1) return null;
            StringBuilder hex = new StringBuilder();
            for (byte b : java.security.MessageDigest.getInstance("SHA-256").digest(sigs[0].toByteArray()))
                hex.append(String.format("%02x", b));
            return hex.toString();
        } catch (Exception e) {
            Log.w(TAG, "launcher certificate: " + e);
            return null;
        }
    }

    /**
     * Rewrites and signs `apk` into getExternalFilesDir/repack/<label>.apk, appending the report to `out`. Returns the
     * signed file, or null (the reason is in `out`). Runs on the calling (background) thread.
     */
    static File convert(Context ctx, File apk, String renameTo, String label, StringBuilder out) {
        File signed = null;
        try {
            File dir = new File(ctx.getExternalFilesDir(null), "repack");
            if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("could not create " + dir);
            signed = new File(dir, label + ".apk");
            File unsigned = new File(ctx.getCacheDir(), label + ".unsigned.apk");
            out.append("repack: ").append(apk.getName()).append(" (").append(apk.length()).append(" bytes)\n");
            ApkRepackager.Result r = ApkRepackager.transform(apk, readRuntime(ctx, out), unsigned, renameTo);
            out.append("package: ").append(r.packageName)
                    .append(r.renamedFrom == null ? "" : " (renamed from " + r.renamedFrom + ")").append('\n');
            out.append("minSdk: ").append(r.oldMinSdk < 0 ? "absent" : String.valueOf(r.oldMinSdk))
                    .append(" -> ").append(r.newMinSdk).append('\n');
            out.append("targetSdk: ").append(r.oldTargetSdk < 0 ? "absent" : String.valueOf(r.oldTargetSdk))
                    .append(" -> ").append(r.newTargetSdk).append('\n');
            out.append("guest libs (assets/rb32/): ").append(r.guestLibs).append('\n');
            out.append("stubs (lib/arm64-v8a/): ").append(r.stubs).append('\n');
            for (String line : r.report) out.append("  ").append(line).append('\n');
            try {
                RepackSigner.sign(unsigned, signed);
            } finally {
                if (!unsigned.delete()) Log.w(TAG, "repack: could not delete " + unsigned);
            }
            out.append("signed: ").append(signed.getAbsolutePath()).append(" (").append(signed.length())
                    .append(" bytes, key ").append(RepackSigner.ALIAS).append(", v1+v2+v3)\n");
            out.append("RESULT: repack OK — ").append(r.packageName).append('\n');
            return signed;
        } catch (Throwable t) {
            String m = t.getMessage();
            if (t instanceof IOException && m != null && m.startsWith("refused:")) {
                out.append("RESULT: repack ").append(m).append('\n');
            } else {
                out.append("RESULT: repack FAILED — ").append(t.getClass().getName()).append(": ").append(m).append('\n');
                Log.e(TAG, "repack failed", t);
            }
            if (signed != null && signed.exists() && !signed.delete()) Log.w(TAG, "repack: could not delete " + signed);
            return null;
        }
    }

    /**
     * The library's conversion: renamed `.rb`, for `settings.target`, then installed. False (nothing started) when a
     * conversion is already running.
     */
    static boolean start(final Context ctx, final Game game, final GameSettings settings, final Callback cb) {
        if (!claim()) return false;
        final Context app = ctx.getApplicationContext();
        final Handler main = new Handler(Looper.getMainLooper());
        new Thread(new Runnable() {
            @Override public void run() {
                final StringBuilder out = new StringBuilder();
                boolean handedToInstaller = false;
                try {
                    ApkRepackager.debuggable = false;
                    ApkRepackager.nativeControl = false;
                    ApkRepackager.targetFloor = settings.target;
                    final File signed = convert(app, game.apk, ApkRepackager.RENAME_SUFFIX, game.copyPackage(), out);
                    writeText(new File(new File(app.getExternalFilesDir(null), "repack"), game.copyPackage() + ".txt"),
                            out.toString());
                    if (signed != null) {
                        handedToInstaller = true;
                        final AtomicBoolean finished = new AtomicBoolean(false);
                        final RepackInstaller.Callback status = new RepackInstaller.Callback() {
                            @Override public void onStatus(final String line, boolean done) {
                                Log.i(TAG, line);
                                out.append(line).append('\n');
                                if (!done) {
                                    post(main, cb, line);
                                    return;
                                }
                                if (!finished.compareAndSet(false, true)) return;  // one verdict per conversion
                                RepackInstaller.pendingConfirm = null;
                                final boolean ok = line.contains("(SUCCESS)");
                                if (ok && !ObbStore.sources(app, game).isEmpty()) {
                                    // The game's OBB goes into the copy's own folder before the tile says Ready.
                                    new Thread(new Runnable() {
                                        @Override public void run() {
                                            String why = ObbStore.install(app, game, new ObbStore.Progress() {
                                                @Override public void at(long done, long total) {
                                                    post(main, cb, "obb: " + (total > 0 ? done * 100 / total : 0) + "%");
                                                }
                                            });
                                            out.append(why == null ? "obb: installed for the copy\n" : "obb: " + why + "\n");
                                            release();
                                            main.post(new Runnable() {
                                                @Override public void run() { cb.onDone(true, out.toString()); }
                                            });
                                        }
                                    }, "rb-obb").start();
                                    return;
                                }
                                release();
                                main.post(new Runnable() {
                                    @Override public void run() { cb.onDone(ok, out.toString()); }
                                });
                            }
                        };
                        main.post(new Runnable() {
                            @Override public void run() {
                                try {
                                    RepackInstaller.install(app, signed, status);
                                } catch (RuntimeException e) {
                                    // The slot is released whatever the installer does; a game never stays "Getting ready".
                                    status.onStatus("install: could not start: " + e, true);
                                }
                            }
                        });
                    }
                } finally {
                    ApkRepackager.targetFloor = 0;
                    if (!handedToInstaller) {
                        release();
                        main.post(new Runnable() {
                            @Override public void run() { cb.onDone(false, out.toString()); }
                        });
                    }
                }
            }
        }, "rb-convert").start();
        return true;
    }

    private static void post(Handler main, final Callback cb, final String line) {
        main.post(new Runnable() {
            @Override public void run() { cb.onProgress(line); }
        });
    }

    static void writeText(File f, String s) {
        try (OutputStream o = new FileOutputStream(f)) {
            o.write(s.getBytes("UTF-8"));
        } catch (IOException e) {
            Log.w(TAG, "could not write " + f + ": " + e);
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) o.write(buf, 0, n);
        return o.toByteArray();
    }
}
