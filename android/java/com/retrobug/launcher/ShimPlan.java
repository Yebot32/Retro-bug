// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Decides, at repackage time, which targetSdk compatibility shims a repackaged game gets, and names the risky
// platform calls its DEX makes.
//
// Why this exists: the repackager raises targetSdk to the install floor (24), and that switches on every
// behaviour change the platform gated on targetSdk between the game's own target and 24. The old Java was
// never written for them. Gumball Lite (no targetSdk, minSdk 8, so effectively 8) does an implicit
// `bindService` for Market billing on its first run, which throws "Service Intent must be explicit" at
// targetSdk >= 21 and kills its activity in onCreate.
//
// The shims run inside the game's own process, from `com.retrobug.shim.RbShimProvider` (android/shim): a
// ContentProvider added to the manifest, because providers are created before Application.onCreate and before
// the first activity or service, and need no change to the game's own classes. Its classes travel as an extra
// classesN.dex. The decisions are made here, on the PC-testable side, and written to `assets/rb32/rbshim.properties`;
// the provider only executes the list it is given.
//
// The shims (each starts where the platform's own gate starts, so a game already targeting past it gets none):
//   target-sdk      the process's own ApplicationInfo.targetSdkVersion is set back to the game's original target.
//                   The platform still carries the old-target code paths (an app installed years ago runs on
//                   them), and the in-process checks read exactly that field: implicit service Intents (21),
//                   MODE_WORLD_READABLE/WRITEABLE (24), @JavascriptInterface (17), the legacy menu key (14),
//                   View/layout compat. Server-side checks (runtime permissions, background limits) keep 24.
//   strict-network  NetworkOnMainThreadException (targets >= 11): StrictMode's main-thread network detection is
//                   switched off again. It is set at bind time, before any provider runs, so target-sdk alone
//                   does not undo it.
//   file-uri        FileUriExposedException (targets >= 24, i.e. every repackaged game below 24): same reason,
//                   VM policy set at bind time.
//   permissions     runtime permissions (targets >= 23): the game's declared dangerous permissions were granted
//                   at install on its original target; now nothing grants them. The provider asks for the
//                   missing ones when the first activity is created.
//
// Not shimmed, named in the report instead: APIs the platform removed outright (NoSuchMethodError whatever the
// target). Not needed at 24: background-service limits (26), package visibility (30), Apache HTTP (the platform
// keeps org.apache.http.legacy on the classpath below 28).
//
// Pure Java, no android.* import: tools/run_java_tests.sh runs it on the PC.
package com.retrobug.launcher;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;

final class ShimPlan {
    /** runtimeFiles key of the shim DEX (the launcher ships it as assets/rbshim.dex). */
    static final String SHIM_DEX_KEY = "shimdex";
    static final String CONFIG_ENTRY = "assets/rb32/rbshim.properties";
    static final String PROVIDER_CLASS = "com.retrobug.shim.RbShimProvider";
    static final String AUTHORITY_SUFFIX = ".rbshim";
    /** The launcher's SettingsProvider: where a converted copy reads its per-game settings at start. */
    static final String SETTINGS_AUTHORITY = "com.retrobug.launcher.settings";
    /**
     * The copy's save export/import (the launcher's Data tab): exported, because the launcher asks it, and it answers
     * only the launcher signed with the certificate recorded in the config as launcherCert.
     */
    static final String SAVES_PROVIDER_CLASS = "com.retrobug.shim.RbSaveProvider";
    static final String SAVES_AUTHORITY_SUFFIX = ".rbsaves";
    /** runtimeFiles key of the launcher's signing certificate's SHA-256, lower-case hex (ConvertJob reads it). */
    static final String LAUNCHER_CERT_KEY = "launchercert";

    static final String TARGET_SDK = "target-sdk";
    static final String STRICT_NETWORK = "strict-network";
    static final String FILE_URI = "file-uri";
    static final String PERMISSIONS = "permissions";

    final int originalTarget;
    final int newTarget;
    final List<String> shims;

    private ShimPlan(int originalTarget, int newTarget, List<String> shims) {
        this.originalTarget = originalTarget;
        this.newTarget = newTarget;
        this.shims = Collections.unmodifiableList(shims);
    }

    /** The target the platform applied to the original APK: targetSdk, else minSdk, else 1. */
    static int effectiveTarget(Integer targetSdk, Integer minSdk) {
        if (targetSdk != null) return targetSdk;
        return minSdk != null ? minSdk : 1;
    }

