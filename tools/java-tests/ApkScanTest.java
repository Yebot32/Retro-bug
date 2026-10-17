// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for ApkScan: which ABIs an APK carries (the library's "is this a 32-bit game" filter) and the pixel sizes
// of its images, read from the image headers alone. The images are hand-built header bytes of known sizes, so a wrong
// offset in any of the three formats fails a row.
//
// args: <scratch dir>
package com.retrobug.launcher;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public class ApkScanTest {
    static int checks = 0, failures = 0;
    static File scratch;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static File zip(String name, Map<String, byte[]> entries) throws IOException {
        File f = new File(scratch, name);
        try (ZipOutputStream zo = new ZipOutputStream(new FileOutputStream(f))) {
            for (Map.Entry<String, byte[]> e : entries.entrySet()) {
                zo.putNextEntry(new ZipEntry(e.getKey()));
                zo.write(e.getValue());
                zo.closeEntry();
            }
        }
        return f;
    }

    static void be32(ByteArrayOutputStream o, int v) { o.write(v >>> 24); o.write(v >>> 16); o.write(v >>> 8); o.write(v); }
    static void le16(ByteArrayOutputStream o, int v) { o.write(v); o.write(v >>> 8); }
    static void le24(ByteArrayOutputStream o, int v) { o.write(v); o.write(v >>> 8); o.write(v >>> 16); }
    static void le32(ByteArrayOutputStream o, int v) { o.write(v); o.write(v >>> 8); o.write(v >>> 16); o.write(v >>> 24); }

    /** A PNG signature and IHDR chunk for w x h (no image data: sizes come from the header). */
    static byte[] png(int w, int h) {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        o.write(0x89); o.write('P'); o.write('N'); o.write('G'); o.write(0x0D); o.write(0x0A); o.write(0x1A); o.write(0x0A);
        be32(o, 13); o.write('I'); o.write('H'); o.write('D'); o.write('R');
        be32(o, w); be32(o, h); o.write(8); o.write(6); o.write(0); o.write(0); o.write(0);
        be32(o, 0);  // CRC (not checked)
        return o.toByteArray();
    }

    /** SOI, an APP1 (EXIF) segment of 300 bytes, then SOF0 for w x h. */
    static byte[] jpeg(int w, int h) {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        o.write(0xFF); o.write(0xD8);
        o.write(0xFF); o.write(0xE1); o.write(300 >>> 8); o.write(300 & 0xFF);
        for (int i = 0; i < 298; i++) o.write(0);
        o.write(0xFF); o.write(0xC0); o.write(0); o.write(17); o.write(8);
        o.write(h >>> 8); o.write(h & 0xFF); o.write(w >>> 8); o.write(w & 0xFF);
        for (int i = 0; i < 10; i++) o.write(0);
        return o.toByteArray();
    }

    /** RIFF/WEBP with a VP8X chunk for w x h (canvas sizes stored minus one, 24-bit little-endian). */
    static byte[] webp(int w, int h) {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        o.write('R'); o.write('I'); o.write('F'); o.write('F'); le32(o, 30);
        o.write('W'); o.write('E'); o.write('B'); o.write('P');
        o.write('V'); o.write('P'); o.write('8'); o.write('X'); le32(o, 10);
        le32(o, 0); le24(o, w - 1); le24(o, h - 1);
        return o.toByteArray();
    }

    static ApkScan.Image find(ApkScan.Info info, String path) {
        for (ApkScan.Image i : info.images) if (i.path.equals(path)) return i;
        return null;
    }

    static void abis() throws IOException {
        Map<String, byte[]> a = new LinkedHashMap<>();
        a.put("lib/armeabi-v7a/libx.so", new byte[] {1});
        check(ApkScan.scan(zip("v7a.apk", a)).is32BitOnly(), "armeabi-v7a only is a 32-bit game");
        Map<String, byte[]> b = new LinkedHashMap<>(a);
        b.put("lib/arm64-v8a/libx.so", new byte[] {1});
        check(!ApkScan.scan(zip("both.apk", b)).is32BitOnly(), "with arm64-v8a it is not");
        Map<String, byte[]> c = new LinkedHashMap<>();
        c.put("lib/armeabi/liby.so", new byte[] {1});
        check(ApkScan.scan(zip("armeabi.apk", c)).is32BitOnly(), "armeabi only is a 32-bit game");
        Map<String, byte[]> d = new LinkedHashMap<>();
        d.put("classes.dex", new byte[] {1});
        check(!ApkScan.scan(zip("nolib.apk", d)).is32BitOnly(), "no lib/ is not a 32-bit game");
    }

    static void images() throws IOException {
        Map<String, byte[]> m = new LinkedHashMap<>();
        m.put("assets/splash.png", png(1280, 720));
        m.put("res/drawable/title.jpg", jpeg(800, 480));
        m.put("assets/bg.webp", webp(1920, 1080));
        m.put("res/drawable/frame.9.png", png(64, 64));
        m.put("assets/broken.png", new byte[] {(byte) 0x89, 'P', 'N', 'G'});
        m.put("assets/readme.txt", new byte[] {'x'});
        ApkScan.Info info = ApkScan.scan(zip("img.apk", m));
        ApkScan.Image p = find(info, "assets/splash.png"), j = find(info, "res/drawable/title.jpg"),
                w = find(info, "assets/bg.webp");
        check(p != null && p.width == 1280 && p.height == 720, "PNG size from IHDR");
        check(j != null && j.width == 800 && j.height == 480, "JPEG size from SOF0 after an EXIF segment");
        check(w != null && w.width == 1920 && w.height == 1080, "WebP size from VP8X");
        check(find(info, "res/drawable/frame.9.png") == null, "9-patches are skipped");
        check(find(info, "assets/broken.png") == null, "a truncated image is left out, without an exception");
        check(info.images.size() == 3, "only the three images: " + info.images.size());
        check(ApkScan.imageSize(new byte[] {1, 2, 3}) == null, "unknown bytes have no size");
    }

    public static void main(String[] a) throws Exception {
        scratch = new File(a[0]);
        abis();
        images();
        System.out.println("ApkScanTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
