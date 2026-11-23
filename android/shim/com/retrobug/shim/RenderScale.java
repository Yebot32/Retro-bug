// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// M6 render scale for converted games that draw into a SurfaceView (GLSurfaceView and friends — most of them): the
// surface's buffer is made smaller than the view (SurfaceHolder.setFixedSize) and the compositor scales it up, so the
// game renders fewer pixels. The game learns the smaller size from its own surfaceChanged and maps touches against it,
// so touches reaching that view are scaled by the same factor. Off (100 %) unless the player picked 75 or 50 %.
package com.retrobug.shim;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.os.Bundle;
import android.util.Log;
import android.view.MotionEvent;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;

final class RenderScale {
    private RenderScale() {}

    static void install(Context ctx, final int percent) {
        if (percent >= 100 || percent < 25) return;
        Context app = ctx.getApplicationContext();
        if (!(app instanceof Application)) return;
        final float s = percent / 100f;
        Log.i(Shims.TAG, "render scale: " + percent + "% for SurfaceView games");
        ((Application) app).registerActivityLifecycleCallbacks(new Shims.Lifecycle() {
            @Override public void onActivityResumed(final Activity a) {
                final View decor = a.getWindow() == null ? null : a.getWindow().getDecorView();
                if (decor == null) return;
                decor.post(new Runnable() {
                    @Override public void run() { scaleAll(decor, s); }
                });
            }
        });
    }

    private static void scaleAll(View v, float s) {
        if (v instanceof SurfaceView) {
            scale((SurfaceView) v, s);
            return;
        }
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) scaleAll(g.getChildAt(i), s);
        }
    }

    private static void scale(final SurfaceView sv, final float s) {
        if (sv.getTag(TAG_KEY) != null) return;  // once per view
        sv.setTag(TAG_KEY, Boolean.TRUE);
        final Runnable apply = new Runnable() {
            @Override public void run() {
                int w = sv.getWidth(), h = sv.getHeight();
                if (w > 0 && h > 0) sv.getHolder().setFixedSize(Math.max(1, Math.round(w * s)), Math.max(1, Math.round(h * s)));
            }
        };
        apply.run();
        sv.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            @Override public void onLayoutChange(View v, int l, int t, int r, int b, int ol, int ot, int or, int ob) {
                if (r - l != or - ol || b - t != ob - ot) apply.run();
            }
        });
        // Touches in view pixels, scaled to the surface's pixels the game draws and thinks in.
        sv.setOnTouchListener(new View.OnTouchListener() {
            @Override public boolean onTouch(View v, MotionEvent e) {
                android.graphics.Matrix m = new android.graphics.Matrix();
                m.setScale(s, s);
                MotionEvent all = MotionEvent.obtain(e);
                all.transform(m);  // every pointer, not only the first
                try {
                    return v.onTouchEvent(all);
                } finally {
                    all.recycle();
                }
            }
        });
        Log.i(Shims.TAG, "render scale: " + sv.getClass().getName() + " at " + Math.round(s * 100) + "%");
    }

    private static final int TAG_KEY = 0x7f5242a1;  // a view-tag key in the application's id space, unlikely to clash
}
