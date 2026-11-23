// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for the DEX authority patch (C47): a game that names its provider authority as a string has to
// follow the manifest's rename, and the edit must leave a DEX ART still accepts — the string table is rewritten
// in place, so the header's SHA-1 signature and Adler-32 checksum have to be recomputed.
package com.retrobug.launcher;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.security.MessageDigest;
import java.util.zip.Adler32;

public class DexStringsTest {
    static int checks = 0, failures = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            failures++;
            System.out.println("FAIL: " + what);
        }
    }

    static byte[] load(Path dir, String name) throws Exception {
        return Files.readAllBytes(dir.resolve(name));
    }

    static boolean checksumOk(byte[] dex) {
        Adler32 adler = new Adler32();
        adler.update(dex, 12, dex.length - 12);
        int want = (int) adler.getValue();
        int got = (dex[8] & 0xff) | (dex[9] & 0xff) << 8 | (dex[10] & 0xff) << 16 | (dex[11] & 0xff) << 24;
        return want == got;
    }

    static boolean signatureOk(byte[] dex) throws Exception {
        MessageDigest sha = MessageDigest.getInstance("SHA-1");
        sha.update(dex, 32, dex.length - 32);
        byte[] want = sha.digest();
        for (int i = 0; i < 20; i++) if (want[i] != dex[12 + i]) return false;
        return true;
    }

    public static void main(String[] a) throws Exception {
        Path fixtures = Paths.get(a.length > 0 ? a[0] : "fixtures");
        final String OLD = "com.retrobug.fixture.ipc_provider";
        final String NEW = "com.retrobug.fixture.rbc_provider";  // same length, in place: "ipc" -> "rbc"

        byte[] dex = load(fixtures, "AuthorityFixture.dex");
        check(DexScanner.containsStringPart(dex, OLD), "the fixture names its authority by literal");
        check(checksumOk(dex) && signatureOk(dex), "the fixture starts with valid digests");

        // Same length and still between its string-table neighbours: the record can be rewritten in place.
        int n = DexScanner.replaceStringPart(dex, OLD, NEW);
        check(n == 1, "one string follows the rename (got " + n + ")");
        check(!DexScanner.containsStringPart(dex, OLD), "the old authority is gone");
        check(DexScanner.containsStringPart(dex, "content://" + NEW), "the literal is the renamed URI");

        DexScanner.refreshDigests(dex);
        check(checksumOk(dex), "the checksum is recomputed after the edit");
        check(signatureOk(dex), "the SHA-1 signature is recomputed after the edit");

        // Anything that would change the record's length is refused: the table's records are packed, and the
        // verifier rejects a shorter one (and a longer one would run into its neighbour).
        byte[] shorter = load(fixtures, "AuthorityFixture.dex");
        check(DexScanner.replaceStringPart(shorter, OLD, "retrobug.fixture.rb.ipc_provider") == 0,
                "a shorter replacement is refused");
        check(DexScanner.containsStringPart(shorter, OLD), "and the original string is untouched");
        byte[] longer = load(fixtures, "AuthorityFixture.dex");
        check(DexScanner.replaceStringPart(longer, OLD, "com.retrobug.fixture.rb.ipc_provider") == 0,
                "a longer replacement is refused");
        check(DexScanner.containsStringPart(longer, OLD), "and the original string is untouched");

        System.out.println("DexStringsTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
