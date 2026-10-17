// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for ArtPicker: which image inside an APK becomes a game's backdrop. Each scoring rule has a row where it
// is the deciding one, so a rule that is missing or inverted fails exactly that row.
package com.retrobug.launcher;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

public class ArtPickerTest {
    static int checks = 0, failures = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static ApkScan.Image img(String path, int w, int h) { return new ApkScan.Image(path, w, h); }

    static String picked(ApkScan.Image... images) {
        ApkScan.Image p = ArtPicker.pick(new ArrayList<>(Arrays.asList(images)));
        return p == null ? null : p.path;
    }

    public static void main(String[] a) {
        check("assets/splash.png".equals(picked(img("assets/tex/a.png", 2048, 2048), img("assets/splash.png", 1280, 720))),
                "a keyword image beats a larger plain one");
        check("assets/title_wide.png".equals(picked(img("assets/title_tall.png", 720, 1280), img("assets/title_wide.png", 1280, 720))),
                "landscape beats portrait at the same keyword and size");
        check(picked(img("assets/splash.png", 640, 360)) == null, "under 800x480 never qualifies");
        check("assets/menu_tall.png".equals(picked(img("assets/menu_tall.png", 480, 800))), "480x800 portrait qualifies");
        check(picked(img("res/mipmap-xxxhdpi/ic_launcher.png", 1024, 1024)) == null, "a launcher icon is never the backdrop");
        check(picked(img("res/drawable-xxhdpi/ic_launcher_foreground.png", 1024, 1024)) == null, "nor its foreground layer");
        check(ArtPicker.pick(Collections.<ApkScan.Image>emptyList()) == null, "nothing to choose from: null");
        check("assets/bg.png".equals(picked(img("assets/bgx/bg.png", 1280, 720), img("assets/bg.png", 1280, 720))),
                "a tie goes to the shorter path");
        check("assets/a/bg.png".equals(picked(img("assets/b/bg.png", 1280, 720), img("assets/a/bg.png", 1280, 720))),
                "then to the lexicographically first");
        check("assets/Loading_Screen.JPG".equals(picked(img("assets/x.png", 900, 600), img("assets/Loading_Screen.JPG", 900, 600))),
                "keywords match regardless of case");
        System.out.println("ArtPickerTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
