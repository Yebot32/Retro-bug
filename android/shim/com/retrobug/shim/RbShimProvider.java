// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Entry point of the targetSdk compatibility shims in a repackaged game. The repackager adds this provider to
// the game's manifest (not exported, highest initOrder) because the platform creates providers on the main
// thread after the Application object is attached but before Application.onCreate, any activity or any
// service — early enough for every shim, with no change to the game's own classes. It reads
// assets/rb32/rbshim.properties (written by com.retrobug.launcher.ShimPlan) and runs the listed shims. It
// serves no data.
package com.retrobug.shim;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.util.Log;

import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Properties;

public final class RbShimProvider extends ContentProvider {
    static final String CONFIG = "rb32/rbshim.properties";

    @Override public boolean onCreate() {
        Properties p = new Properties();
        try (InputStream in = getContext().getAssets().open(CONFIG)) {
            p.load(in);
            int target = Integer.parseInt(p.getProperty("targetSdk", "").trim());
            List<String> ids = new ArrayList<>();
            for (String s : Arrays.asList(p.getProperty("shims", "").split(","))) if (!s.trim().isEmpty()) ids.add(s.trim());
            Log.i(Shims.TAG, "shims: " + ids + " for original targetSdk " + target + " (" + getContext().getPackageName() + ")");
            Shims.apply(getContext(), target, ids);
        } catch (Throwable t) {
            Log.i(Shims.TAG, "shims FAILED: could not read assets/" + CONFIG + ": " + t);
        }
        return true;
    }

    @Override public Cursor query(Uri u, String[] proj, String sel, String[] args, String order) { return null; }
    @Override public String getType(Uri u) { return null; }
    @Override public Uri insert(Uri u, ContentValues v) { return null; }
    @Override public int delete(Uri u, String sel, String[] args) { return 0; }
    @Override public int update(Uri u, ContentValues v, String sel, String[] args) { return 0; }
}
