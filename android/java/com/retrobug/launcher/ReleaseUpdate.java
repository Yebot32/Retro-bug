// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
package com.retrobug.launcher;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.DialogInterface;
import android.content.Intent;
import android.net.Uri;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.URL;
import java.nio.charset.StandardCharsets;

import javax.net.ssl.HttpsURLConnection;

final class ReleaseUpdate {
    private static final String ENDPOINT = "https://api.github.com/repos/Yebot32/Retro-bug/releases/latest";
    private static final String DOWNLOAD_PATH = "/Yebot32/Retro-bug/releases/download/";

    interface Visibility { boolean canShow(); }

    static void check(final Activity activity, final Visibility visibility) {
        new Thread(new Runnable() {
            @Override public void run() { checkInBackground(activity, visibility); }
        }, "retrobug-release-check").start();
    }

    private static void checkInBackground(final Activity activity, final Visibility visibility) {
        try {
            String installed = activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0).versionName;
            JSONObject release = latest();
            if (release == null || release.optBoolean("draft", true) || release.optBoolean("prerelease", true)) return;
            final String tag = release.getString("tag_name");
            if (!isNewer(installed, tag)) return;
            final Uri download = apk(release.optJSONArray("assets"), tag);
            if (download == null) return;
            final String notes = release.isNull("body") ? "" : release.optString("body");
            activity.runOnUiThread(new Runnable() {
                @Override public void run() {
                    if (activity.isFinishing() || activity.isDestroyed() || !visibility.canShow()) return;
                    show(activity, tag, notes, download);
                }
            });
        } catch (Exception ignored) {
            // A network or release-format error leaves the library available.
        }
    }

    private static void show(final Activity activity, String tag, String notes, final Uri download) {
        new AlertDialog.Builder(activity)
                .setTitle("RetroBug " + tag)
                .setMessage(notes)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Update", new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface dialog, int which) {
                        try {
                            activity.startActivity(new Intent(Intent.ACTION_VIEW, download));
                        } catch (ActivityNotFoundException e) {
                            Toast.makeText(activity, "No browser available to download RetroBug.",
                                    Toast.LENGTH_LONG).show();
                        }
                    }
                }).show();
    }

    private static JSONObject latest() throws Exception {
        HttpsURLConnection connection = (HttpsURLConnection) new URL(ENDPOINT).openConnection();
        connection.setConnectTimeout(8000);
        connection.setReadTimeout(8000);
        connection.setInstanceFollowRedirects(false);
        connection.setRequestProperty("Accept", "application/vnd.github+json");
        try {
            if (connection.getResponseCode() != 200) return null;
            try (InputStream input = connection.getInputStream(); ByteArrayOutputStream body = new ByteArrayOutputStream()) {
                byte[] buffer = new byte[8192];
                int size;
                while ((size = input.read(buffer)) != -1) {
                    if (body.size() + size > 1_000_000) return null;
                    body.write(buffer, 0, size);
                }
                return new JSONObject(new String(body.toByteArray(), StandardCharsets.UTF_8));
            }
        } finally {
            connection.disconnect();
        }
    }

    static boolean isNewer(String installed, String available) {
        long[] local = version(installed);
        long[] remote = version(available);
        if (local == null || remote == null) return false;
        for (int i = 0; i < local.length; i++) {
            if (remote[i] != local[i]) return remote[i] > local[i];
        }
        return false;
    }

    private static long[] version(String tag) {
        if (tag == null || !tag.matches("v?[0-9]+\\.[0-9]+\\.[0-9]+")) return null;
        String[] pieces = (tag.startsWith("v") ? tag.substring(1) : tag).split("\\.");
        try {
            return new long[] {Long.parseLong(pieces[0]), Long.parseLong(pieces[1]), Long.parseLong(pieces[2])};
        } catch (NumberFormatException e) {
            return null;
        }
    }

    private static Uri apk(JSONArray assets, String tag) {
        if (assets == null) return null;
        for (int i = 0; i < assets.length(); i++) {
            JSONObject asset = assets.optJSONObject(i);
            if (asset == null || !"uploaded".equals(asset.optString("state"))) continue;
            String name = asset.optString("name");
            String lower = name.toLowerCase(java.util.Locale.ROOT);
            if (!(lower.startsWith("retrobug") || lower.equals("rbguest-launcher.apk")) || !lower.endsWith(".apk")) {
                continue;
            }
            Uri url = Uri.parse(asset.optString("browser_download_url"));
            if ("https".equals(url.getScheme()) && "github.com".equals(url.getHost())
                    && (DOWNLOAD_PATH + tag + "/" + name).equals(url.getPath())) return url;
        }
        return null;
    }

    private ReleaseUpdate() {}
}
