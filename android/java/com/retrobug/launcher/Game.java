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
    /** The APK the player added for this game, when there is one (found by the scan, so the menu never looks). */
    File addedFile;
    /**
     * False for a game with no Java code of its own (hasCode="false"): its copy carries no settings provider, so the
     * per-game CPU and overlay settings cannot reach it.
     */
    boolean hasCode = true;
    /** Why an UNREADABLE game cannot be offered, in the player's words; null for the plain "can't read this file". */
    String why;
    /** The class the launcher starts, as the scan found it: what decides whether the game is a NativeActivity (C31). */
    String launcher;

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

    /** The sentence an UNREADABLE tile and its tap show. */
    String reason() { return why != null ? why : "Can't read this file"; }

    Game unplayable(String why) {
        this.state = State.UNREADABLE;
        this.why = why;
        return this;
    }

    /** The converted copy's package. */
    String copyPackage() { return packageName + ApkRepackager.RENAME_SUFFIX; }
}
