// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// A game's icon and backdrop, decoded once and kept: in memory for this run, and as PNGs under getCacheDir()/art/
// keyed by package and version, so the next start shows them without opening the APK again. Decoding happens on the
// executor it is given; results arrive on the main thread. Images are decoded at a reduced sample size (never larger
// than the screen for a backdrop, 192 px for an icon), so a 4096-pixel texture cannot run the launcher out of memory.
package com.retrobug.launcher;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.LruCache;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.concurrent.Executor;
import java.util.function.Consumer;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class ArtCache {
    private static final String TAG = "RetroBug";
    static final int ICON_PX = 192;

    private final Context ctx;
    private final Executor bg;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final File dir;
    private final LruCache<String, Bitmap> mem;

    ArtCache(Context ctx, Executor bg) {
        this.ctx = ctx.getApplicationContext();
        this.bg = bg;
        this.dir = new File(this.ctx.getCacheDir(), "art");
        int kb = (int) (Runtime.getRuntime().maxMemory() / 1024 / 8);  // an eighth of the heap
        this.mem = new LruCache<String, Bitmap>(kb) {
            @Override protected int sizeOf(String k, Bitmap b) { return b.getByteCount() / 1024; }
        };
    }

    void icon(final Game g, final Consumer<Bitmap> done) {
        load(g, "icon", done);
    }

    /** The backdrop: the best large image in the APK (ArtPicker), else the icon. */
    void backdrop(final Game g, final Consumer<Bitmap> done) {
        load(g, "art", done);
    }

    private void load(final Game g, final String kind, final Consumer<Bitmap> done) {
        final String key = g.packageName.replaceAll("[^A-Za-z0-9._-]", "_") + "-" + g.versionCode + "-" + kind;
        Bitmap hit = mem.get(key);
        if (hit != null) {
            done.accept(hit);
            return;
        }
        bg.execute(new Runnable() {
            @Override public void run() {
                Bitmap b = null;
                File cached = new File(dir, key + ".png");
                try {
                    if (cached.isFile()) b = BitmapFactory.decodeFile(cached.getAbsolutePath());
                    if (b == null) {
                        b = kind.equals("icon") ? decodeIcon(g) : decodeBackdrop(g);
                        if (b != null) save(cached, b);
                    }
                } catch (Throwable t) {
                    Log.w(TAG, "art: " + g.packageName + " " + kind + ": " + t);
                }
                if (b != null) mem.put(key, b);
                final Bitmap out = b;
                main.post(new Runnable() {
                    @Override public void run() { done.accept(out); }
                });
            }
        });
    }

    private Bitmap decodeIcon(Game g) {
        PackageManager pm = ctx.getPackageManager();
        Drawable d;
        try {
            if (g.installed) {
                d = pm.getApplicationIcon(g.packageName);
            } else {
                PackageInfo pi = pm.getPackageArchiveInfo(g.apk.getAbsolutePath(), 0);
                if (pi == null || pi.applicationInfo == null) return null;
                pi.applicationInfo.sourceDir = g.apk.getAbsolutePath();
                pi.applicationInfo.publicSourceDir = g.apk.getAbsolutePath();
                d = pi.applicationInfo.loadIcon(pm);
            }
        } catch (PackageManager.NameNotFoundException e) {
            return null;
        }
        return render(d, ICON_PX);
    }

    private Bitmap decodeBackdrop(Game g) throws Exception {
        ApkScan.Image best = ArtPicker.pick(ApkScan.scan(g.apk).images);
        if (best == null) return decodeIcon(g);  // the caller blurs it into a backdrop
        int maxSide = Math.max(ctx.getResources().getDisplayMetrics().widthPixels,
                ctx.getResources().getDisplayMetrics().heightPixels);
        int sample = 1;
        while (Math.max(best.width, best.height) / (sample * 2) >= maxSide) sample *= 2;
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inSampleSize = sample;
        try (ZipFile z = new ZipFile(g.apk)) {
            ZipEntry e = z.getEntry(best.path);
            if (e == null) return decodeIcon(g);
            try (InputStream in = z.getInputStream(e)) {
                Bitmap b = BitmapFactory.decodeStream(in, null, o);
                return b != null ? b : decodeIcon(g);
            }
        }
    }

    private static Bitmap render(Drawable d, int px) {
        if (d instanceof BitmapDrawable && ((BitmapDrawable) d).getBitmap() != null) {
            Bitmap src = ((BitmapDrawable) d).getBitmap();
            if (src.getWidth() <= px && src.getHeight() <= px) return src;
            return Bitmap.createScaledBitmap(src, px, px, true);
        }
        Bitmap b = Bitmap.createBitmap(px, px, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(b);
        d.setBounds(0, 0, px, px);
        d.draw(c);
        return b;
    }

    private void save(File f, Bitmap b) {
        if (!dir.isDirectory() && !dir.mkdirs()) return;
        try (OutputStream o = new FileOutputStream(f)) {
            b.compress(Bitmap.CompressFormat.PNG, 100, o);
        } catch (Exception e) {
            Log.w(TAG, "art: could not cache " + f + ": " + e);
        }
    }
}
