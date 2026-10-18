// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for ObbRules: the name an OBB gets in the converted copy's own OBB folder (Android/obb/<pkg>.rb/), where a
// game that builds the path from getPackageName() looks for it.
package com.retrobug.launcher;

public class ObbRulesTest {
    static int checks = 0, failures = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static void eq(String got, String want, String what) {
        check(want.equals(got), what + ": got " + got + ", want " + want);
    }

    public static void main(String[] a) {
        String p = "com.turner.fcallstars";
        eq(ObbRules.copyName("main.45.com.turner.fcallstars.obb", p, 45), "main.45.com.turner.fcallstars.rb.obb", "main OBB renamed");
        eq(ObbRules.copyName("patch.46.com.turner.fcallstars.obb", p, 45), "patch.46.com.turner.fcallstars.rb.obb", "patch OBB keeps its own version");
        eq(ObbRules.copyName("10.com.sega.sonicracing.obb", "com.sega.sonicracing", 10), "10.com.sega.sonicracing.rb.obb",
                "a game's own naming keeps its shape");
        eq(ObbRules.copyName("MAIN.45.com.turner.fcallstars.OBB", p, 45), "MAIN.45.com.turner.fcallstars.rb.obb", "case of .obb");
        eq(ObbRules.copyName("fcallstars-data.obb", p, 45), "main.45.com.turner.fcallstars.rb.obb",
                "a file named otherwise becomes the game's main OBB");
        eq(ObbRules.copyName("main.45.com.other.game.obb", p, 45), "main.45.com.turner.fcallstars.rb.obb",
                "another game's OBB name is not kept");
        check(ObbRules.isObb("main.1.a.b.obb") && ObbRules.isObb("X.OBB") && !ObbRules.isObb("game.apk") && !ObbRules.isObb(null),
                "isObb");
        check(ObbRules.isObb("main.1.a.b.obb") && !ObbRules.belongsTo("main.1.com.other.obb", p)
                && ObbRules.belongsTo("main.45.com.turner.fcallstars.obb", p), "belongsTo: an original's own OBB names it");
        System.out.println("ObbRulesTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
