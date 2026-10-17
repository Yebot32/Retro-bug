// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Chooses a game's backdrop from the images inside its APK (ApkScan's list): large enough for a screen, preferring
// names a game gives its own splash, loading and title art, and landscape shapes. Null when nothing qualifies; the
// caller then uses the icon.
package com.retrobug.launcher;

import java.util.List;

final class ArtPicker {
    private ArtPicker() {}

    private static final String[] KEYWORDS = {"splash", "loading", "title", "background", "bg", "cover", "banner", "menu"};

    static ApkScan.Image pick(List<ApkScan.Image> images) {
        ApkScan.Image best = null;
        long bestScore = Long.MIN_VALUE;
        for (ApkScan.Image i : images) {
            if (!qualifies(i)) continue;
            long s = score(i);
            if (best == null || s > bestScore || (s == bestScore && better(i.path, best.path))) {
                best = i;
                bestScore = s;
            }
        }
        return best;
    }

    static boolean qualifies(ApkScan.Image i) {
        String p = i.path.toLowerCase();
        if (p.startsWith("res/mipmap") || p.contains("ic_launcher")) return false;
        return (i.width >= 800 && i.height >= 480) || (i.height >= 800 && i.width >= 480);
    }

    static long score(ApkScan.Image i) {
        long s = (long) i.width * i.height / 10000;
        String p = i.path.toLowerCase();
        for (String k : KEYWORDS) {
            if (p.contains(k)) { s += 300; break; }
        }
        if (i.width > i.height) s += 100;
        return s;
    }

    /** The tie-break: the shorter path, then the lexicographically first. */
    private static boolean better(String a, String b) {
        return a.length() != b.length() ? a.length() < b.length() : a.compareTo(b) < 0;
    }
}
