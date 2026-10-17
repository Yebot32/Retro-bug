// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// One game in the library: an installed original, an APK the player added, or both (the same package). Its state
// says what a tap does — convert it, open its converted copy, or bring the copy up to date.
package com.retrobug.launcher;

import java.io.File;

final class Game {
    enum State { NOT_CONVERTED, CONVERTING, READY, UPDATE, UNREADABLE }

    /** The library key: the original's package name (or the file name for an APK whose manifest could not be read). */
    final String key;
    final String packageName;
    final String label;
    final long versionCode;
    /** The APK a conversion reads: the installed original's base.apk, else the added file. */
    final File apk;
    final boolean installed;
    final boolean added;
    volatile State state;

    Game(String key, String packageName, String label, long versionCode, File apk, boolean installed, boolean added,
         State state) {
        this.key = key;
        this.packageName = packageName;
        this.label = label;
        this.versionCode = versionCode;
        this.apk = apk;
        this.installed = installed;
        this.added = added;
        this.state = state;
    }

    /** The converted copy's package. */
    String copyPackage() { return packageName + ApkRepackager.RENAME_SUFFIX; }
}
