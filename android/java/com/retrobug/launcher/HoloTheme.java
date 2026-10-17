// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The look: Android's Holo dark (Ice Cream Sandwich to KitKat), the era the games come from. Flat near-black surfaces,
// Roboto (the platform's own sans-serif families), a single accent — Holo cyan by default, with Holo's green, orange
// and purple as the alternatives (a Xenra-style accent ramp) — and Holo's pressed highlight: the accent at 40 %.
// Everything is built in code, as the rest of the launcher is.
package com.retrobug.launcher;

import android.animation.AnimatorSet;
import android.animation.ObjectAnimator;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.animation.DecelerateInterpolator;
import android.widget.LinearLayout;
import android.widget.TextView;

final class HoloTheme {
    private HoloTheme() {}

    static final int BG = 0xFF101010;
    static final int PANEL = 0xFF1B1B1B;
    static final int PANEL_RAISED = 0xFF262626;
    static final int DIVIDER = 0xFF2A2A2A;
    static final int TEXT = 0xFFFFFFFF;
    static final int TEXT_2 = 0xFFB3B3B3;
    static final int TEXT_3 = 0xFF7A7A7A;

    static final int[] ACCENTS = {0xFF33B5E5, 0xFF99CC00, 0xFFFFBB33, 0xFFAA66CC};
    static final String[] ACCENT_NAMES = {"Holo blue", "Holo green", "Holo orange", "Holo purple"};

    static final Typeface LIGHT = Typeface.create("sans-serif-light", Typeface.NORMAL);
    static final Typeface REGULAR = Typeface.create("sans-serif", Typeface.NORMAL);
    static final Typeface MEDIUM = Typeface.create("sans-serif-medium", Typeface.NORMAL);
    static final Typeface CONDENSED = Typeface.create("sans-serif-condensed", Typeface.NORMAL);

    private static SharedPreferences prefs(Context c) { return c.getSharedPreferences("ui", Context.MODE_PRIVATE); }

    static int accentIndex(Context c) {
        int i = prefs(c).getInt("accent", 0);
        return i >= 0 && i < ACCENTS.length ? i : 0;
    }

    static void setAccentIndex(Context c, int i) { prefs(c).edit().putInt("accent", i).apply(); }

    static int accent(Context c) { return ACCENTS[accentIndex(c)]; }

    /** The accent at Holo's pressed-highlight alpha (40 %). */
    static int pressed(Context c) { return (accent(c) & 0x00FFFFFF) | 0x66000000; }

    static int dp(Context c, float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, c.getResources().getDisplayMetrics());
    }

    static Drawable panel() { return new ColorDrawable(PANEL); }

    /** A grid tile or list row: transparent at rest, the accent highlight while pressed or focused. */
    static Drawable tile(Context c) {
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[] {android.R.attr.state_pressed}, new ColorDrawable(pressed(c)));
        s.addState(new int[] {android.R.attr.state_focused}, new ColorDrawable((accent(c) & 0x00FFFFFF) | 0x33000000));
        s.addState(new int[] {}, new ColorDrawable(Color.TRANSPARENT));
        return s;
    }

    /** Holo's flat button: a raised grey slab; the accent while pressed. `primary` fills it with the accent. */
    static Drawable button(Context c, boolean primary) {
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[] {android.R.attr.state_pressed}, rect(primary ? darken(accent(c)) : pressed(c), 2, c));
        s.addState(new int[] {-android.R.attr.state_enabled}, rect(0xFF202020, 2, c));
        s.addState(new int[] {}, rect(primary ? accent(c) : PANEL_RAISED, 2, c));
        return s;
    }

    static TextView text(Context c, String s, float sp, int color, Typeface tf) {
        TextView v = new TextView(c);
        v.setText(s);
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        v.setTextColor(color);
        v.setTypeface(tf);
        return v;
    }

    static TextView buttonView(Context c, String label, boolean primary) {
        TextView b = text(c, label, 16, primary ? 0xFF0B2A36 : TEXT, MEDIUM);
        if (primary) b.setTextColor(darkOn(accent(c)));
        b.setGravity(Gravity.CENTER);
        b.setAllCaps(true);
        b.setLetterSpacing(0.05f);
        b.setBackground(button(c, primary));
        b.setPadding(dp(c, 20), dp(c, 12), dp(c, 20), dp(c, 12));
        b.setClickable(true);
        b.setFocusable(true);
        return b;
    }

    /** The action bar: the app's name on a panel with Holo's solid accent underline; `actions` on the right. */
    static LinearLayout actionBar(Context c, String title, View... actions) {
        LinearLayout bar = new LinearLayout(c);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(dp(c, 16), 0, dp(c, 8), dp(c, 3));
        LayerDrawable bg = new LayerDrawable(new Drawable[] {new ColorDrawable(PANEL), new ColorDrawable(accent(c))});
        bg.setLayerInset(1, 0, dp(c, 53), 0, 0);  // the 3 dp underline at the bottom of a 56 dp bar
        bar.setBackground(bg);
        bar.setMinimumHeight(dp(c, 56));
        TextView t = text(c, title, 21, TEXT, REGULAR);
        bar.addView(t, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        for (View a : actions) bar.addView(a);
        return bar;
    }

    /** An action-bar action: a caps label in the accent, Holo's text action. */
    static TextView barAction(Context c, String label) {
        TextView a = text(c, label, 14, accent(c), MEDIUM);
        a.setAllCaps(true);
        a.setGravity(Gravity.CENTER);
        a.setPadding(dp(c, 14), dp(c, 14), dp(c, 14), dp(c, 14));
        a.setBackground(tile(c));
        a.setClickable(true);
        return a;
    }

    /** Xenra's card entrance: fade in and rise 24 dp over 220 ms, staggered 40 ms per index (capped). */
    static void fadeRise(View v, int index) {
        v.setAlpha(0f);
        v.setTranslationY(dp(v.getContext(), 24));
        AnimatorSet s = new AnimatorSet();
        s.playTogether(ObjectAnimator.ofFloat(v, View.ALPHA, 0f, 1f), ObjectAnimator.ofFloat(v, View.TRANSLATION_Y,
                v.getTranslationY(), 0f));
        s.setDuration(220);
        s.setStartDelay(Math.min(index, 10) * 40L);
        s.setInterpolator(new DecelerateInterpolator());
        s.start();
    }

    private static GradientDrawable rect(int color, float radiusDp, Context c) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(dp(c, radiusDp));
        return g;
    }

    static int darken(int color) {
        int r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
        return 0xFF000000 | ((r * 3 / 4) << 16) | ((g * 3 / 4) << 8) | (b * 3 / 4);
    }

    /** Text that reads on the accent: near-black for the light Holo accents. */
    static int darkOn(int accent) { return 0xFF101010; }
}
