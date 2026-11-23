// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The boot animation, in the spirit of Ice Cream Sandwich's: the wordmark in Roboto Light, dim, with a band of the
// accent sweeping across it. About 1.5 s; a tap skips it. The library scans while it plays.
package com.retrobug.launcher;

import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Shader;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;

final class BootScreen extends FrameLayout {
    private final Runnable onFinished;
    private boolean finished = false;
    private final ValueAnimator sweep;

    BootScreen(Context c, Runnable onFinished) {
        super(c);
        this.onFinished = onFinished;
        setBackgroundColor(0xFF000000);
        final Sweep word = new Sweep(c, HoloTheme.accent(c));
        word.setText("RetroBug");
        word.setTextSize(TypedValue.COMPLEX_UNIT_SP, 56);
        word.setTypeface(HoloTheme.LIGHT);
        word.setTextColor(0xFF4A4A4A);
        addView(word, new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        sweep = ValueAnimator.ofFloat(-0.4f, 1.4f);
        sweep.setDuration(1500);
        sweep.addUpdateListener(new ValueAnimator.AnimatorUpdateListener() {
            @Override public void onAnimationUpdate(ValueAnimator a) {
                word.at = (Float) a.getAnimatedValue();
                word.invalidate();
            }
        });
        sweep.addListener(new android.animation.AnimatorListenerAdapter() {
            @Override public void onAnimationEnd(android.animation.Animator a) { finish(); }
        });
        setOnClickListener(new OnClickListener() {
            @Override public void onClick(View v) { finish(); }
        });
    }

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        sweep.start();
    }

    private void finish() {
        if (finished) return;
        finished = true;
        sweep.cancel();
        onFinished.run();
    }

    /** The wordmark: drawn dim, then again through a moving accent gradient — the ICS logo's sweep of light. */
    private static final class Sweep extends TextView {
        float at = -0.4f;
        private final int accent;
        private final Matrix m = new Matrix();

        Sweep(Context c, int accent) {
            super(c);
            this.accent = accent;
        }

        @Override protected void onDraw(Canvas canvas) {
            getPaint().setShader(null);
            super.onDraw(canvas);
            float w = getWidth();
            LinearGradient g = new LinearGradient(0, 0, w * 0.35f, 0,
                    new int[] {0x00000000, accent, 0xFFFFFFFF, accent, 0x00000000}, null, Shader.TileMode.CLAMP);
            m.setTranslate(at * w, 0);
            g.setLocalMatrix(m);
            // The paint's colour (the dim text colour, fully opaque) only lends its alpha to a shader: no re-colouring,
            // which would invalidate the view from inside onDraw.
            getPaint().setShader(g);
            super.onDraw(canvas);
            getPaint().setShader(null);
        }
    }
}
