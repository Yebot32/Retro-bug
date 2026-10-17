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
                default:
                    break;  // a newer launcher's key: ignored
            }
        }
        return s;
    }

    String format() {
        return "cpu=" + cpu + "\noverlay=" + (overlay ? "1" : "0") + "\ntarget=" + target + "\n";
    }
}
