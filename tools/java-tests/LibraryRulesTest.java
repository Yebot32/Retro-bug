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
        List<Game> fresh = new ArrayList<>(Arrays.asList(game("a", Game.State.READY), game("b", Game.State.NOT_CONVERTED)));
        LibraryRules.keepConverting(fresh, new HashSet<>(Collections.singleton("b")));
        check(fresh.get(0).state == Game.State.READY && fresh.get(1).state == Game.State.CONVERTING, "b is mid-conversion, a is not");
        List<Game> after = new ArrayList<>(Collections.singletonList(game("b", Game.State.READY)));
        LibraryRules.keepConverting(after, Collections.<String>emptySet());
        check(after.get(0).state == Game.State.READY, "after the conversion ends, the rescan's state stands");
        check(LibraryRules.needsRuntimeUpdate(0), "a legacy copy without a runtime stamp is updated");
        check(LibraryRules.needsRuntimeUpdate(ApkRepackager.RUNTIME_STAMP - 1), "an older runtime is updated");
        check(!LibraryRules.needsRuntimeUpdate(ApkRepackager.RUNTIME_STAMP), "the same runtime opens directly");
        check(!LibraryRules.needsRuntimeUpdate(ApkRepackager.RUNTIME_STAMP + 1), "a newer copy is never downgraded");
        check(LibraryRules.legacyPushTarget("com.pikpok.oreo", 35) == 30, "Oreo keeps legacy PendingIntents");
        check(LibraryRules.legacyPushTarget("com.rovio.angrybirdsgo", 24) == 24, "a lower explicit target stands");
        check(LibraryRules.legacyPushTarget("com.rovio.angrybirdsgo", 34) == 30,
                "Angry Birds Go is capped even when an earlier repack raised its manifest target");
        check(LibraryRules.legacyPushTarget("com.rovio.angrybirdsgo.rb", 34) == 30,
                "Angry Birds Go's already-renamed package keeps the legacy cap");
        check(LibraryRules.legacyPushTarget("com.turbochilli.rollingsky", 33) == 30,
                "Rolling Sky's CM push service keeps legacy PendingIntents");
        check(LibraryRules.legacyPushTarget("com.turbochilli.rollingsky.rb", 35) == 30,
                "Rolling Sky's already-renamed package keeps the legacy cap");
        check(LibraryRules.legacyPushTarget("com.sega.ssasr", 33) == 30,
                "All-Stars Racing can create its legacy PendingIntents on current Android");
        check(LibraryRules.legacyPushTarget("com.example.game", 34) == 34, "other games are unchanged");
        check(LibraryRules.forcedOutputTarget(24, 30) == 30,
                "the legacy PendingIntent cap may lower an original target above 30");

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
