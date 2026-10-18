// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// One game's settings, as the launcher stores them and a converted game reads them at start (through the launcher's
// SettingsProvider, then its shim): a few `key=value` lines. Anything missing, unknown or out of range reads as the
// default, so a malformed record can never reach the runtime as a value it does not expect.
package com.retrobug.launcher;

final class GameSettings {
    /** "fast" = Tier J (the JIT), "safe" = Tier I (the interpreter). */
    String cpu = "fast";
    /** The fps / instruction-rate readout over the game. */
    boolean overlay = false;
    /** The targetSdk the converted copy is made for (24..36); 35 installs past Play Protect on both test devices. */
    int target = 35;

    /** M6: enabled patch ids (comma separated), applied when the copy is made. */
    String patches = "";
    /** M6: enabled mod ids (comma separated), applied when the copy is made. */
    String mods = "";
    /** M6: render scale in percent (100, 75, 50) for games drawing through a SurfaceView; 100 = untouched. */
    int scale = 100;
    /** M6: "stretch" (fill the screen, as before), "fit169" or "fit43" (letterboxed to that shape). */
    String aspect = "stretch";

    boolean hasPatch(String id) { return ("," + patches + ",").contains("," + id + ","); }

    /** Only well-formed ids (as Patch/Mod ids are) survive; anything else is dropped. */
    static String ids(String csv) {
        StringBuilder out = new StringBuilder();
        for (String s : csv.split(",")) {
            String t = s.trim();
            if (t.matches("[a-z0-9][a-z0-9._-]*") && !t.contains("..")) out.append(out.length() == 0 ? "" : ",").append(t);
        }
        return out.toString();
    }

    static GameSettings parse(String text) {
        GameSettings s = new GameSettings();
        if (text == null) return s;
        for (String line : text.split("\n")) {
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            String k = line.substring(0, eq).trim(), v = line.substring(eq + 1).trim();
            switch (k) {
                case "cpu":
                    if (v.equals("fast") || v.equals("safe")) s.cpu = v;
                    break;
                case "overlay":
                    s.overlay = v.equals("1");
                    break;
                case "target":
                    try {
                        int t = Integer.parseInt(v);
                        if (t >= 24 && t <= 36) s.target = t;
                    } catch (NumberFormatException ignored) {
                        // the default stands
                    }
                    break;
                case "patches": s.patches = ids(v); break;
                case "mods": s.mods = ids(v); break;
                case "scale":
                    if (v.equals("100") || v.equals("75") || v.equals("50")) s.scale = Integer.parseInt(v);
                    break;
                case "aspect":
                    if (v.equals("stretch") || v.equals("fit169") || v.equals("fit43")) s.aspect = v;
                    break;
                default:
                    break;  // a newer launcher's key: ignored
            }
        }
        return s;
    }

    String format() {
        String s = "cpu=" + cpu + "\noverlay=" + (overlay ? "1" : "0") + "\ntarget=" + target + "\n";
        // M6 keys only when set: older settings text stays exactly as it was.
        if (!patches.isEmpty()) s += "patches=" + patches + "\n";
        if (!mods.isEmpty()) s += "mods=" + mods + "\n";
        if (scale != 100) s += "scale=" + scale + "\n";
        if (!aspect.equals("stretch")) s += "aspect=" + aspect + "\n";
        return s;
    }
}