    static ShimPlan of(int originalTarget, int newTarget) {
        List<String> s = new ArrayList<>();
        if (originalTarget < newTarget) {
            s.add(TARGET_SDK);
            if (originalTarget < 11 && newTarget >= 11) s.add(STRICT_NETWORK);
            if (originalTarget < 24 && newTarget >= 24) s.add(FILE_URI);
            if (originalTarget < 23 && newTarget >= 23) s.add(PERMISSIONS);
        }
        return new ShimPlan(originalTarget, newTarget, s);
    }

    /** The provider's input. One key per line, so the runtime side needs nothing but java.util.Properties. */
    String config() {
        return "targetSdk=" + originalTarget + "\nshims=" + String.join(",", shims) + "\n";
    }

    // ---------------------------------------------------------------- risky calls

    private static final class Risk {
        final String[] refs;    // "Lpkg/Class;->name"; "*->name" matches any class
        final int since;        // the platform's gate (targetSdk >= since); 0 = whatever the target
        final String what;
        final String shim;      // null: nothing covers it

        Risk(int since, String what, String shim, String... refs) {
            this.refs = refs; this.since = since; this.what = what; this.shim = shim;
        }
    }

    private static final Risk[] RISKS = {
        new Risk(21, "implicit service Intent throws IllegalArgumentException at targetSdk >= 21", TARGET_SDK,
                "*->bindService", "*->startService"),
        new Risk(24, "MODE_WORLD_READABLE/WRITEABLE (if used) throws SecurityException at targetSdk >= 24", TARGET_SDK,
                "*->getSharedPreferences", "*->openFileOutput"),
        new Risk(17, "JavaScript bridge methods need @JavascriptInterface at targetSdk >= 17", TARGET_SDK,
                "Landroid/webkit/WebView;->addJavascriptInterface"),
        new Risk(11, "network on the main thread throws NetworkOnMainThreadException at targetSdk >= 11", STRICT_NETWORK,
                "Ljava/net/URL;->openConnection", "Ljava/net/URL;->openStream", "Ljava/net/Socket;->connect",
                "Lorg/apache/http/impl/client/DefaultHttpClient;-><init>"),
        new Risk(24, "file:// Uri in an Intent throws FileUriExposedException at targetSdk >= 24", FILE_URI,
                "Landroid/net/Uri;->fromFile"),
        new Risk(23, "dangerous permission needs a runtime grant at targetSdk >= 23", PERMISSIONS,
                "Landroid/telephony/TelephonyManager;->getDeviceId",
                "Landroid/telephony/TelephonyManager;->getSubscriberId",
                "Landroid/os/Environment;->getExternalStorageDirectory",
                "Landroid/location/LocationManager;->requestLocationUpdates",
                "Landroid/location/LocationManager;->getLastKnownLocation",
                "Landroid/accounts/AccountManager;->getAccounts",
                "Landroid/hardware/Camera;->open"),
        new Risk(0, "removed from the platform in API 23: NoSuchMethodError", null,
                "Landroid/app/Notification;->setLatestEventInfo"),
    };

    private static boolean matches(String pattern, String ref) {
        if (pattern.startsWith("*->")) return ref.endsWith(pattern.substring(1));
        return ref.equals(pattern);
    }

    /**
     * One "risk:" line per behaviour change the DEX can hit at the new target, naming the references found and
     * whether a shim in {@code added} (the ones actually put in the APK) covers it. Changes the original target
     * was already past are left out.
     */
    List<String> riskReport(Set<String> methodRefs, List<String> added) {
        List<String> out = new ArrayList<>();
        for (Risk k : RISKS) {
            if (k.since != 0 && (originalTarget >= k.since || newTarget < k.since)) continue;
            Set<String> hit = new TreeSet<>();
            for (String ref : methodRefs) for (String p : k.refs) if (matches(p, ref)) hit.add(ref);
            if (hit.isEmpty()) continue;
            String cover = k.shim != null && added.contains(k.shim) ? "covered by shim " + k.shim
                    : k.shim != null ? "NOT covered (shim " + k.shim + " not added)" : "NOT covered (no shim)";
            out.add("risk: " + k.what + " " + hit + " — " + cover);
        }
        return out;
    }
}
