// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The home screen: every 32-bit game on this device and every APK the player added, as an app-drawer grid. A tap does
// what the game's state says — make it playable (convert, then Android's install prompt) or open its converted copy.
// A long press opens the game's menu. Installs and uninstalls anywhere on the device refresh the grid.
package com.retrobug.launcher;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Bitmap;
import android.os.Build;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.GridView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.function.Consumer;

final class LibraryScreen extends LinearLayout {
    interface Host {
        void addApk();
        void openSettings();
        /** A document picker; `done` gets the chosen file (not called when the player backs out). */
        void pickFile(Intent intent, Consumer<android.net.Uri> done);
        /** A picked file's display name, or null. */
        String nameOf(android.net.Uri uri);
    }

    void pickFile(Intent intent, Consumer<android.net.Uri> done) { host.pickFile(intent, done); }

    String nameOf(android.net.Uri uri) { return host.nameOf(uri); }

    private static final int RECEIVER_NOT_EXPORTED = 0x4;  // Context.RECEIVER_NOT_EXPORTED (API 33)

    private final Host host;
    private final GameCatalog catalog;
    private final ArtCache art;
    private final GameSettingsStore settings;
    private final List<Game> games = new ArrayList<>();
    /** The keys of the games being converted: a rescan's fresh Game objects learn it from here, not from the old ones. */
    private final java.util.Set<String> converting = new java.util.HashSet<>();
    private final Adapter adapter = new Adapter();
    private final TextView empty;
    private final GridView grid;
    private boolean scanning = false, rescan = false;
    private int shown = 0;

