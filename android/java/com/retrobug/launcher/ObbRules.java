// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The name an OBB gets in a converted copy's own OBB folder. A game finds its OBB as getObbDir() +
// "main.<versionCode>.<getPackageName()>.obb" (the APK expansion convention); in the copy both parts carry the new
// package, so the file sits in Android/obb/<pkg>.rb/ named for <pkg>.rb. A game's own naming (its package at the end)
// keeps its shape with the package swapped; a file named any other way becomes the game's main OBB.
package com.retrobug.launcher;

import java.util.Locale;

final class ObbRules {
    private ObbRules() {}

    static boolean isObb(String name) {
        return name != null && name.toLowerCase(Locale.ROOT).endsWith(".obb");
    }

    /** True when `name` is an OBB named for `pkg` (ends ".<pkg>.obb"). */
    static boolean belongsTo(String name, String pkg) {
        return name != null && name.toLowerCase(Locale.ROOT).endsWith(("." + pkg + ".obb").toLowerCase(Locale.ROOT));
    }

    /** The file name in the copy's OBB folder for an OBB of the original game `pkg` (versionCode `vc`). */
    static String copyName(String name, String pkg, long vc) {
        if (belongsTo(name, pkg)) {
            String base = name.substring(0, name.length() - (pkg.length() + ".obb".length()));
            return base + pkg + ApkRepackager.RENAME_SUFFIX + ".obb";
        }
        return "main." + vc + "." + pkg + ApkRepackager.RENAME_SUFFIX + ".obb";
    }
}
