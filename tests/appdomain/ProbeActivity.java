/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The Activity that carries the app-domain probe.
 *
 * It exists because the thing being measured is *the process this runs in*, not the code. An
 * `adb shell` binary answers for the `shell` domain; only an installed, launched app answers for
 * `untrusted_app`, which is the domain RetroBug ships into (docs/10 U-13, docs/21 §E-series).
 *
 * Two Java-side measurements, both of which are Java questions rather than syscall questions:
 * whether the framework puts a native library somewhere the app can load from, and whether it will
 * load one from app-writable storage — the latter being the rule that landed with `targetSdk >= 29`
 * and the reason M1's guest `.so` files cannot simply be dropped into the files dir.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
package com.retrobug.probe;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public class ProbeActivity extends Activity {
    private static final String TAG = "rbprobe";

    static {
        System.loadLibrary("rbprobe");
    }

    private static native void runNative();

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        ApplicationInfo ai = getApplicationInfo();
        Log.i(TAG, "java: release=" + Build.VERSION.RELEASE + " sdk=" + Build.VERSION.SDK_INT
                + " targetSdk=" + ai.targetSdkVersion + " minSdk=" + ai.minSdkVersion
                + " flags=0x" + Integer.toHexString(ai.flags)
                + " nativeLibraryDir=" + ai.nativeLibraryDir
                + " dataDir=" + ai.dataDir);
        Log.i(TAG, "java: product=" + Build.MANUFACTURER + " " + Build.MODEL
                + " abiList=" + java.util.Arrays.toString(Build.SUPPORTED_ABIS));

        runNative();

        tryLoadFromAppWritableDir();

        Log.i(TAG, "=== app-domain probe complete ===");
        finish();
    }

    /**
     * Copies this app's own native library into app-writable storage and tries to load it.
     *
     * What it is deciding: whether guest libraries can be unpacked to the files dir and `dlopen`ed from
     * there, or have to be loaded from a file descriptor the linker is told about explicitly. A failure
     * here is not a surprise at targetSdk 29+, but the *error string* is the useful part — it is what
     * `docs/research/02` §6.4 asked to have recorded rather than inferred from a changelog.
     */
    private void tryLoadFromAppWritableDir() {
        File src = new File(getApplicationInfo().nativeLibraryDir, "librbprobe.so");
        File dst = new File(getCacheDir(), "librbprobe_copy.so");
        try (InputStream in = new java.io.FileInputStream(src);
             OutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.flush();
        } catch (Throwable t) {
            Log.i(TAG, "load-from-writable: COPY FAILED " + t.getClass().getSimpleName()
                    + ": " + t.getMessage());
            return;
        }
        Log.i(TAG, "load-from-writable: copied " + dst.length() + " bytes to " + dst.getAbsolutePath());
        try {
            System.load(dst.getAbsolutePath());
            Log.i(TAG, "load-from-writable: LOADED from app-writable storage");
        } catch (Throwable t) {
            Log.i(TAG, "load-from-writable: REFUSED " + t.getClass().getSimpleName()
                    + ": " + t.getMessage());
        }
        dst.delete();
    }
}
