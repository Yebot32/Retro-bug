// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
package com.retrobug.launcher;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Installs a signed APK through a PackageInstaller session and reports what the platform said, verbatim.
 *
 * The installer's own status text is the useful output — "INSTALL_FAILED_UPDATE_INCOMPATIBLE: ... signatures
 * do not match" names the problem better than anything this class could paraphrase — so `EXTRA_STATUS` and
 * `EXTRA_STATUS_MESSAGE` go to the callback unchanged.
 *
 * When RetroBug is not allowed to install apps the session would fail with a bare status; instead the user
 * is told which setting it is and taken to it. Toggling it is theirs to do: it is a security setting.
 */
final class RepackInstaller {
    private RepackInstaller() {}

    private static final String TAG = "RBLAUNCH";
    private static final String ACTION = "com.retrobug.launcher.REPACK_INSTALL_STATUS";

    interface Callback {
        /** One line of progress or the final verdict; `done` is true on the last call. */
        void onStatus(String line, boolean done);
    }

    // API 31 / 33 constants, spelled out so this compiles and runs from minSdk 26.
    private static final int FLAG_MUTABLE = 0x02000000;        // PendingIntent.FLAG_MUTABLE (31)
    private static final int RECEIVER_NOT_EXPORTED = 0x4;       // Context.RECEIVER_NOT_EXPORTED (33)

    static void install(final Context context, File apk, final Callback cb) {
        final Context app = context.getApplicationContext();
        if (!app.getPackageManager().canRequestPackageInstalls()) {
            cb.onStatus("RetroBug is not allowed to install apps. Opening Settings > \"Install unknown apps\""
                    + " for RetroBug: turn its switch on (\"Allow from this source\" / \"Allow permission\"),"
                    + " come back and tap Make installable again.", true);
            Intent settings = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:" + app.getPackageName()));
            settings.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            try {
                context.startActivity(settings);
            } catch (RuntimeException e) {
                cb.onStatus("could not open the setting: " + e, true);
            }
            return;
        }

        PackageInstaller pi = app.getPackageManager().getPackageInstaller();
        final int sessionId;
        PackageInstaller.Session session = null;
        try {
            PackageInstaller.SessionParams params =
                    new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setSize(apk.length());
            sessionId = pi.createSession(params);
            session = pi.openSession(sessionId);
            try (InputStream in = new FileInputStream(apk);
                 OutputStream out = session.openWrite("base.apk", 0, apk.length())) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                session.fsync(out);
            }
        } catch (IOException | RuntimeException e) {
            if (session != null) session.abandon();
            cb.onStatus("install: could not write the session: " + e, true);
            return;
        }

        final String action = ACTION + "." + sessionId;
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent intent) {
                int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, Integer.MIN_VALUE);
                String message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    cb.onStatus("install: status=" + status + " (PENDING_USER_ACTION) — confirm on screen", false);
                    Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
                    if (confirm != null) {
                        confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                        try {
                            app.startActivity(confirm);
                        } catch (RuntimeException e) {
                            cb.onStatus("install: could not show the confirmation: " + e, false);
                        }
                    }
                    return;
                }
                try {
                    app.unregisterReceiver(this);
                } catch (IllegalArgumentException ignored) {
                    // Already gone.
                }
                cb.onStatus("install: status=" + status
                        + (status == PackageInstaller.STATUS_SUCCESS ? " (SUCCESS)" : "")
                        + " message=" + message, true);
            }
        };
        IntentFilter filter = new IntentFilter(action);
        if (Build.VERSION.SDK_INT >= 33) {
            app.registerReceiver(receiver, filter, RECEIVER_NOT_EXPORTED);
        } else {
            app.registerReceiver(receiver, filter);
        }
        Intent statusIntent = new Intent(action).setPackage(app.getPackageName());
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 31) flags |= FLAG_MUTABLE;   // the installer fills in the status extras
        PendingIntent pending = PendingIntent.getBroadcast(app, sessionId, statusIntent, flags);
        try {
            Log.i(TAG, "install: committing session " + sessionId + " for " + apk.getName());
            session.commit(pending.getIntentSender());
            cb.onStatus("install: session " + sessionId + " committed", false);
        } catch (RuntimeException e) {
            app.unregisterReceiver(receiver);
            session.abandon();
            cb.onStatus("install: commit failed: " + e, true);
        } finally {
            session.close();
        }
    }
}
