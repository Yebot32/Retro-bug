// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// What the library needs to know about an APK without unpacking it: the ABIs under lib/ (is this a 32-bit game?) and
// the pixel size of every image inside, read from the image's own header — PNG's IHDR, JPEG's first SOF marker, WebP's
// VP8 / VP8L / VP8X chunk. No image is decoded here; ArtPicker chooses from these sizes and ArtCache decodes one.
package com.retrobug.launcher;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class ApkScan {
    private ApkScan() {}

    /** At most this much of an image entry is read: a JPEG's SOF can sit behind a large EXIF or ICC segment. */
    static final int HEAD_BYTES = 128 * 1024;

    static final class Image {
        final String path;
        final int width, height;

        Image(String path, int width, int height) { this.path = path; this.width = width; this.height = height; }
    }

    static final class Info {
        final Set<String> abis;
        final List<Image> images;

        Info(Set<String> abis, List<Image> images) {
            this.abis = Collections.unmodifiableSet(abis);
            this.images = Collections.unmodifiableList(images);
        }

        /** Built for 32-bit ARM only: what RetroBug is for. A 64-bit build in the same APK runs natively instead. */
        boolean is32BitOnly() {
            return (abis.contains("armeabi") || abis.contains("armeabi-v7a")) && !abis.contains("arm64-v8a");
        }
    }

    static Info scan(File apk) throws IOException {
        Set<String> abis = new TreeSet<>();
        List<Image> images = new ArrayList<>();
        try (ZipFile zip = new ZipFile(apk)) {
            for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement();
                String n = e.getName();
                if (n.startsWith("lib/")) {
                    int slash = n.indexOf('/', 4);
                    if (slash > 4) abis.add(n.substring(4, slash));
                    continue;
                }
                String lower = n.toLowerCase();
                if (lower.endsWith(".9.png") || e.isDirectory()) continue;
                if (!(lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".webp")))
                    continue;
                byte[] head;
                try (InputStream in = zip.getInputStream(e)) { head = readUpTo(in, HEAD_BYTES); }
                int[] size = imageSize(head);
                if (size != null && size[0] > 0 && size[1] > 0) images.add(new Image(n, size[0], size[1]));
            }
        }
        return new Info(abis, images);
    }

    /** {width, height} from an image's first bytes, or null when they are not a PNG, JPEG or WebP header. */
    static int[] imageSize(byte[] b) {
        if (b == null) return null;
        // PNG: 8-byte signature, then the IHDR chunk (length, "IHDR", width, height — big-endian).
        if (b.length >= 24 && (b[0] & 0xff) == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G'
                && b[12] == 'I' && b[13] == 'H' && b[14] == 'D' && b[15] == 'R') {
            return new int[] {be32(b, 16), be32(b, 20)};
        }
        // JPEG: walk the segments to the first SOFn (C0..CF, except C4 DHT, C8 JPG, CC DAC).
        if (b.length >= 4 && (b[0] & 0xff) == 0xFF && (b[1] & 0xff) == 0xD8) {
            int p = 2;
            while (p + 9 < b.length) {
                if ((b[p] & 0xff) != 0xFF) return null;
                int marker = b[p + 1] & 0xff;
                if (marker == 0xFF) { p++; continue; }  // fill byte
                int len = ((b[p + 2] & 0xff) << 8) | (b[p + 3] & 0xff);
                if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                    int h = ((b[p + 5] & 0xff) << 8) | (b[p + 6] & 0xff);
                    int w = ((b[p + 7] & 0xff) << 8) | (b[p + 8] & 0xff);
                    return new int[] {w, h};
                }
                if (len < 2) return null;
                p += 2 + len;
            }
            return null;
        }
        // WebP: RIFF....WEBP, then the first chunk.
        if (b.length >= 30 && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F'
                && b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P') {
            if (b[12] == 'V' && b[13] == 'P' && b[14] == '8') {
                if (b[15] == 'X') return new int[] {le24(b, 24) + 1, le24(b, 27) + 1};
                if (b[15] == 'L' && (b[20] & 0xff) == 0x2F) {  // lossless: 14-bit width-1, 14-bit height-1
                    int bits = (b[21] & 0xff) | (b[22] & 0xff) << 8 | (b[23] & 0xff) << 16 | (b[24] & 0xff) << 24;
                    return new int[] {(bits & 0x3FFF) + 1, ((bits >>> 14) & 0x3FFF) + 1};
                }
                if (b[15] == ' ' && (b[23] & 0xff) == 0x9D && (b[24] & 0xff) == 0x01 && (b[25] & 0xff) == 0x2A) {
                    return new int[] {le16(b, 26) & 0x3FFF, le16(b, 28) & 0x3FFF};  // lossy keyframe
                }
            }
            return null;
        }
        return null;
    }

    private static int be32(byte[] b, int o) {
        return (b[o] & 0xff) << 24 | (b[o + 1] & 0xff) << 16 | (b[o + 2] & 0xff) << 8 | (b[o + 3] & 0xff);
    }
    private static int le16(byte[] b, int o) { return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8; }
    private static int le24(byte[] b, int o) { return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8 | (b[o + 2] & 0xff) << 16; }

    private static byte[] readUpTo(InputStream in, int max) throws IOException {
        byte[] buf = new byte[max];
        int n = 0;
        for (int r; n < max && (r = in.read(buf, n, max - n)) > 0; ) n += r;
        byte[] out = new byte[n];
        System.arraycopy(buf, 0, out, 0, n);
        return out;
    }
}
