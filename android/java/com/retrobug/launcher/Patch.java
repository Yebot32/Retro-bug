// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// A per-game patch (M6): byte edits to one of a game's 32-bit libraries, applied when its .rb copy is made. The
// original APK and the installed original game are never changed; turning a patch off and updating the copy puts the
// original bytes back. A patch applies only to the exact library it was written for (its SHA-256, when given) and
// only where every expected byte is in place — anything else is reported and nothing is changed. Pure Java.
//
// File format (.rbpatch, UTF-8, one key=value per line, # comments):
//   id=skip-intro                 lower-case letters, digits, '-', '_', '.'
//   title=Skip the intro movie
//   description=…                 optional
//   game=com.example.game         optional: the original package it is for
//   versionCode=42                optional: the game version it is for
//   lib=libgame.so                the library under lib/armeabi-v7a (or lib/armeabi)
//   sha256=…                      optional but recommended: the library it was written against
//   edit=0x10 10 11 12 13 -> 00 bf 00 bf    offset (hex 0x… or decimal), expected bytes -> new bytes (same count)
package com.retrobug.launcher;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

final class Patch {
    static final class Edit {
        final long offset;
        final byte[] expect, write;
        Edit(long offset, byte[] expect, byte[] write) { this.offset = offset; this.expect = expect; this.write = write; }
    }

    static final class Applied {
        final boolean ok;
        final String why;      // null when ok
        final byte[] bytes;    // the patched copy when ok
        Applied(boolean ok, String why, byte[] bytes) { this.ok = ok; this.why = why; this.bytes = bytes; }
    }

    String id, title, description = "", game, lib, sha256;
    long versionCode = -1;
    final List<Edit> edits = new ArrayList<>();
    /** The text it was parsed from (kept so the fingerprint covers the edits exactly). */
    String source;

    static Patch parse(String text) {
        Patch p = new Patch();
        p.source = text;
        for (String raw : text.split("\n")) {
            String line = raw.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            int eq = line.indexOf('=');
            if (eq <= 0) throw new IllegalArgumentException("not key=value: " + line);
            String k = line.substring(0, eq).trim(), v = line.substring(eq + 1).trim();
            switch (k) {
                case "id": p.id = v; break;
                case "title": p.title = v; break;
                case "description": p.description = v; break;
                case "game": p.game = v; break;
                case "versionCode": p.versionCode = Long.parseLong(v); break;
                case "lib": p.lib = v; break;
                case "sha256": p.sha256 = v.toLowerCase(Locale.ROOT); break;
                case "edit": p.edits.add(edit(v)); break;
                default: break;  // unknown keys are ignored: newer files stay readable
            }
        }
        if (p.id == null || !p.id.matches("[a-z0-9][a-z0-9._-]*") || p.id.contains(".."))
            throw new IllegalArgumentException("a patch needs an id of lower-case letters, digits, '-', '_', '.'");
        if (p.title == null || p.title.isEmpty()) throw new IllegalArgumentException("a patch needs a title");
        if (p.lib == null || !p.lib.matches("[A-Za-z0-9._+-]+\\.so") || p.lib.contains(".."))
            throw new IllegalArgumentException("a patch needs lib=<name>.so");
        if (p.edits.isEmpty()) throw new IllegalArgumentException("a patch needs at least one edit");
        return p;
    }

    private static Edit edit(String v) {
        int arrow = v.indexOf("->");
        if (arrow < 0) throw new IllegalArgumentException("edit needs '->': " + v);
        String[] left = v.substring(0, arrow).trim().split("\\s+");
        String[] right = v.substring(arrow + 2).trim().split("\\s+");
        if (left.length < 2) throw new IllegalArgumentException("edit needs an offset and expected bytes: " + v);
        long off;
        try {
            off = left[0].startsWith("0x") || left[0].startsWith("0X") ? Long.parseLong(left[0].substring(2), 16) : Long.parseLong(left[0]);
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("bad offset: " + left[0]);
        }
        byte[] expect = hex(Arrays.copyOfRange(left, 1, left.length)), write = hex(right);
        if (expect.length != write.length) throw new IllegalArgumentException("expected and new bytes differ in count: " + v);
        if (off < 0) throw new IllegalArgumentException("negative offset: " + v);
        return new Edit(off, expect, write);
    }

    private static byte[] hex(String[] parts) {
        byte[] b = new byte[parts.length];
        for (int i = 0; i < parts.length; i++) {
            if (!parts[i].matches("[0-9A-Fa-f]{2}")) throw new IllegalArgumentException("not a hex byte: " + parts[i]);
            b[i] = (byte) Integer.parseInt(parts[i], 16);
        }
        return b;
    }

    /** The library with every edit applied, or why not (and the input untouched either way). */
    Applied apply(byte[] libBytes) {
        if (sha256 != null && !sha256.isEmpty() && !sha256.equals(sha256(libBytes))) {
            return new Applied(false, "this game's " + lib + " is not the one the patch was made for (SHA-256 differs)", null);
        }
        byte[] out = libBytes.clone();
        for (Edit e : edits) {
            if (e.offset + e.expect.length > out.length) {
                return new Applied(false, String.format("edit at 0x%x reaches past the end of %s", e.offset, lib), null);
            }
            for (int i = 0; i < e.expect.length; i++) {
                if (out[(int) e.offset + i] != e.expect[i]) {
                    return new Applied(false, String.format("%s: the bytes at 0x%x are not the expected ones", lib, e.offset + i), null);
                }
            }
            System.arraycopy(e.write, 0, out, (int) e.offset, e.write.length);
        }
        return new Applied(true, null, out);
    }

    /** The patches whose ids are listed in `csv` (comma separated), in id order. */
    static List<Patch> enabled(List<Patch> all, String csv) {
        List<String> ids = new ArrayList<>();
        for (String s : csv == null ? new String[0] : csv.split(",")) if (!s.trim().isEmpty()) ids.add(s.trim());
        List<Patch> out = new ArrayList<>();
        for (Patch p : all) if (ids.contains(p.id)) out.add(p);
        Collections.sort(out, (x, y) -> x.id.compareTo(y.id));
        return out;
    }

    /** A short fingerprint of a set of patches (ids and exact content); "" for none. Stamped into the copy. */
    static String fingerprint(List<Patch> set) {
        if (set.isEmpty()) return "";
        StringBuilder s = new StringBuilder();
        for (Patch p : set) s.append(p.id).append('\n').append(p.source).append('\n');
        return sha256(s.toString().getBytes(StandardCharsets.UTF_8)).substring(0, 12);
    }

    static String sha256(byte[] b) {
        try {
            StringBuilder s = new StringBuilder();
            for (byte x : MessageDigest.getInstance("SHA-256").digest(b)) s.append(String.format("%02x", x));
            return s.toString();
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }
}
