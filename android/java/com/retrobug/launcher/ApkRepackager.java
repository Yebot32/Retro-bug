// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
package com.retrobug.launcher;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.util.GregorianCalendar;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * Rewrites a 32-bit game's APK so a 64-bit-only device will install it: the 32-bit libs move to
 * {@code assets/rb32/} (where the runtime reads them), each is replaced by a 64-bit stub under
 * {@code lib/arm64-v8a/} (the platform picks the app's ABI from that directory, and the stub's job is to boot
 * the guest engine), and targetSdk is raised to the install floor. The result is unsigned: signing is a
 * separate step, and the old signature files are dropped because they would no longer verify.
 *
 * Pure java.util.zip and no android.* imports, so it is host-testable.
 */
final class ApkRepackager {
    private ApkRepackager() {}

    /** Lowest targetSdk the platform will install without a "built for an older Android" block. */
    static final int INSTALL_FLOOR_SDK = 24;
    /** minSdk the output declares at least (never lowered); RepackSigner signs for the same level. */
    static final int MIN_SDK_FLOOR = 24;
    static final String STUB_KEY = "stub";

    private static final String V7A = "lib/armeabi-v7a/";
    private static final String ARM = "lib/armeabi/";
    private static final String ARM64 = "lib/arm64-v8a/";
    private static final String GUEST_DIR = "assets/rb32/";
    private static final String MANIFEST = "AndroidManifest.xml";

    static final class Result {
        String packageName;
        int oldTargetSdk = -1;   // -1: the manifest declared none
        int newTargetSdk;
        int oldMinSdk = -1;      // -1: the manifest declared none (the platform reads that as 1)
        int newMinSdk;
        List<String> guestLibs = new ArrayList<>();
        List<String> stubs = new ArrayList<>();
        List<String> report = new ArrayList<>();
    }

    private static IOException refused(String why) { return new IOException("refused: " + why); }

    static Result transform(File inApk, Map<String, byte[]> runtimeFiles, File outUnsigned) throws IOException {
        byte[] stub = runtimeFiles.get(STUB_KEY);
        if (stub == null) throw new IOException("runtimeFiles has no \"" + STUB_KEY + "\" entry");
        try {
            return run(inApk, stub, runtimeFiles, outUnsigned);
        } catch (IOException | RuntimeException e) {
            // A half-written zip must never be mistaken for a finished one by the signing step.
            Files.deleteIfExists(outUnsigned.toPath());
            throw e;
        }
    }

