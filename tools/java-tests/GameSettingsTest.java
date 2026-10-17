// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for GameSettings, the per-game record the launcher stores and converted games read at start: it round
// trips, and anything missing, unknown or out of range falls back to the default rather than reaching the runtime.
package com.retrobug.launcher;

public class GameSettingsTest {
    static int checks = 0, failures = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    public static void main(String[] a) {
        GameSettings d = GameSettings.parse("");
        check("fast".equals(d.cpu) && !d.overlay && d.target == 35, "defaults: fast, no overlay, target 35");
        GameSettings s = new GameSettings();
        s.cpu = "safe";
        s.overlay = true;
        s.target = 28;
        check("cpu=safe\noverlay=1\ntarget=28\n".equals(s.format()), "format: " + s.format());
        GameSettings r = GameSettings.parse(s.format());
        check("safe".equals(r.cpu) && r.overlay && r.target == 28, "round trip");
        check("fast".equals(GameSettings.parse("cpu=turbo").cpu), "an unknown cpu mode is the default");
        check(GameSettings.parse("target=99").target == 35, "a target past 36 is the default");
        check(GameSettings.parse("target=23").target == 35, "a target below 24 is the default");
        check(GameSettings.parse("target=abc").target == 35, "a non-number target is the default");
        check(GameSettings.parse("overlay=yes").overlay == false, "overlay is 1 or nothing");
        GameSettings u = GameSettings.parse("foo=1\ncpu=safe\n");
        check("safe".equals(u.cpu), "unknown keys are ignored, known ones still read");
        check(GameSettings.parse(null).target == 35, "null text is the defaults");
        System.out.println("GameSettingsTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
