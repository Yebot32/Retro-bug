// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The library's decisions, kept free of Android so the host tests pin them: which tiles stay "Getting ready" across a
// rescan, which targetSdk a converted copy is really made for (the repackager's own rule), where an APK's 32-bit code
// sits, and how far a backdrop is sampled down before it is decoded.
package com.retrobug.launcher;

import java.util.List;
import java.util.Set;

final class LibraryRules {
    private LibraryRules() {}

    enum Kind { GAME, SPLIT, NOT_32_BIT }

    /** A backdrop is only shown blurred at ~320 px: decode it to at most this on its longer side. */
    static final int BACKDROP_MAX = 640;

    /** A rescan's games keep CONVERTING while their conversion runs; every other game takes the rescan's state. */
    static void keepConverting(List<Game> fresh, Set<String> converting) {
        for (Game g : fresh) if (converting.contains(g.key)) g.state = Game.State.CONVERTING;
    }

    /**
     * The targetSdk a copy is made for: at least the install floor, the copy's minSdk and the player's setting, and never
     * below the original's (an absent targetSdk means minSdk). {@code setting} 0 means none.
     */
    static int outputTarget(Integer oldTarget, Integer oldMin, int setting) {
        int newMin = oldMin != null && oldMin >= ApkRepackager.MIN_SDK_FLOOR ? oldMin : ApkRepackager.MIN_SDK_FLOOR;
        int floor = Math.max(Math.max(ApkRepackager.INSTALL_FLOOR_SDK, newMin), setting);
        if (oldTarget == null && oldMin != null && oldMin >= floor) return oldMin;
        if (oldTarget != null && oldTarget >= floor) return oldTarget;
        return floor;
    }

    /** Where the 32-bit ARM code is: in the base APK, only in a split, or nowhere a 64-bit phone would not run natively. */
    static Kind classify(Set<String> baseAbis, Set<String> splitAbis) {
        if (baseAbis.contains("arm64-v8a") || splitAbis.contains("arm64-v8a")) return Kind.NOT_32_BIT;
        if (ApkScan.is32BitOnly(baseAbis)) return Kind.GAME;
        if (ApkScan.is32BitOnly(splitAbis)) return Kind.SPLIT;
        return Kind.NOT_32_BIT;
    }

    /** The power-of-two inSampleSize that brings the longer side to at most BACKDROP_MAX; 1 for an unknown size. */
    static int backdropSample(int w, int h) {
        int longer = Math.max(w, h), s = 1;
        while (longer / s > BACKDROP_MAX) s *= 2;
        return s;
    }
}
