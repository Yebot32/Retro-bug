// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for LibraryRules, the library's decisions that the final review found wrong in their first, inline form:
// which tiles stay "Getting ready" across a rescan, which targetSdk a copy will really be made for, how an APK whose
// 32-bit code sits in a split is classified, and how far a backdrop is sampled down before decoding.
package com.retrobug.launcher;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;

public class LibraryRulesTest {
    static int checks = 0, failures = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static Game game(String key, Game.State s) {
        return new Game(key, key, key, 1, new File(key + ".apk"), true, false, s);
    }

    static Set<String> set(String... s) { return new TreeSet<>(Arrays.asList(s)); }

    public static void main(String[] a) {
        // Review 1: a rescan keeps CONVERTING only for the game actually being converted; once it ends, the fresh state.
        List<Game> fresh = new ArrayList<>(Arrays.asList(game("a", Game.State.READY), game("b", Game.State.UPDATE)));
        LibraryRules.keepConverting(fresh, new HashSet<>(Collections.singleton("b")));
        check(fresh.get(0).state == Game.State.READY && fresh.get(1).state == Game.State.CONVERTING, "b is mid-conversion, a is not");
        List<Game> after = new ArrayList<>(Collections.singletonList(game("b", Game.State.READY)));
        LibraryRules.keepConverting(after, Collections.<String>emptySet());
        check(after.get(0).state == Game.State.READY, "after the conversion ends, the rescan's state stands");

        // Review 2: the target a copy is really made for — never below the original (the repackager never lowers it),
        // never below the install floor, at least the player's setting.
        check(LibraryRules.outputTarget(28, 9, 35) == 35, "an old game raised to the setting");
        check(LibraryRules.outputTarget(30, 21, 24) == 30, "a setting below the original: the original stands");
        check(LibraryRules.outputTarget(null, 26, 24) == 26, "absent target means minSdk; not lowered");
        check(LibraryRules.outputTarget(null, 9, 24) == 24, "absent target, low minSdk: the floor");
        check(LibraryRules.outputTarget(10, 9, 0) == ApkRepackager.INSTALL_FLOOR_SDK, "no setting: the install floor");

        // Review 3: where the 32-bit code is.
        check(LibraryRules.classify(set("armeabi-v7a"), set(), false) == LibraryRules.Kind.GAME, "base has v7a, one APK: a game");
        check(LibraryRules.classify(set("armeabi-v7a"), set(), true) == LibraryRules.Kind.SPLIT,
                "base has v7a but the install has splits: a copy would miss their resources (C7)");
        check(LibraryRules.classify(set("armeabi-v7a"), set("armeabi-v7a"), true) == LibraryRules.Kind.SPLIT,
                "v7a in the base and in a split: still a split install");
        check(LibraryRules.classify(set(), set("armeabi-v7a"), true) == LibraryRules.Kind.SPLIT, "v7a only in a split: a split install");
        check(LibraryRules.classify(set(), set("armeabi-v7a", "arm64-v8a"), true) == LibraryRules.Kind.NOT_32_BIT, "a split with arm64 too: runs natively");
        check(LibraryRules.classify(set("arm64-v8a", "armeabi-v7a"), set(), false) == LibraryRules.Kind.NOT_32_BIT, "base with arm64: runs natively");
        check(LibraryRules.classify(set("arm64-v8a", "armeabi-v7a"), set(), true) == LibraryRules.Kind.NOT_32_BIT, "arm64 base with splits: runs natively");
        check(LibraryRules.classify(set(), set(), false) == LibraryRules.Kind.NOT_32_BIT, "no native code: not ours");

        // Review 7: a backdrop is only ever shown blurred at ~320 px; decode to at most twice that.
        check(LibraryRules.backdropSample(4096, 4096) == 8, "4096² -> sample 8 (512 px)");
        check(LibraryRules.backdropSample(1280, 720) == 2, "1280x720 -> sample 2 (640 px)");
        check(LibraryRules.backdropSample(800, 480) == 2, "800x480 -> sample 2 (400 px)");
        check(LibraryRules.backdropSample(640, 360) == 1, "640x360 -> sample 1");
        check(LibraryRules.backdropSample(0, 0) == 1, "no size: sample 1");

        System.out.println("LibraryRulesTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
