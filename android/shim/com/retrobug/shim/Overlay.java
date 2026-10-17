// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The performance overlay the player switched on in RetroBug's game menu: a small readout in the corner of every
// screen of the game — frames per second and the guest CPU rate — refreshed once a second from the runtime
// (librbguest's Java_com_retrobug_shim_Overlay_nativeStats). It draws over the game's own views and never takes touch.
package com.retrobug.shim;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

final class Overlay {
    private Overlay() {}

    static native String nativeStats();

    private static final String TAG_VIEW = "rb-overlay";

    static void install(Context ctx) {
        Context app = ctx.getApplicationContext() != null ? ctx.getApplicationContext() : ctx;
        if (!(app instanceof Application)) return;
        try {
            System.loadLibrary("rbguest");  // already loaded by the stub; this binds nativeStats in this class loader
        } catch (Throwable t) {
            Log.i(Shims.TAG, "overlay: no runtime library (" + t + ")");
            return;
        }
        ((Application) app).registerActivityLifecycleCallbacks(new Shims.Lifecycle() {
            @Override public void onActivityResumed(final Activity a) { attach(a); }
        });
        Log.i(Shims.TAG, "overlay: on");
    }

    private static void attach(final Activity a) {
        ViewGroup decor = (ViewGroup) a.getWindow().getDecorView();
        if (decor.findViewWithTag(TAG_VIEW) != null) return;
        final TextView v = new TextView(a);
        v.setTag(TAG_VIEW);
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        v.setTextColor(0xFF33B5E5);
        v.setTypeface(android.graphics.Typeface.MONOSPACE);
        v.setBackgroundColor(0xAA000000);
        int p = (int) (6 * a.getResources().getDisplayMetrics().density);
        v.setPadding(p, p / 2, p, p / 2);
        v.setClickable(false);
        v.setFocusable(false);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START);
        lp.setMargins(p * 2, p * 6, 0, 0);
        decor.addView(v, lp);
        final Handler h = new Handler(Looper.getMainLooper());
        h.post(new Runnable() {
            @Override public void run() {
                if (v.getParent() == null || a.isFinishing()) return;
                try {
                    v.setText(nativeStats());
                } catch (Throwable t) {
                    v.setText("RetroBug");
                }
                h.postDelayed(this, 1000);
            }
        });
    }
}