    private final BroadcastReceiver packages = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent i) { refresh(); }
    };

    LibraryScreen(Context c, Executor bg, Host host) {
        super(c);
        this.host = host;
        this.catalog = new GameCatalog(c, bg);
        this.art = new ArtCache(c, bg);
        this.settings = new GameSettingsStore(c);
        setOrientation(VERTICAL);
        setBackgroundColor(HoloTheme.BG);
        TextView add = HoloTheme.barAction(c, "Add APK");
        add.setOnClickListener(new OnClickListener() {
            @Override public void onClick(View v) { LibraryScreen.this.host.addApk(); }
        });
        TextView more = HoloTheme.barAction(c, "Settings");
        more.setOnClickListener(new OnClickListener() {
            @Override public void onClick(View v) { LibraryScreen.this.host.openSettings(); }
        });
        addView(HoloTheme.actionBar(c, "RetroBug", add, more));
        empty = HoloTheme.text(c, "Looking for your games…", 16, HoloTheme.TEXT_2, HoloTheme.LIGHT);
        empty.setGravity(Gravity.CENTER);
        int p = HoloTheme.dp(c, 32);
        empty.setPadding(p, p, p, p);
        addView(empty, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));
        grid = new GridView(c);
        grid.setColumnWidth(HoloTheme.dp(c, 128));
        grid.setNumColumns(GridView.AUTO_FIT);
        grid.setStretchMode(GridView.STRETCH_SPACING_UNIFORM);
        grid.setVerticalSpacing(HoloTheme.dp(c, 4));
        int gp = HoloTheme.dp(c, 8);
        grid.setPadding(gp, gp, gp, gp);
        grid.setClipToPadding(false);
        grid.setSelector(new android.graphics.drawable.ColorDrawable(0));
        grid.setAdapter(adapter);
        grid.setOnItemClickListener(new android.widget.AdapterView.OnItemClickListener() {
            @Override public void onItemClick(android.widget.AdapterView<?> a, View v, int pos, long id) { tap(games.get(pos)); }
        });
        grid.setOnItemLongClickListener(new android.widget.AdapterView.OnItemLongClickListener() {
            @Override public boolean onItemLongClick(android.widget.AdapterView<?> a, View v, int pos, long id) {
                GameMenu.show(getContext(), games.get(pos), art, settings, LibraryScreen.this);
                return true;
            }
        });
        addView(grid, new LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f));
    }

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        IntentFilter f = new IntentFilter();
        f.addAction(Intent.ACTION_PACKAGE_ADDED);
        f.addAction(Intent.ACTION_PACKAGE_REMOVED);
        f.addAction(Intent.ACTION_PACKAGE_REPLACED);
        f.addDataScheme("package");
        if (Build.VERSION.SDK_INT >= 33) getContext().registerReceiver(packages, f, RECEIVER_NOT_EXPORTED);
        else getContext().registerReceiver(packages, f);
        refresh();
    }

    @Override protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        try {
            getContext().unregisterReceiver(packages);
        } catch (IllegalArgumentException ignored) {
            // never registered
        }
    }

    /** Rescans the device; a request during a scan runs once more after it. */
    void refresh() {
        if (scanning) {
            rescan = true;
            return;
        }
        scanning = true;
        final boolean first = games.isEmpty();
        catalog.refresh(new GameCatalog.Listener() {
            @Override public void onGame(Game g) {
                if (first) {  // the first scan fills the grid as games are found; later ones swap in at the end
                    games.add(g);
                    adapter.notifyDataSetChanged();
                    empty.setVisibility(GONE);
                }
            }

            @Override public void onDone(List<Game> all) {
                LibraryRules.keepConverting(all, converting);  // a conversion in flight outlives the rescans it triggers
                games.clear();
                games.addAll(all);
                adapter.notifyDataSetChanged();
                empty.setText("No 32-bit games on this device yet. Add an APK to get started.");
                empty.setVisibility(games.isEmpty() ? VISIBLE : GONE);
                scanning = false;
                if (rescan) {
                    rescan = false;
                    refresh();
                }
            }
        });
    }

    private Game find(String key) {
        for (Game g : games) if (g.key.equals(key)) return g;
        return null;
    }

    void tap(final Game g) {
        Context c = getContext();
        switch (g.state) {
            case READY: {
                Intent launch = c.getPackageManager().getLaunchIntentForPackage(g.copyPackage());
                if (launch == null) {
                    Toast.makeText(c, "Couldn't open " + g.label + ". Try making it playable again.", Toast.LENGTH_LONG).show();
                    refresh();
                    return;
                }
                c.startActivity(launch);
                return;
            }
            case UNREADABLE:
                Toast.makeText(c, g.reason(), Toast.LENGTH_LONG).show();
                return;
            case CONVERTING:
                // Backed out of Android's install prompt? This brings it back; the conversion is waiting on it.
                if (!RepackInstaller.reconfirm(c)) {
                    Toast.makeText(c, "Already getting " + g.label + " ready", Toast.LENGTH_SHORT).show();
                }
                return;
            default:
                convert(g);
        }
    }

    void convert(final Game g) {
        final Context c = getContext();
        final Game.State before = g.state;
        final String key = g.key;
        boolean started = ConvertJob.start(c, g, settings.get(g.packageName), new ConvertJob.Callback() {
            @Override public void onProgress(String line) {}

            @Override public void onDone(boolean ok, String report) {
                converting.remove(key);
                Game now = find(key);  // a rescan during the conversion replaced the Game object the tap started with
                if (now != null) now.state = ok ? Game.State.READY : before;
                adapter.notifyDataSetChanged();
                if (!ok) {
                    Toast.makeText(c, "Couldn't make " + g.label + " playable. Long press it for details.",
                            Toast.LENGTH_LONG).show();
                } else if (report != null && report.contains("\nobb: ") && !report.contains("\nobb: installed")) {
                    // Installed, but its OBB copy failed: the game would sit at its loading screen without saying why.
                    String why = report.substring(report.lastIndexOf("\nobb: ") + 6).trim();
                    Toast.makeText(c, g.label + " is ready, but its OBB wasn't copied: " + why
                            + ". Long press it → Data to try again.", Toast.LENGTH_LONG).show();
                }
                refresh();
            }
        });
        if (!started) {
            Toast.makeText(c, "Another game is getting ready. Try again in a moment.", Toast.LENGTH_SHORT).show();
            return;
        }
        converting.add(key);
        g.state = Game.State.CONVERTING;
        adapter.notifyDataSetChanged();
    }

    private final class Adapter extends BaseAdapter {
        @Override public int getCount() { return games.size(); }
        @Override public Object getItem(int i) { return games.get(i); }
        @Override public long getItemId(int i) { return i; }

        @Override public View getView(int pos, View convert, ViewGroup parent) {
            final GameTile t = convert instanceof GameTile ? (GameTile) convert : new GameTile(getContext());
            final Game g = games.get(pos);
            boolean fresh = t.game != g;
            t.bind(g);
            if (fresh) {
                t.setIcon(null);
                art.icon(g, new Consumer<Bitmap>() {
                    @Override public void accept(Bitmap b) {
                        if (t.game == g) t.setIcon(b);
                    }
                });
                if (pos >= shown) {
                    HoloTheme.fadeRise(t, pos - shown);
                    shown = pos + 1;
                }
            }
            return t;
        }
    }
}