    private static Result run(File inApk, byte[] stub, Map<String, byte[]> runtimeFiles, File out) throws IOException {
        Result r = new Result();
        try (ZipFile zip = new ZipFile(inApk)) {
            ZipEntry me = zip.getEntry(MANIFEST);
            if (me == null) throw new IOException("no " + MANIFEST + " in " + inApk);
            byte[] manifest;
            try (InputStream in = zip.getInputStream(me)) { manifest = readFully(in); }
            AxmlEditor ed = AxmlEditor.parse(manifest);
            r.packageName = ed.packageName();

            // With extraction off the platform maps libs straight out of the APK, so the stubs would have to
            // be the real libs; there is nothing for the guest engine to load.
            if (Boolean.FALSE.equals(ed.extractNativeLibs()))
                throw refused("extractNativeLibs=\"false\" (libs are mapped from the APK, stubs would not load)");

            boolean hasV7a = false;
            for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement();
                if (e.isDirectory()) continue;
                if (e.getName().startsWith(ARM64))
                    throw refused("already has " + e.getName() + " (app is not 32-bit only)");
                if (e.getName().startsWith(V7A)) hasV7a = true;
            }
            // armeabi libs run on v7a hardware, so an armeabi-only app is a valid input; they are only
            // redundant (and dropped) when a v7a directory supplies the real set.
            String guestDir = hasV7a ? V7A : ARM;

            // minSdk is raised to the same floor: the output only ever runs on a 64-bit device (the stubs are
            // arm64), and a declared minSdk below 18 makes the v1 signature's SHA-256 digest "unsupported" to
            // `apksigner verify`, which checks v1 down to the manifest's minSdk.
            boolean edited = false;
            Integer oldMin = ed.minSdk();
            r.oldMinSdk = oldMin == null ? -1 : oldMin;
            if (oldMin != null && oldMin >= MIN_SDK_FLOOR) {
                r.newMinSdk = oldMin;    // never lowered
                r.report.add("minSdk " + oldMin + " already >= " + MIN_SDK_FLOOR + ", left unchanged");
            } else {
                ed.setMinSdk(MIN_SDK_FLOOR);
                edited = true;
                r.newMinSdk = MIN_SDK_FLOOR;
                r.report.add("minSdk " + (oldMin == null ? "absent" : oldMin) + " -> " + MIN_SDK_FLOOR);
            }

            Integer oldT = ed.targetSdk();
            r.oldTargetSdk = oldT == null ? -1 : oldT;
            // Never below the install floor nor below the (new) minSdk; never lowered.
            int floorT = Math.max(INSTALL_FLOOR_SDK, r.newMinSdk);
            if (oldT == null && oldMin != null && oldMin >= floorT) {
                // An absent targetSdk means "= minSdk"; writing the floor here would lower it.
                r.newTargetSdk = oldMin;
                r.report.add("targetSdk absent (defaults to minSdk " + oldMin + "), left unchanged");
            } else if (oldT != null && oldT >= floorT) {
                r.newTargetSdk = oldT;
                r.report.add("targetSdk " + oldT + " already >= " + floorT + ", left unchanged");
            } else {
                ed.setTargetSdk(floorT);
                edited = true;
                r.newTargetSdk = floorT;
                r.report.add("targetSdk " + (oldT == null ? "absent" : oldT) + " -> " + floorT);
            }
            if (edited) manifest = ed.toBytes();

            List<ZipEntry> guest = new ArrayList<>();
            List<String> sigDropped = new ArrayList<>();
            Set<String> names = new HashSet<>();
            try (ZipOutputStream zo = new ZipOutputStream(Files.newOutputStream(out.toPath()))) {
                putBytes(zo, MANIFEST, manifest, ZipEntry.DEFLATED);
                names.add(MANIFEST);
                for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements(); ) {
                    ZipEntry e = en.nextElement();
                    String n = e.getName();
                    if (e.isDirectory() || n.equals(MANIFEST)) continue;
                    if (n.startsWith("META-INF/")) { sigDropped.add(n); continue; }
                    if (n.startsWith("lib/")) {
                        if (n.startsWith(guestDir)) { guest.add(e); continue; }
                        r.report.add("dropped " + n + (n.startsWith(ARM) ? " (lib/armeabi-v7a/ supplies the 32-bit set)"
                                : " (not a 32-bit ARM lib)"));
                        continue;
                    }
                    claim(names, n);
                    copy(zip, e, zo);
                }
                if (guest.isEmpty()) throw refused("no 32-bit ARM native libs (lib/armeabi-v7a or lib/armeabi)");
                if (!sigDropped.isEmpty())
                    r.report.add("dropped " + sigDropped.size() + " META-INF entries (old signature no longer valid): "
                            + sigDropped);

                List<String> stubs = new ArrayList<>();
                for (ZipEntry e : guest) {
                    String name = e.getName().substring(guestDir.length());
                    r.guestLibs.add(name);
                    String dest = GUEST_DIR + name;
                    claim(names, dest);
                    try (InputStream in = zip.getInputStream(e)) { putBytes(zo, dest, readFully(in), ZipEntry.DEFLATED); }
                    // Only .so files are loaded by name through the linker; anything else (gdbserver, data
                    // blobs) is carried along for the guest but gets no stub the platform would try to load.
                    if (name.endsWith(".so") && name.indexOf('/') < 0) stubs.add(name);
                    else r.report.add(name + ": no stub (not a top-level .so); kept only under " + GUEST_DIR);
                }
                Collections.sort(stubs);
                for (String s : stubs) {
                    claim(names, ARM64 + s);
                    putBytes(zo, ARM64 + s, stub, ZipEntry.DEFLATED);
                }
                r.stubs = stubs;

                for (Map.Entry<String, byte[]> f : runtimeFiles.entrySet()) {
                    if (f.getKey().equals(STUB_KEY)) continue;
                    claim(names, f.getKey());
                    putBytes(zo, f.getKey(), f.getValue(), ZipEntry.DEFLATED);
                }
            }
        }
        return r;
    }

    private static void claim(Set<String> names, String n) throws IOException {
        if (!names.add(n)) throw refused("duplicate output entry " + n);
    }

    // Fixed timestamp: the same input and runtime must give byte-identical output.
    private static void stamp(ZipEntry e) {
        // Calendar fields in the default zone: ZipEntry stores local DOS time, so this is 2008-01-01 00:00 in every zone.
        e.setTime(new GregorianCalendar(2008, 0, 1, 0, 0, 0).getTimeInMillis());
    }

    // InputStream.readAllBytes is API 33; this class runs on-device from API 26.
    private static byte[] readFully(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        for (int n; (n = in.read(buf)) > 0; ) bo.write(buf, 0, n);
        return bo.toByteArray();
    }

    private static void putBytes(ZipOutputStream zo, String name, byte[] data, int method) throws IOException {
        ZipEntry e = new ZipEntry(name);
        e.setMethod(method);
        if (method == ZipEntry.STORED) {
            CRC32 c = new CRC32(); c.update(data);
            e.setSize(data.length); e.setCompressedSize(data.length); e.setCrc(c.getValue());
        }
        stamp(e);
        zo.putNextEntry(e);
        zo.write(data);
        zo.closeEntry();
    }

    /** Copies an entry keeping its compression method (resources.arsc must stay STORED for newer Android). */
    private static void copy(ZipFile zip, ZipEntry src, ZipOutputStream zo) throws IOException {
        byte[] data;
        try (InputStream in = zip.getInputStream(src)) { data = readFully(in); }
        putBytes(zo, src.getName(), data, src.getMethod() == ZipEntry.STORED ? ZipEntry.STORED : ZipEntry.DEFLATED);
    }
}
