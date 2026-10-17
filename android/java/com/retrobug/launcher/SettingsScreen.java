// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// RetroBug's own settings, as Holo preference rows: the accent colour (Holo blue, green, orange, purple), what the
// library shows, and About. Tapping the version seven times turns on the Developer tools — the engine self-test,
// reports and the diagnostic run modes — kept out of a player's way, the way Android keeps its own.
package com.retrobug.launcher;

import android.content.Context;
import android.content.SharedPreferences;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

final class SettingsScreen extends LinearLayout {
    interface Host {
        void back();
        void openDeveloper();
        /** The accent changed: the screens are rebuilt in it. */
        void restyle();
    }

    static final String[] SHOW = {"both", "installed", "added"};
    static final String[] SHOW_LABELS = {"Installed games and added APKs", "Installed games only", "Added APKs only"};

    static String libraryShows(Context c) {
        String s = prefs(c).getString("show", "both");
        for (String k : SHOW) if (k.equals(s)) return s;
        return "both";
    }

    static boolean developer(Context c) { return prefs(c).getBoolean("developer", false); }

    private static SharedPreferences prefs(Context c) { return c.getSharedPreferences("ui", Context.MODE_PRIVATE); }

    private final Host host;
    private int taps = 0;

    SettingsScreen(final Context c, final Host host) {
        super(c);
        this.host = host;
        setOrientation(VERTICAL);
        setBackgroundColor(HoloTheme.BG);
        TextView done = HoloTheme.barAction(c, "Done");
        done.setOnClickListener(new OnClickListener() {
            @Override public void onClick(View v) { host.back(); }
        });
        addView(HoloTheme.actionBar(c, "Settings", done));
        LinearLayout list = new LinearLayout(c);
        list.setOrientation(VERTICAL);
        ScrollView scroll = new ScrollView(c);
        scroll.addView(list);
        addView(scroll, new LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f));

        list.addView(header(c, "Look"));
        final TextView accentValue = value(c, HoloTheme.ACCENT_NAMES[HoloTheme.accentIndex(c)]);
        list.addView(row(c, "Accent colour", "The colour of highlights, buttons and the action bar's line", accentValue,
                new OnClickListener() {
                    @Override public void onClick(View v) {
                        int next = (HoloTheme.accentIndex(c) + 1) % HoloTheme.ACCENTS.length;
                        HoloTheme.setAccentIndex(c, next);
                        host.restyle();
                    }
                }));
        list.addView(header(c, "Library"));
        final TextView showValue = value(c, SHOW_LABELS[indexOf(libraryShows(c))]);
        list.addView(row(c, "Library shows", null, showValue, new OnClickListener() {
            @Override public void onClick(View v) {
                int next = (indexOf(libraryShows(c)) + 1) % SHOW.length;
                prefs(c).edit().putString("show", SHOW[next]).apply();
                showValue.setText(SHOW_LABELS[next]);
            }
        }));
        list.addView(header(c, "About"));
        list.addView(row(c, "RetroBug", "Plays 32-bit Android games on 64-bit phones by running their ARM code in an emulator, "
                + "while Android runs the rest of the game as usual.", null, null));
        String version = "1";
        try {
            version = c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName;
        } catch (Exception ignored) {
            // the default stands
        }
        final String v = version;
        list.addView(row(c, "Version", v + " · runtime " + ApkRepackager.RUNTIME_STAMP, null, new OnClickListener() {
            @Override public void onClick(View view) {
                if (developer(c)) {
                    Toast.makeText(c, "Developer tools are already on", Toast.LENGTH_SHORT).show();
                    return;
                }
                taps++;
                if (taps >= 7) {
                    prefs(c).edit().putBoolean("developer", true).apply();
                    Toast.makeText(c, "Developer tools are on", Toast.LENGTH_SHORT).show();
                    host.back();
                } else if (taps >= 4) {
                    Toast.makeText(c, (7 - taps) + " more to turn on developer tools", Toast.LENGTH_SHORT).show();
                }
            }
        }));
        if (developer(c)) {
            list.addView(header(c, "Developer"));
            list.addView(row(c, "Developer tools", "Engine self-test, reports and the diagnostic run modes", null,
                    new OnClickListener() {
                        @Override public void onClick(View view) { host.openDeveloper(); }
                    }));
            list.addView(row(c, "Turn off developer tools", null, null, new OnClickListener() {
                @Override public void onClick(View view) {
                    prefs(c).edit().putBoolean("developer", false).apply();
                    host.restyle();
                }
            }));
        }
    }

    private static int indexOf(String show) {
        for (int i = 0; i < SHOW.length; i++) if (SHOW[i].equals(show)) return i;
        return 0;
    }

    /** A Holo section header: the accent, small caps, with the accent rule under it. */
    private static View header(Context c, String title) {
        TextView t = HoloTheme.text(c, title, 14, HoloTheme.accent(c), HoloTheme.MEDIUM);
        t.setAllCaps(true);
        int p = HoloTheme.dp(c, 20);
        t.setPadding(p, HoloTheme.dp(c, 24), p, HoloTheme.dp(c, 6));
        LinearLayout w = new LinearLayout(c);
        w.setOrientation(VERTICAL);
        w.addView(t);
        View rule = new View(c);
        rule.setBackgroundColor((HoloTheme.accent(c) & 0x00FFFFFF) | 0x88000000);
        LayoutParams lp = new LayoutParams(LayoutParams.MATCH_PARENT, HoloTheme.dp(c, 1));
        lp.setMargins(HoloTheme.dp(c, 16), 0, HoloTheme.dp(c, 16), 0);
        w.addView(rule, lp);
        return w;
    }

    private static TextView value(Context c, String s) {
        TextView v = HoloTheme.text(c, s, 15, HoloTheme.accent(c), HoloTheme.MEDIUM);
        v.setPadding(HoloTheme.dp(c, 16), 0, 0, 0);
        return v;
    }

    private static View row(Context c, String title, String sub, TextView value, OnClickListener click) {
        LinearLayout r = new LinearLayout(c);
        r.setOrientation(HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        int p = HoloTheme.dp(c, 20);
        r.setPadding(p, HoloTheme.dp(c, 14), p, HoloTheme.dp(c, 14));
        LinearLayout texts = new LinearLayout(c);
        texts.setOrientation(VERTICAL);
        texts.addView(HoloTheme.text(c, title, 17, HoloTheme.TEXT, HoloTheme.REGULAR));
        if (sub != null) texts.addView(HoloTheme.text(c, sub, 13, HoloTheme.TEXT_2, HoloTheme.REGULAR));
        r.addView(texts, new LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        if (value != null) r.addView(value);
        if (click != null) {
            r.setBackground(HoloTheme.tile(c));
            r.setOnClickListener(click);
        }
        return r;
    }
}
