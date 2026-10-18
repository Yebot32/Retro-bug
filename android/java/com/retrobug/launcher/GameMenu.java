// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// A game's menu, opened by a long press on its tile. The header is the game's page: its best artwork from inside the
// APK, dimmed and softened, with the icon, title and the one action a tap would take. Below it, five Holo tabs —
// Settings (per game), Mods and Patches (empty until M6 fills them), Data (the OBB, save export and import), Delete
// (the converted copy, or the added APK; the original installed game is never touched).
package com.retrobug.launcher;

import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.drawable.ColorDrawable;
import android.net.Uri;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.CompoundButton;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.Set;
import java.util.function.Consumer;

final class GameMenu {
    private static final String[] TABS = {"Settings", "Mods", "Patches", "Data", "Delete"};
    private static final int DATA_TAB = 3;
    private int current = 0;
    /** The Data tab's line for work in progress (an OBB copy, an export), shown until the tab is rebuilt. */
    private String busy;
    private static final int[] TARGETS = {35, 33, 28, 24};

    private final Context c;
    private final Game game;
    private final LibraryScreen library;
    private final GameSettingsStore store;
    private final GameSettings settings;
    private final Dialog dialog;
    private final FrameLayout content;
    private TextView details;
    private final TextView[] tabViews = new TextView[TABS.length];

    static void show(Context c, Game g, ArtCache art, GameSettingsStore store, LibraryScreen library) {
        new GameMenu(c, g, art, store, library).dialog.show();
    }

