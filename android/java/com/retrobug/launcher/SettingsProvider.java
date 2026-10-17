// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// How a converted game reads the settings the player chose for it here (GameSettings). The game's shim asks for
// content://com.retrobug.launcher.settings/<original package> when it starts. The answer goes only to the copy of that
// very game (`<package>.rb`) and only when it is signed with this device's repack key — the key RepackSigner signs
// every copy with — so no other app can read anything through it. Any other caller gets an empty answer.
package com.retrobug.launcher;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Build;
import android.util.Log;

import java.security.KeyStore;
import java.security.cert.Certificate;
import java.util.Arrays;

public final class SettingsProvider extends ContentProvider {
    private static final String TAG = "RetroBug";

    @Override public boolean onCreate() { return true; }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        MatrixCursor out = new MatrixCursor(new String[] {"settings"});
        String pkg = uri.getLastPathSegment();
        String caller = getCallingPackage();
        if (pkg == null || caller == null || !caller.equals(pkg + ApkRepackager.RENAME_SUFFIX)) return out;
        if (!signedWithRepackKey(caller)) {
            Log.w(TAG, "settings: " + caller + " is not signed with this device's repack key — not answered");
            return out;
        }
        out.addRow(new Object[] {new GameSettingsStore(getContext()).get(pkg).format()});
        return out;
    }

    @SuppressWarnings("deprecation")
    private boolean signedWithRepackKey(String caller) {
        try {
            KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
            ks.load(null);
            Certificate cert = ks.getCertificate(RepackSigner.ALIAS);
            if (cert == null) return false;
            byte[] ours = cert.getEncoded();
            PackageManager pm = getContext().getPackageManager();
            Signature[] sigs;
            if (Build.VERSION.SDK_INT >= 28) {
                PackageInfo pi = pm.getPackageInfo(caller, PackageManager.GET_SIGNING_CERTIFICATES);
                sigs = pi.signingInfo == null ? null : pi.signingInfo.getApkContentsSigners();
            } else {
                sigs = pm.getPackageInfo(caller, PackageManager.GET_SIGNATURES).signatures;
            }
            if (sigs == null || sigs.length != 1) return false;
            return Arrays.equals(sigs[0].toByteArray(), ours);
        } catch (Exception e) {
            Log.w(TAG, "settings: could not check " + caller + ": " + e);
            return false;
        }
    }

    @Override public String getType(Uri uri) { return null; }
    @Override public Uri insert(Uri uri, ContentValues v) { return null; }
    @Override public int delete(Uri uri, String s, String[] a) { return 0; }
    @Override public int update(Uri uri, ContentValues v, String s, String[] a) { return 0; }
}
