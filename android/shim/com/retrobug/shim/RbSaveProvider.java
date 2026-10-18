// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The converted game's save export and import, asked for by the RetroBug launcher's Data tab. Exported because the
// launcher is another app; every call and file open first checks the caller is the RetroBug launcher signed with the
// certificate recorded in this copy when it was made (assets/rb32/rbshim.properties, launcherCert) — anyone else gets
// a SecurityException and nothing is read or written. It runs in the game's own process, the only one that can read
// the game's private files.
//
// Export: call("export") zips the saves (SaveArchive) to cache/rbsave-export.zip; the launcher then reads it through
// openFile("export", "r"). Import: the launcher writes the zip through openFile("import", "w"), then call("import")
// checks and unpacks it, keeps the current saves as no_backup/rbsave-undo.zip, replaces them, and ends this process
// so the game starts from the imported saves next time. call("undo") puts the kept saves back the same way.
//
// OBB: the game reads its OBB as itself, so the file must be this app's — one the launcher wrote would be the
// launcher's, and Android refuses the game (EACCES). call("obbPrepare") creates getObbDir() as this app; the launcher
// writes through openFile("obb/<name>.part", "w") (a file this app opens, so this app owns it) and call("obbCommit",
// name) renames it into place.
package com.retrobug.shim;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;

public final class RbSaveProvider extends ContentProvider {
    static final String EXPORT = "rbsave-export.zip", IMPORT = "rbsave-import.zip", UNDO = "rbsave-undo.zip";