    private GameMenu(Context c, Game g, ArtCache art, GameSettingsStore store, LibraryScreen library) {
        this.c = c;
        this.game = g;
        this.library = library;
        this.store = store;
        this.settings = store.get(g.packageName);
        dialog = new Dialog(c, android.R.style.Theme_Holo_Dialog_NoActionBar);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout root = new LinearLayout(c);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(HoloTheme.PANEL);
        int sh0 = c.getResources().getDisplayMetrics().heightPixels;
        root.addView(header(art), new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                Math.max(HoloTheme.dp(c, 200), (int) (sh0 * 0.88f * 0.38f))));
        details = HoloTheme.text(c, "", 13, HoloTheme.TEXT_2, android.graphics.Typeface.MONOSPACE);
        int dp16 = HoloTheme.dp(c, 16);
        details.setPadding(HoloTheme.dp(c, 20), dp16, HoloTheme.dp(c, 20), dp16);
        details.setBackgroundColor(0xFF151515);
        details.setVisibility(View.GONE);
        details.setTextIsSelectable(true);
        root.addView(details);
        root.addView(tabs());
        content = new FrameLayout(c);
        root.addView(content, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        dialog.setContentView(root);
        select(0);
        Window w = dialog.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(HoloTheme.PANEL));
            int sw = c.getResources().getDisplayMetrics().widthPixels, sh = c.getResources().getDisplayMetrics().heightPixels;
            w.setLayout(Math.min((int) (sw * 0.94f), HoloTheme.dp(c, 760)), (int) (sh * 0.88f));
        }
    }

    // ------------------------------------------------------------------ header (the game's page)

    private View header(ArtCache art) {
        FrameLayout h = new FrameLayout(c);
        final ImageView backdrop = new ImageView(c);
        backdrop.setScaleType(ImageView.ScaleType.CENTER_CROP);
        backdrop.setBackgroundColor(0xFF000000);
        h.addView(backdrop, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        View shade = new View(c);
        shade.setBackgroundColor(0x99000000);  // dimmed so the title reads over any art
        h.addView(shade, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        art.backdrop(game, new Consumer<Bitmap>() {
            @Override public void accept(Bitmap b) { if (b != null) backdrop.setImageBitmap(b); }  // softened by ArtCache
        });

        LinearLayout row = new LinearLayout(c);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        int p = HoloTheme.dp(c, 20);
        row.setPadding(p, HoloTheme.dp(c, 64), p, p);
        final ImageView icon = new ImageView(c);
        art.icon(game, new Consumer<Bitmap>() {
            @Override public void accept(Bitmap b) {
                if (b != null) icon.setImageBitmap(b);
                else icon.setImageResource(android.R.drawable.sym_def_app_icon);
            }
        });
        int is = HoloTheme.dp(c, 72);
        row.addView(icon, new LinearLayout.LayoutParams(is, is));
        LinearLayout titles = new LinearLayout(c);
        titles.setOrientation(LinearLayout.VERTICAL);
        titles.setPadding(HoloTheme.dp(c, 16), 0, HoloTheme.dp(c, 12), 0);
        titles.addView(HoloTheme.text(c, game.label, 24, HoloTheme.TEXT, HoloTheme.LIGHT));
        titles.addView(HoloTheme.text(c, stateLine(), 13, HoloTheme.TEXT_2, HoloTheme.REGULAR));
        TextView more = HoloTheme.text(c, "Details", 13, HoloTheme.accent(c), HoloTheme.MEDIUM);
        more.setAllCaps(true);
        more.setPadding(0, HoloTheme.dp(c, 8), HoloTheme.dp(c, 8), HoloTheme.dp(c, 4));
        more.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { toggleDetails(); }
        });
        titles.addView(more);
        row.addView(titles, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView action = HoloTheme.buttonView(c, actionLabel(), true);
        action.setEnabled(game.state != Game.State.CONVERTING && game.state != Game.State.UNREADABLE);
        action.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                dialog.dismiss();
                library.tap(game);
            }
        });
        boolean narrow = c.getResources().getConfiguration().screenWidthDp < 600;
        View bottom = row;
        if (narrow) {
            // A phone in portrait: the action goes under the title, full width, so the name keeps its line.
            LinearLayout col = new LinearLayout(c);
            col.setOrientation(LinearLayout.VERTICAL);
            row.setPadding(p, HoloTheme.dp(c, 48), p, HoloTheme.dp(c, 12));
            col.addView(row);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.setMargins(p, 0, p, p);
            col.addView(action, lp);
            bottom = col;
        } else {
            row.addView(action);
        }
        h.addView(bottom, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM));
        return h;
    }

    /** The Details fold: package, version, size, the 32-bit libraries, and the end of the last conversion report. */
    private void toggleDetails() {
        if (details.getVisibility() == View.VISIBLE) {
            details.setVisibility(View.GONE);
            return;
        }
        details.setText("Reading…");
        details.setVisibility(View.VISIBLE);
        final File reportDir = new File(c.getExternalFilesDir(null), "repack");
        new Thread(new Runnable() {  // the APK's zip directory can be large: read off the UI thread
            @Override public void run() {
                StringBuilder s = new StringBuilder();
                s.append("package   ").append(game.packageName).append('\n');
                s.append("version   ").append(game.versionCode).append('\n');
                s.append("size      ").append(game.apk.length() >> 20).append(" MB").append('\n');
                s.append("source    ").append(game.installed ? "installed on this device" : "added APK").append('\n');
                try {
                    Set<String> abis = ApkScan.abis(game.apk);
                    s.append("libraries ").append(abis).append('\n');
                } catch (Exception e) {
                    s.append("libraries unreadable\n");
                }
                File report = new File(reportDir, game.copyPackage() + ".txt");
                if (report.isFile()) s.append("\nlast conversion\n").append(readTail(report, 1200));
                final String text = s.toString().trim();
                details.post(new Runnable() {
                    @Override public void run() { details.setText(text); }
                });
            }
        }, "rb-details").start();
    }

    private static String readTail(File f, int max) {
        try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(f, "r")) {
            long start = Math.max(0, r.length() - max);
            r.seek(start);
            byte[] b = new byte[(int) (r.length() - start)];
            r.readFully(b);
            return new String(b, "UTF-8");
        } catch (Exception e) {
            return "";
        }
    }

    /**
     * A soft-focus backdrop without RenderScript: scaled to about 320 px wide, then three passes of a box blur in each
     * direction (close to a Gaussian); the image view scales it up with filtering.
     */
    static Bitmap soften(Bitmap b) {
        float k = Math.min(1f, 320f / Math.max(1, b.getWidth()));
        int w = Math.max(1, Math.round(b.getWidth() * k)), h = Math.max(1, Math.round(b.getHeight() * k));
        Bitmap small = Bitmap.createScaledBitmap(b, w, h, true).copy(Bitmap.Config.ARGB_8888, true);
        int[] px = new int[w * h], tmp = new int[w * h];
        small.getPixels(px, 0, w, 0, 0, w, h);
        for (int pass = 0; pass < 3; pass++) {
            boxBlur(px, tmp, w, h, 6, true);
            boxBlur(tmp, px, w, h, 6, false);
        }
        small.setPixels(px, 0, w, 0, 0, w, h);
        return small;
    }

    /** One box-blur pass of radius `r`, rows (`horizontal`) or columns, from `in` to `out`. */
    private static void boxBlur(int[] in, int[] out, int w, int h, int r, boolean horizontal) {
        int lines = horizontal ? h : w, len = horizontal ? w : h;
        for (int line = 0; line < lines; line++) {
            int ra = 0, rr = 0, rg = 0, rb = 0;
            for (int i = -r; i <= r; i++) {
                int p = in[index(line, Math.max(0, Math.min(len - 1, i)), w, horizontal)];
                ra += p >>> 24; rr += (p >> 16) & 0xFF; rg += (p >> 8) & 0xFF; rb += p & 0xFF;
            }
            int n = 2 * r + 1;
            for (int i = 0; i < len; i++) {
                out[index(line, i, w, horizontal)] = (ra / n) << 24 | (rr / n) << 16 | (rg / n) << 8 | (rb / n);
                int add = in[index(line, Math.min(len - 1, i + r + 1), w, horizontal)];
                int sub = in[index(line, Math.max(0, i - r), w, horizontal)];
                ra += (add >>> 24) - (sub >>> 24);
                rr += ((add >> 16) & 0xFF) - ((sub >> 16) & 0xFF);
                rg += ((add >> 8) & 0xFF) - ((sub >> 8) & 0xFF);
                rb += (add & 0xFF) - (sub & 0xFF);
            }
        }
    }

    private static int index(int line, int i, int w, boolean horizontal) {
        return horizontal ? line * w + i : i * w + line;
    }

    private String actionLabel() {
        switch (game.state) {
            case READY: return "Play";
            case UPDATE: return "Update";
            case CONVERTING: return "Getting ready";
            default: return "Make playable";
        }
    }

    private String stateLine() {
        switch (game.state) {
            case READY: return "Ready to play";
            case UPDATE: return "A newer RetroBug can update this game's copy";
            case CONVERTING: return "Getting ready…";
            case UNREADABLE: return game.reason();
            default: return game.installed ? "Installed on this device · not playable yet" : "Added APK · not playable yet";
        }
    }

    // ------------------------------------------------------------------ tabs

    private View tabs() {
        LinearLayout bar = new LinearLayout(c);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setBackgroundColor(0xFF222222);
        for (int i = 0; i < TABS.length; i++) {
            final int k = i;
            TextView t = HoloTheme.text(c, TABS[i], 14, HoloTheme.TEXT_2, HoloTheme.MEDIUM);
            t.setAllCaps(true);
            t.setGravity(Gravity.CENTER);
            t.setPadding(0, HoloTheme.dp(c, 14), 0, HoloTheme.dp(c, 14));
            t.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) { select(k); }
            });
            tabViews[i] = t;
            bar.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        }
        return bar;
    }

    private void select(int k) {
        for (int i = 0; i < tabViews.length; i++) {
            TextView t = tabViews[i];
            boolean on = i == k;
            t.setTextColor(on ? HoloTheme.TEXT : HoloTheme.TEXT_3);
            // Holo's tab indicator: a 3 dp accent bar under the selected tab.
            android.graphics.drawable.LayerDrawable bg = new android.graphics.drawable.LayerDrawable(new android.graphics.drawable.Drawable[] {
                    HoloTheme.tile(c), new ColorDrawable(on ? HoloTheme.accent(c) : 0)});
            bg.setLayerGravity(1, Gravity.BOTTOM | Gravity.FILL_HORIZONTAL);
            bg.setLayerHeight(1, HoloTheme.dp(c, 3));
            t.setBackground(bg);
        }
        content.removeAllViews();
        current = k;
        View v;
        switch (k) {
            case 0: v = settingsTab(); break;
            case 1: v = emptyTab("No mods yet", "Texture packs, translations and music for this game will appear here, each "
                    + "with its own switch.", "Add a mod pack"); break;
            case 2: v = emptyTab("No patches yet", "Code patches, fixes and cheats made for this game's version will appear "
                    + "here, each with its own switch.", "Add a patch"); break;
            case DATA_TAB: v = dataTab(); break;
            default: v = deleteTab(); break;
        }
        content.addView(v);
    }

    // ------------------------------------------------------------------ Settings

    private boolean copyInstalled() {
        try {
            c.getPackageManager().getPackageInfo(game.copyPackage(), 0);
            return true;
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }

    private View settingsTab() {
        LinearLayout list = list();
        final boolean copy = copyInstalled();
        list.addView(row("Permissions", copy ? "What this game is allowed to use, in Android's settings"
                : "Make the game playable first", copy, new View.OnClickListener() {
            @Override public void onClick(View v) { appInfo(); }
        }));
        if (!game.hasCode) {
            // No Java code of its own, so its copy has no settings provider: these two cannot reach it (Fast, no overlay).
            String why = "This game has no Java code, so RetroBug can't pass this setting to it";
            list.addView(row("CPU mode", why, false, null));
            list.addView(row("Performance overlay", why, false, null));
        }
        final TextView[] cpuValue = new TextView[1];
        View cpu = valueRow("CPU mode", cpuText(), "Fast uses the JIT; Safe is slower and useful when a game misbehaves",
                cpuValue, new View.OnClickListener() {
                    @Override public void onClick(View v) {
                        settings.cpu = settings.cpu.equals("fast") ? "safe" : "fast";
                        store.put(game.packageName, settings);
                        cpuValue[0].setText(cpuText());
                    }
                });
        if (game.hasCode) list.addView(cpu);
        final TextView[] targetValue = new TextView[1];
        list.addView(valueRow("Android target", "Android " + version(settings.target),
                "Older targets avoid some newer Android rules; Play Protect may block them. Changing it updates the copy.",
                targetValue, new View.OnClickListener() {
                    @Override public void onClick(View v) {
                        int i = 0;
                        while (i < TARGETS.length && TARGETS[i] != settings.target) i++;
                        settings.target = TARGETS[(i + 1) % TARGETS.length];
                        store.put(game.packageName, settings);
                        targetValue[0].setText("Android " + version(settings.target));
                        library.refresh();  // the copy now reads "Update available"
                    }
                }));
        if (game.hasCode) {
            list.addView(switchRow("Performance overlay", "Frames per second and CPU speed over the game", settings.overlay,
                    new CompoundButton.OnCheckedChangeListener() {
                        @Override public void onCheckedChanged(CompoundButton b, boolean on) {
                            settings.overlay = on;
                            store.put(game.packageName, settings);
                        }
                    }));
        }
        list.addView(row("Clear game data", copy ? "Opens the game's app info, where Storage has Android's own Clear storage"
                : "Make the game playable first", copy, new View.OnClickListener() {
            @Override public void onClick(View v) { appInfo(); }
        }));
        list.addView(row("Resolution", "Coming with M6", false, null));
        return scroll(list);
    }

    private String cpuText() { return settings.cpu.equals("safe") ? "Safe" : "Fast"; }

    private static String version(int sdk) {
        switch (sdk) {
            case 24: return "7.0";
            case 28: return "9";
            case 33: return "13";
            case 35: return "15";
            default: return "API " + sdk;
        }
    }

    private void appInfo() {
        try {
            c.startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", game.copyPackage(), null))
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        } catch (Exception e) {
            Toast.makeText(c, "Couldn't open Android's settings for this game", Toast.LENGTH_SHORT).show();
        }
    }

    // ------------------------------------------------------------------ Mods / Patches

    private View emptyTab(String title, String body, String button) {
        LinearLayout box = list();
        box.setGravity(Gravity.CENTER_HORIZONTAL);
        box.setPadding(HoloTheme.dp(c, 32), HoloTheme.dp(c, 40), HoloTheme.dp(c, 32), HoloTheme.dp(c, 24));
        TextView t = HoloTheme.text(c, title, 20, HoloTheme.TEXT, HoloTheme.LIGHT);
        t.setGravity(Gravity.CENTER);
        box.addView(t);
        TextView b = HoloTheme.text(c, body, 14, HoloTheme.TEXT_2, HoloTheme.REGULAR);
        b.setGravity(Gravity.CENTER);
        b.setPadding(0, HoloTheme.dp(c, 8), 0, HoloTheme.dp(c, 24));
        box.addView(b);
        TextView add = HoloTheme.buttonView(c, button, false);
        add.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { Toast.makeText(c, "Mods and patches come with M6", Toast.LENGTH_SHORT).show(); }
        });
        box.addView(add, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return scroll(box);
    }

    // ------------------------------------------------------------------ Data (OBB and saves)

    private View dataTab() {
        LinearLayout list = list();
        final boolean copy = copyInstalled();
        list.addView(section("Game data (OBB)"));
        if (busy != null) list.addView(row(busy, null, false, null));
        java.util.List<File> src = ObbStore.sources(c, game), have = ObbStore.installed(game);
        boolean mine = ObbStore.added(c, game);
        String status;
        if (!have.isEmpty() && ObbStore.upToDate(c, game)) {
            status = "Installed for RetroBug's copy · " + mb(ObbStore.size(have));
        } else if (!src.isEmpty()) {
            status = (mine ? "Your OBB file" : "The original game's OBB") + " (" + mb(ObbStore.size(src)) + ") · "
                    + (copy ? "not installed for the copy yet" : "installed when the game is made playable");
        } else {
            status = "No OBB. Most games don't need one; if this game says its data is missing, add its .obb file.";
        }
        list.addView(row("OBB", status, false, null));
        list.addView(row("Install OBB", copy ? "Copies it into the folder RetroBug's copy reads"
                : "Make the game playable first", copy && !src.isEmpty() && busy == null && !ObbStore.upToDate(c, game),
                new View.OnClickListener() {
                    @Override public void onClick(View v) { installObb(); }
                }));
        list.addView(row("Add OBB file", "Pick the game's .obb file (main.<version>." + game.packageName + ".obb)",
                busy == null, new View.OnClickListener() {
                    @Override public void onClick(View v) { addObb(); }
                }));
        list.addView(row("Remove OBB", "From RetroBug's copy" + (mine ? ", with the file you added" : "")
                + ". The original game's OBB is never touched.", busy == null && (!have.isEmpty() || mine),
                new View.OnClickListener() {
                    @Override public void onClick(View v) {
                        confirm("Remove the OBB?", "RetroBug's copy of " + game.label + " loses its OBB. The original "
                                + "game's OBB is not touched.", "Remove", new Runnable() {
                            @Override public void run() {
                                ObbStore.remove(game);
                                ObbStore.forgetAdded(c, game);
                                select(DATA_TAB);
                            }
                        });
                    }
                }));

        list.addView(section("Saves"));
        boolean saves = copy && game.hasCode;
        String why = !game.hasCode ? "This game has no Java code, so RetroBug can't reach its saves"
                : !copy ? "Make the game playable first" : null;
        list.addView(row("Export saves", why != null ? why : "Saves the game's progress as a .zip wherever you choose",
                saves && busy == null, new View.OnClickListener() {
                    @Override public void onClick(View v) { exportSaves(); }
                }));
        list.addView(row("Import saves", why != null ? why : "Replaces the game's progress with a saved .zip. The current "
                + "progress is kept so you can undo.", saves && busy == null, new View.OnClickListener() {
                    @Override public void onClick(View v) {
                        confirm("Import saves?", game.label + " closes if it is open, and its progress is replaced by the "
                                + "save you pick. The current progress is kept: Undo import brings it back.", "Pick a save",
                                new Runnable() {
                                    @Override public void run() { importSaves(); }
                                });
                    }
                }));
        final View undo = row("Undo import", "Puts back the progress from before the last import", false, null);
        list.addView(undo);
        if (saves) {
            background(new Runnable() {
                @Override public void run() {
                    final Boolean can = SaveClient.hasUndo(c, game);
                    content.post(new Runnable() {
                        @Override public void run() {
                            if (current != DATA_TAB || undo.getParent() == null || !Boolean.TRUE.equals(can)) return;
                            ViewGroup parent = (ViewGroup) undo.getParent();
                            int at = parent.indexOfChild(undo);
                            parent.removeView(undo);
                            parent.addView(row("Undo import", "Puts back the progress from before the last import", true,
                                    new View.OnClickListener() {
                                        @Override public void onClick(View v) { undoImport(); }
                                    }), at);
                        }
                    });
                }
            });
        }
        return scroll(list);
    }

    private static String mb(long bytes) { return (bytes >> 20) + " MB"; }

    private View section(String title) {
        TextView t = HoloTheme.text(c, title, 14, HoloTheme.accent(c), HoloTheme.MEDIUM);
        t.setAllCaps(true);
        t.setPadding(HoloTheme.dp(c, 20), HoloTheme.dp(c, 20), HoloTheme.dp(c, 20), HoloTheme.dp(c, 6));
        return t;
    }

    private void background(Runnable r) { new Thread(r, "rb-data").start(); }

    /** Runs `work` off the UI thread with `label` as the Data tab's busy line, then toasts its answer (null = `ok`). */
    private void busyWith(String label, final String ok, final java.util.concurrent.Callable<String> work) {
        busy = label;
        if (current == DATA_TAB) select(DATA_TAB);
        background(new Runnable() {
            @Override public void run() {
                String why;
                try {
                    why = work.call();
                } catch (Exception e) {
                    why = String.valueOf(e.getMessage());
                }
                final String msg = why == null ? ok : why;
                content.post(new Runnable() {
                    @Override public void run() {
                        busy = null;
                        if (current == DATA_TAB) select(DATA_TAB);
                        Toast.makeText(c, msg, Toast.LENGTH_LONG).show();
                    }
                });
            }
        });
    }

    /** Progress into the busy line, from the copying thread. */
    private ObbStore.Progress progress(final String verb) {
        return new ObbStore.Progress() {
            @Override public void at(final long done, final long total) {
                content.post(new Runnable() {
                    @Override public void run() {
                        if (busy == null) return;
                        busy = verb + " " + (total > 0 ? (done * 100 / total) + " %" : mb(done)) + "…";
                        if (current == DATA_TAB) select(DATA_TAB);
                    }
                });
            }
        };
    }

    private void installObb() {
        final ObbStore.Progress p = progress("Installing the OBB");
        busyWith("Installing the OBB…", "OBB installed for " + game.label, new java.util.concurrent.Callable<String>() {
            @Override public String call() { return ObbStore.install(c, game, p); }
        });
    }

    private void addObb() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        library.pickFile(i, new Consumer<Uri>() {
            @Override public void accept(final Uri uri) {
                final String name = library.nameOf(uri);
                final ObbStore.Progress p = progress("Adding the OBB");
                busyWith("Adding the OBB…", "OBB added for " + game.label, new java.util.concurrent.Callable<String>() {
                    @Override public String call() {
                        String why = ObbStore.add(c, game, uri, name, p);
                        if (why == null && copyInstalled()) why = ObbStore.install(c, game, progress("Installing the OBB"));
                        return why;
                    }
                });
            }
        });
    }

    private void exportSaves() {
        String day = new java.text.SimpleDateFormat("yyyy-MM-dd", java.util.Locale.ROOT).format(new java.util.Date());
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("application/zip")
                .putExtra(Intent.EXTRA_TITLE, game.label.replaceAll("[\\\\/:*?\"<>|]", "_") + " - save " + day + ".zip");
        library.pickFile(i, new Consumer<Uri>() {
            @Override public void accept(final Uri uri) {
                busyWith("Exporting saves…", "Saves exported", new java.util.concurrent.Callable<String>() {
                    @Override public String call() { return SaveClient.export(c, game, uri); }
                });
            }
        });
    }

    private void importSaves() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("application/zip")
                .putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/octet-stream",
                        "application/x-zip-compressed"});
        library.pickFile(i, new Consumer<Uri>() {
            @Override public void accept(final Uri uri) {
                busyWith("Importing saves…", "Saves imported. " + game.label + " starts from them next time.",
                        new java.util.concurrent.Callable<String>() {
                            @Override public String call() { return SaveClient.importFrom(c, game, uri); }
                        });
            }
        });
    }

    private void undoImport() {
        confirm("Undo the import?", game.label + " closes if it is open and goes back to the progress it had before the "
                + "last import.", "Undo", new Runnable() {
            @Override public void run() {
                busyWith("Putting the old saves back…", "Old saves restored", new java.util.concurrent.Callable<String>() {
                    @Override public String call() { return SaveClient.undo(c, game); }
                });
            }
        });
    }

    // ------------------------------------------------------------------ Delete

    private View deleteTab() {
        LinearLayout list = list();
        final boolean copy = copyInstalled();
        list.addView(row("Remove the playable copy", copy ? "Uninstalls RetroBug's copy of " + game.label
                + ". Its saves go with it. The original game stays." : "There is no playable copy yet", copy,
                new View.OnClickListener() {
                    @Override public void onClick(View v) {
                        confirm("Remove the playable copy?", "RetroBug's copy of " + game.label + " and its saves are "
                                + "removed. The original game is not touched.", "Remove", new Runnable() {
                            @Override public void run() {
                                dialog.dismiss();
                                c.startActivity(new Intent(Intent.ACTION_DELETE, Uri.parse("package:" + game.copyPackage()))
                                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                            }
                        });
                    }
                }));
        if (game.added) {
            final File file = addedFile();
            list.addView(row("Remove from RetroBug", "Deletes the APK you added"
                    + (game.installed ? ". The installed game stays in the library." : " and its settings."), file != null,
                    new View.OnClickListener() {
                        @Override public void onClick(View v) {
                            confirm("Remove from RetroBug?", "The APK you added for " + game.label + " is deleted from "
                                    + "RetroBug.", "Remove", new Runnable() {
                                @Override public void run() {
                                    if (file != null && !file.delete()) {
                                        Toast.makeText(c, "Couldn't remove the file", Toast.LENGTH_SHORT).show();
                                        return;
                                    }
                                    if (!game.installed) store.remove(game.packageName);
                                    dialog.dismiss();
                                    library.refresh();
                                }
                            });
                        }
                    }));
        }
        return scroll(list);
    }

    /** The added APK in the library folder for this game, as the scan found it (never the installed game's own APK). */
    private File addedFile() {
        return game.addedFile != null ? game.addedFile : game.installed ? null : game.apk;
    }

    private void confirm(String title, String body, String yes, final Runnable then) {
        new AlertDialog.Builder(c, android.R.style.Theme_Holo_Dialog)
                .setTitle(title).setMessage(body)
                .setNegativeButton("Cancel", null)
                .setPositiveButton(yes, new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface d, int w) { then.run(); }
                }).show();
    }

    // ------------------------------------------------------------------ rows

    private LinearLayout list() {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    private View scroll(View v) {
        ScrollView s = new ScrollView(c);
        s.addView(v);
        return s;
    }

    private LinearLayout rowBase(String title, String sub, boolean enabled) {
        LinearLayout r = new LinearLayout(c);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        int p = HoloTheme.dp(c, 20);
        r.setPadding(p, HoloTheme.dp(c, 14), p, HoloTheme.dp(c, 14));
        LinearLayout texts = new LinearLayout(c);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(HoloTheme.text(c, title, 17, enabled ? HoloTheme.TEXT : HoloTheme.TEXT_3, HoloTheme.REGULAR));
        if (sub != null) texts.addView(HoloTheme.text(c, sub, 13, enabled ? HoloTheme.TEXT_2 : HoloTheme.TEXT_3, HoloTheme.REGULAR));
        r.addView(texts, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        return r;
    }

    private View divided(View row) {
        LinearLayout w = list();
        w.addView(row);
        View d = new View(c);
        d.setBackgroundColor(HoloTheme.DIVIDER);
        w.addView(d, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 1));
        return w;
    }

    private View row(String title, String sub, boolean enabled, View.OnClickListener click) {
        LinearLayout r = rowBase(title, sub, enabled);
        if (enabled && click != null) {
            r.setBackground(HoloTheme.tile(c));
            r.setOnClickListener(click);
        }
        return divided(r);
    }

    private View valueRow(String title, String value, String sub, TextView[] valueOut, View.OnClickListener click) {
        LinearLayout r = rowBase(title, sub, true);
        TextView v = HoloTheme.text(c, value, 15, HoloTheme.accent(c), HoloTheme.MEDIUM);
        v.setPadding(HoloTheme.dp(c, 16), 0, 0, 0);
        valueOut[0] = v;
        r.addView(v);
        r.setBackground(HoloTheme.tile(c));
        r.setOnClickListener(click);
        return divided(r);
    }

    private View switchRow(String title, String sub, boolean on, CompoundButton.OnCheckedChangeListener l) {
        LinearLayout r = rowBase(title, sub, true);
        final Switch s = new Switch(c);
        s.setChecked(on);
        s.setOnCheckedChangeListener(l);
        r.addView(s);
        r.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { s.toggle(); }
        });
        r.setBackground(HoloTheme.tile(c));
        return divided(r);
    }
}
