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

        // M6 keys: written only when not at their default (so older settings text is unchanged), and validated.
        GameSettings m = new GameSettings();
        m.patches = "skip-intro,no-ads";
        m.mods = "hd-textures";
        m.scale = 75;
        m.aspect = "fit169";
        String f = m.format();
        check(f.contains("\npatches=skip-intro,no-ads\n") && f.contains("\nmods=hd-textures\n") && f.contains("\nscale=75\n")
                && f.contains("\naspect=fit169\n"), "M6 keys written: " + f);
        GameSettings back = GameSettings.parse(f);
        check(back.patches.equals("skip-intro,no-ads") && back.mods.equals("hd-textures") && back.scale == 75
                && back.aspect.equals("fit169"), "M6 keys round trip");
        check(!new GameSettings().format().contains("patches") && !new GameSettings().format().contains("scale"),
                "defaults are not written");
        check(GameSettings.parse("scale=33").scale == 100 && GameSettings.parse("aspect=wide").aspect.equals("stretch"),
                "unknown scale/aspect values read as the defaults");
        check(GameSettings.parse("patches=ok-id,../evil,Bad Id").patches.equals("ok-id"), "only well-formed ids are kept");
        check(m.hasPatch("no-ads") && !m.hasPatch("no-ad"), "hasPatch is exact");
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