    @Override public boolean onCreate() { return true; }

    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        check();
        Context c = getContext();
        Bundle out = new Bundle();
        try {
            switch (method) {
                case "export": {
                    File f = new File(c.getCacheDir(), EXPORT);
                    int n;
                    try (OutputStream o = new FileOutputStream(f)) { n = SaveArchive.write(original(), dirs(), o); }
                    out.putBoolean("ok", true);
                    out.putInt("files", n);
                    out.putLong("bytes", f.length());
                    Log.i(Shims.TAG, "saves: exported " + n + " file(s), " + f.length() + " bytes");
                    return out;
                }
                case "import":
                    return restore(new File(c.getCacheDir(), IMPORT), true, out);
                case "undo":
                    return restore(new File(c.getNoBackupFilesDir(), UNDO), false, out);
                case "obbPrepare": {
                    // Created by this app, the folder is this app's: a file the launcher made there would not be readable.
                    File dir = c.getObbDir();
                    out.putBoolean("ok", dir != null && (dir.isDirectory() || dir.mkdirs()));
                    if (dir != null) out.putString("dir", dir.getAbsolutePath());
                    return out;
                }
                case "obbCommit": {
                    File dir = c.getObbDir();
                    if (dir == null || !SaveArchive.obbName(arg)) throw new IllegalArgumentException("not an OBB name: " + arg);
                    File part = new File(dir, arg + ".part"), to = new File(dir, arg);
                    boolean ok = part.isFile() && (!to.exists() || to.delete()) && part.renameTo(to);
                    out.putBoolean("ok", ok);
                    if (ok) Log.i(Shims.TAG, "obb: " + to + " (" + to.length() + " bytes) from the launcher");
                    return out;
                }
                case "status":
                    out.putBoolean("ok", true);
                    out.putBoolean("undo", new File(c.getNoBackupFilesDir(), UNDO).isFile());
                    return out;
                default:
                    out.putBoolean("ok", false);
                    out.putString("message", "unknown request " + method);
                    return out;
            }
        } catch (Throwable t) {
            Log.i(Shims.TAG, "saves: " + method + " failed: " + t);
            out.putBoolean("ok", false);
            out.putString("message", String.valueOf(t.getMessage()));
            return out;
        }
    }

    /** Unpacks `zip` and makes it the game's saves; with `keepUndo`, the current saves are kept first. */
    private Bundle restore(File zip, boolean keepUndo, Bundle out) throws Exception {
        Context c = getContext();
        if (!zip.isFile()) {
            out.putBoolean("ok", false);
            out.putString("message", keepUndo ? "No save was sent" : "There is nothing to undo");
            return out;
        }
        File staging = new File(c.getCacheDir(), "rbsave-staging");
        SaveArchive.delete(staging);
        String why;
        try (InputStream in = new FileInputStream(zip)) { why = SaveArchive.unpack(in, staging, original()); }
        if (why != null) {
            out.putBoolean("ok", false);
            out.putString("message", why);
            return out;
        }
        if (keepUndo) {
            try (OutputStream o = new FileOutputStream(new File(c.getNoBackupFilesDir(), UNDO))) {
                SaveArchive.write(original(), dirs(), o);
            }
        }
        SaveArchive.replace(dirs(), staging);
        SaveArchive.delete(staging);
        if (keepUndo) zip.delete();
        else if (!zip.delete()) Log.i(Shims.TAG, "saves: could not remove " + zip);
        Log.i(Shims.TAG, "saves: " + (keepUndo ? "imported" : "restored the saves kept before the import")
                + "; ending this process so the game starts from them");
        out.putBoolean("ok", true);
        // The game's own in-memory state (open preferences, a running level) would write the old saves back: this
        // process ends once the answer is on its way, and the next start reads the new files.
        new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
            @Override public void run() { Process.killProcess(Process.myPid()); }
        }, 300);
        return out;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        check();
        String what = uri.getLastPathSegment();
        File cache = getContext().getCacheDir();
        if ("export".equals(what) && "r".equals(mode)) {
            return ParcelFileDescriptor.open(new File(cache, EXPORT), ParcelFileDescriptor.MODE_READ_ONLY);
        }
        if ("import".equals(what) && mode.startsWith("w")) {
            return ParcelFileDescriptor.open(new File(cache, IMPORT), ParcelFileDescriptor.MODE_WRITE_ONLY
                    | ParcelFileDescriptor.MODE_CREATE | ParcelFileDescriptor.MODE_TRUNCATE);
        }
        java.util.List<String> seg = uri.getPathSegments();
        if (seg.size() == 2 && "obb".equals(seg.get(0)) && seg.get(1).endsWith(".part") && mode.startsWith("w")) {
            String name = seg.get(1).substring(0, seg.get(1).length() - ".part".length());
            File dir = getContext().getObbDir();
            if (dir != null && SaveArchive.obbName(name)) {
                return ParcelFileDescriptor.open(new File(dir, seg.get(1)), ParcelFileDescriptor.MODE_WRITE_ONLY
                        | ParcelFileDescriptor.MODE_CREATE | ParcelFileDescriptor.MODE_TRUNCATE);
            }
        }
        throw new FileNotFoundException(uri + " (" + mode + ")");
    }

    /** Throws unless the caller is the RetroBug launcher with the recorded certificate. */
    @SuppressWarnings("deprecation")
    private void check() {
        String caller = getCallingPackage();
        List<byte[]> signers = new ArrayList<>();
        try {
            PackageManager pm = getContext().getPackageManager();
            Signature[] sigs;
            if (android.os.Build.VERSION.SDK_INT >= 28) {
                PackageInfo pi = pm.getPackageInfo(caller, PackageManager.GET_SIGNING_CERTIFICATES);
                sigs = pi.signingInfo == null ? null : pi.signingInfo.getApkContentsSigners();
            } else {
                sigs = pm.getPackageInfo(caller, PackageManager.GET_SIGNATURES).signatures;
            }
            if (sigs != null) for (Signature s : sigs) signers.add(s.toByteArray());
        } catch (Exception e) {
            // no package, no signers: refused below
        }
        if (!SaveArchive.trusted(caller, signers, launcherCert())) {
            Log.i(Shims.TAG, "saves: refused " + caller);
            throw new SecurityException("saves are only for the RetroBug launcher");
        }
    }

    private String launcherCert() {
        Properties p = new Properties();
        try (InputStream in = getContext().getAssets().open(RbShimProvider.CONFIG)) {
            p.load(in);
        } catch (Exception e) {
            return null;
        }
        return p.getProperty("launcherCert");
    }

    private String original() {
        String pkg = getContext().getPackageName();
        return pkg.endsWith(".rb") ? pkg.substring(0, pkg.length() - 3) : pkg;
    }

    private Map<String, File> dirs() {
        Context c = getContext();
        Map<String, File> m = new LinkedHashMap<>();
        m.put("files", c.getFilesDir());
        m.put("shared_prefs", new File(c.getDataDir(), "shared_prefs"));
        m.put("databases", new File(c.getDataDir(), "databases"));
        File ext = c.getExternalFilesDir(null);
        if (ext != null) m.put("external", ext);
        return m;
    }

    @Override public Cursor query(Uri u, String[] proj, String sel, String[] args, String order) { return null; }
    @Override public String getType(Uri u) { return null; }
    @Override public Uri insert(Uri u, ContentValues v) { return null; }
    @Override public int delete(Uri u, String sel, String[] args) { return 0; }
    @Override public int update(Uri u, ContentValues v, String sel, String[] args) { return 0; }
}
