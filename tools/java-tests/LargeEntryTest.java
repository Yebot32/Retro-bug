// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// A game with an asset larger than the converter's heap (Among Us ships a 172 MB asset bundle) still converts: entries
// are streamed, never read whole. Run with a small heap (-Xmx96m) so a whole-entry read fails here as it did on-device.
package com.retrobug.launcher;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.util.Enumeration;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

public final class LargeEntryTest {
    static final int BIG = 160 << 20;  // more than the test heap

    /** Deterministic, poorly compressible bytes: position i of the asset. */
    static void fill(byte[] buf, long at) {
        for (int i = 0; i < buf.length; i++) {
            long x = (at + i) * 0x9E3779B97F4A7C15L;
            buf[i] = (byte) (x >>> 56);
        }
    }

    static long crcOfBig() {
        CRC32 c = new CRC32();
        byte[] buf = new byte[1 << 16];
        for (long at = 0; at < BIG; at += buf.length) { fill(buf, at); c.update(buf); }
        return c.getValue();
    }

    static void writeBig(OutputStream o) throws IOException {
        byte[] buf = new byte[1 << 16];
        for (long at = 0; at < BIG; at += buf.length) { fill(buf, at); o.write(buf); }
    }

    public static void main(String[] a) throws Exception {
        ApkRepackagerTest.fixtures = new File(a[0]);
        ApkRepackagerTest.scratch = new File(a[1]);
        File in = new File(ApkRepackagerTest.scratch, "large.apk");
        long crc = crcOfBig();
        try (ZipFile src = new ZipFile(new File(ApkRepackagerTest.fixtures, "min_only.apk"));
             ZipOutputStream zo = new ZipOutputStream(Files.newOutputStream(in.toPath()))) {
            for (Enumeration<? extends ZipEntry> en = src.entries(); en.hasMoreElements(); ) {
                ZipEntry se = en.nextElement();
                byte[] data;
                try (InputStream is = src.getInputStream(se)) { data = is.readAllBytes(); }
                if (se.getName().equals("resources.arsc")) { ApkRepackagerTest.putStored(zo, se.getName(), data); continue; }
                zo.putNextEntry(new ZipEntry(se.getName())); zo.write(data); zo.closeEntry();
            }
            zo.putNextEntry(new ZipEntry("lib/armeabi-v7a/libfoo.so")); zo.write(ApkRepackagerTest.b("V7A")); zo.closeEntry();
            ZipEntry st = new ZipEntry("assets/big_stored.bundle");
            st.setMethod(ZipEntry.STORED); st.setSize(BIG); st.setCompressedSize(BIG); st.setCrc(crc);
            zo.putNextEntry(st); writeBig(zo); zo.closeEntry();
            zo.putNextEntry(new ZipEntry("assets/big_deflated.bundle")); writeBig(zo); zo.closeEntry();
        }
        File out = new File(ApkRepackagerTest.scratch, "large.out.apk");
        Map<String, byte[]> rt = ApkRepackagerTest.runtime();
        ApkRepackager.Result r = ApkRepackager.transform(in, rt, out);
        try (ZipFile z = new ZipFile(out)) {
            for (String n : new String[] {"assets/big_stored.bundle", "assets/big_deflated.bundle"}) {
                ZipEntry e = z.getEntry(n);
                ApkRepackagerTest.check(e != null && e.getSize() == BIG, n + " is copied whole");
                if (e == null) continue;
                CRC32 c = new CRC32();
                byte[] buf = new byte[1 << 16];
                try (InputStream is = z.getInputStream(e)) { for (int k; (k = is.read(buf)) > 0; ) c.update(buf, 0, k); }
                ApkRepackagerTest.check(c.getValue() == crc, n + " keeps its bytes");
                ApkRepackagerTest.check(e.getMethod() == (n.contains("stored") ? ZipEntry.STORED : ZipEntry.DEFLATED),
                        n + " keeps its compression method");
            }
        }
        ApkRepackagerTest.check(r != null, "converted");
        in.delete();
        out.delete();
        System.out.println("LargeEntryTest: " + ApkRepackagerTest.checks + " checks, " + ApkRepackagerTest.failures + " failures");
        if (ApkRepackagerTest.failures > 0) System.exit(1);
    }
}
