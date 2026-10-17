// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// One game in the library grid, app-drawer style: its own icon, its name, and a small badge saying what a tap does.
// While the game is being made playable, a ring turns over the icon.
package com.retrobug.launcher;

import android.content.Context;
import android.graphics.Bitmap;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

final class GameTile extends LinearLayout {
    private final ImageView icon;
    private final ProgressBar ring;
    private final TextView name, badge;
    Game game;

    GameTile(Context c) {
        super(c);
        setOrientation(VERTICAL);
        setGravity(Gravity.CENTER_HORIZONTAL);
        int pad = HoloTheme.dp(c, 8);
        setPadding(pad, HoloTheme.dp(c, 14), pad, HoloTheme.dp(c, 12));
        setBackground(HoloTheme.tile(c));
        FrameLayout art = new FrameLayout(c);
        icon = new ImageView(c);
        icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
        int size = HoloTheme.dp(c, 72);
        art.addView(icon, new FrameLayout.LayoutParams(size, size, Gravity.CENTER));
        ring = new ProgressBar(c, null, android.R.attr.progressBarStyleLarge);
        ring.setIndeterminate(true);
        ring.setVisibility(GONE);
        art.addView(ring, new FrameLayout.LayoutParams(size, size, Gravity.CENTER));
        addView(art, new LayoutParams(size, size));
        name = HoloTheme.text(c, "", 14, HoloTheme.TEXT, HoloTheme.REGULAR);
        name.setGravity(Gravity.CENTER);
        name.setMaxLines(2);
        name.setEllipsize(TextUtils.TruncateAt.END);
        LayoutParams lp = new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT);
        lp.topMargin = HoloTheme.dp(c, 8);
        addView(name, lp);
        badge = HoloTheme.text(c, "", 11, HoloTheme.TEXT_3, HoloTheme.CONDENSED);
        badge.setGravity(Gravity.CENTER);
        badge.setAllCaps(true);
        addView(badge, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));
    }

    void bind(Game g) {
        game = g;
        name.setText(g.label);
        update();
    }

    void setIcon(Bitmap b) {
        if (b != null) icon.setImageBitmap(b);
        else icon.setImageResource(android.R.drawable.sym_def_app_icon);
    }

    /** Re-reads the game's state into the badge and the ring. */
    void update() {
        Context c = getContext();
        ring.setVisibility(game.state == Game.State.CONVERTING ? VISIBLE : GONE);
        icon.setAlpha(game.state == Game.State.CONVERTING ? 0.35f : 1f);
        switch (game.state) {
            case READY:
                badge.setText("Ready");
                badge.setTextColor(HoloTheme.accent(c));
                break;
            case UPDATE:
                badge.setText("Update available");
                badge.setTextColor(0xFFFFBB33);
                break;
            case CONVERTING:
                badge.setText("Getting ready");
                badge.setTextColor(HoloTheme.TEXT_2);
                break;
            case UNREADABLE:
                badge.setText("Can't read this file");
                badge.setTextColor(0xFFFF4444);
                break;
            default:
                badge.setText("Not converted");
                badge.setTextColor(HoloTheme.TEXT_3);
                break;
        }
    }
}
