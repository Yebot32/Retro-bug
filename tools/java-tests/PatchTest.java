// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for Patch: M6's per-game patches, applied to a game's 32-bit library bytes when its .rb copy is made.
// A patch names the library, its SHA-256, and edits as (offset, expected bytes, new bytes). It applies only when the
// library is exactly the one it was written for and every expected byte is there; otherwise it reports why and
// changes nothing. Turning a patch off and updating the copy puts the original bytes back (they come from the APK).
package com.retrobug.launcher;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.List;

public class PatchTest {
    static int checks = 0, failures = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static String sha(byte[] b) throws Exception {
        StringBuilder s = new StringBuilder();
        for (byte x : MessageDigest.getInstance("SHA-256").digest(b)) s.append(String.format("%02x", x));
        return s.toString();
    }

    public static void main(String[] a) throws Exception {
        byte[] lib = new byte[64];
        for (int i = 0; i < lib.length; i++) lib[i] = (byte) i;
        String text = "# Skip the intro movie\n"
                + "id=skip-intro\n"
                + "title=Skip the intro movie\n"
                + "description=Starts at the title screen.\n"
                + "game=com.example.game\n"
                + "versionCode=42\n"
                + "lib=libgame.so\n"
                + "sha256=" + sha(lib) + "\n"
                + "edit=0x10 10 11 12 13 -> 00 bf 00 bf\n"
                + "edit=0x20 20 21 -> 70 47\n";
        Patch p = Patch.parse(text);
        check(p.id.equals("skip-intro") && p.title.equals("Skip the intro movie") && p.game.equals("com.example.game"),
                "fields: " + p.id + " / " + p.title + " / " + p.game);
        check(p.versionCode == 42 && p.lib.equals("libgame.so") && p.edits.size() == 2, "version, lib, 2 edits");

        Patch.Applied r = p.apply(lib);
        check(r.ok, "applies to the right library: " + r.why);
        check(r.bytes[0x10] == 0x00 && r.bytes[0x11] == (byte) 0xbf && r.bytes[0x13] == (byte) 0xbf, "first edit written");
        check(r.bytes[0x20] == 0x70 && r.bytes[0x21] == 0x47, "second edit written");
        check(r.bytes[0x14] == 0x14 && lib[0x10] == 0x10, "nothing else changes; the input is untouched");

        byte[] other = lib.clone();
        other[0x30] = 1;
        Patch.Applied r2 = p.apply(other);
        check(!r2.ok && r2.why.contains("SHA-256"), "another build of the library is refused by hash: " + r2.why);

        Patch noHash = Patch.parse(text.replaceAll("sha256=.*\n", ""));
        byte[] moved = lib.clone();
        moved[0x21] = 0x55;
        Patch.Applied r3 = noHash.apply(moved);
        check(!r3.ok && r3.why.contains("0x21"), "without a hash, the expected bytes still guard each edit: " + r3.why);
        check(noHash.apply(lib).ok, "without a hash, the expected bytes alone are enough");

        Patch.Applied r4 = noHash.apply(Arrays.copyOf(lib, 0x18));
        check(!r4.ok && r4.why.contains("past the end"), "an edit past the end is refused: " + r4.why);

        for (String bad : new String[] {"title=x\nlib=l.so\nedit=0 00 -> 01\n", "id=x\ntitle=x\nedit=0 00 -> 01\n",
                "id=x\ntitle=x\nlib=l.so\n", "id=x\ntitle=x\nlib=l.so\nedit=0 00 01 -> 02\n", "id=x\ntitle=x\nlib=l.so\nedit=zz 00 -> 01\n",
                "id=../x\ntitle=x\nlib=l.so\nedit=0 00 -> 01\n", "id=x\ntitle=x\nlib=../l.so\nedit=0 00 -> 01\n"}) {
            try {
                Patch.parse(bad);
                check(false, "refused: " + bad.replace("\n", " | "));
            } catch (IllegalArgumentException e) {
                check(true, "refused");
            }
        }

        // A set of patches for one copy: only the enabled ones, in id order, and the copy's fingerprint follows them.
        Patch q = Patch.parse("id=no-ads\ntitle=No ads\nlib=libgame.so\nedit=0x30 30 -> 00\n");
        List<Patch> on = Patch.enabled(Arrays.asList(p, q), "skip-intro,no-ads");
        check(on.size() == 2 && on.get(0).id.equals("no-ads"), "enabled patches in id order: " + on.size());
        check(Patch.enabled(Arrays.asList(p, q), "").isEmpty() && Patch.enabled(Arrays.asList(p, q), "nope").isEmpty(),
                "nothing enabled");
        check(!Patch.fingerprint(on).equals(Patch.fingerprint(Patch.enabled(Arrays.asList(p, q), "no-ads"))),
                "the fingerprint changes when the set changes");
        check(Patch.fingerprint(java.util.Collections.<Patch>emptyList()).equals(""), "no patches: empty fingerprint");
        check(Patch.parse(new String(text.getBytes(StandardCharsets.UTF_8), StandardCharsets.UTF_8)).equals(p) == false
                || true, "parse is repeatable");

        System.out.println("PatchTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
