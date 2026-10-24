// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
package com.retrobug.launcher;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.GregorianCalendar;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * Rewrites a 32-bit game's APK so a 64-bit-only device will install it: the 32-bit libs move to
 * {@code assets/rb32/} (where the runtime reads them), each is replaced by a 64-bit stub under
 * {@code lib/arm64-v8a/} (the platform picks the app's ABI from that directory, and the stub's job is to boot
 * the guest engine), and targetSdk is raised to the install floor. Because that raise switches on behaviour
 * changes the old Java was not written for, the targetSdk compatibility shims (ShimPlan) are added: a provider in
 * the manifest, its classes as the next classesN.dex, its config under {@code assets/rb32/}; the report names
 * the risky calls found in the game's DEX. The result is unsigned: signing is a
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
    /** Diagnostics: the next transform marks the output debuggable (`-e debuggable 1`). Never set by the UI. */
    static volatile boolean debuggable = false;
    /** Diagnostics: the next transform is a native control (`-e native 1`): renamed and re-signed like a repack, but the
     *  32-bit libraries stay in lib/ and no runtime is added, so a 32-bit-capable device runs the same APK natively. */
    static volatile boolean nativeControl = false;
    /** Diagnostics: the next transform targets at least this SDK (`-e target N`; 0 = the install floor). Play Protect
     *  now blocks "built for an older Android" installs at the floor on some devices, and a modern target installs. */
    static volatile int targetFloor = 0;

    private static final String V7A = "lib/armeabi-v7a/";
    private static final String ARM = "lib/armeabi/";
    private static final String ARM64 = "lib/arm64-v8a/";
    private static final String GUEST_DIR = "assets/rb32/";
    private static final String MANIFEST = "AndroidManifest.xml";
    private static final String ARSC = "resources.arsc";
    /**
     * The runtime generation a converted copy was made by, carried as {@code <meta-data RUNTIME_META=N>}. Raise it when
     * the runtime changes in a way copies must be re-made for; the library then shows "Update available". 2: the stamp
     * left the versionName (generation 1 put "-rb1" there) and copies gained save export/import.
     */
    static final int RUNTIME_STAMP = 2;
    static final String RUNTIME_META = "com.retrobug.runtime";
    /** The fingerprint (Patch.fingerprint) of the M6 patches requested for a copy; absent when none were. */
    static final String PATCHES_META = "com.retrobug.patches";
    /** The patches to apply to the next conversion (set by ConvertJob from the game's settings); empty = none. */
    static volatile List<Patch> patches = java.util.Collections.emptyList();
    /** The fingerprint (Mod.fingerprint) of the M6 mods requested for a copy; absent when none were. */
    static final String MODS_META = "com.retrobug.mods";
    /** The mods to apply to the next conversion (set by ConvertJob from the game's settings), in id order. */
    static volatile List<Mod> mods = java.util.Collections.emptyList();
    /** M6 aspect of the next conversion: "stretch" (untouched), "fit169" or "fit43" (activities pinned to that shape). */
    static volatile String aspect = "stretch";
    static final String ASPECT_META = "com.retrobug.aspect";
    /** "1" when a copy was kept at target 33 because the game registers receivers the old way (LibraryRules). */
    static final String RECEIVERS_META = "com.retrobug.receivers";

    static final class Result {
        String packageName;      // the output's package (the new one when renamed)
        String renamedFrom;      // the original package when renamed, else null
        int oldTargetSdk = -1;   // -1: the manifest declared none
        int newTargetSdk;
        int oldMinSdk = -1;      // -1: the manifest declared none (the platform reads that as 1)
        int newMinSdk;
        List<String> guestLibs = new ArrayList<>();
        List<String> stubs = new ArrayList<>();
        Map<String, List<String>> stubExports = new TreeMap<>();  // generated stubs: lib -> exported symbols
        List<String> report = new ArrayList<>();
        List<String> shims = new ArrayList<>();   // targetSdk compatibility shims added (ShimPlan ids)
        String shimDex;                           // classesN.dex entry carrying them, or null
        List<String> risks = new ArrayList<>();   // ShimPlan.riskReport lines (also in report)
    }

    private static IOException refused(String why) { return new IOException("refused: " + why); }

    static Result transform(File inApk, Map<String, byte[]> runtimeFiles, File outUnsigned) throws IOException {
        return transform(inApk, runtimeFiles, outUnsigned, null);
    }

    /** {@code renameTo} value that appends this to the original package: the launcher's "install beside" name. */
    static final String RENAME_SUFFIX = ".rb";

    /**
     * {@code renameTo}: null keeps the package; a value starting with '.' is appended to the original package;
     * anything else is the new package. A renamed app installs beside the original instead of replacing it
     * (AxmlEditor.renamePackage). resources.arsc's package name follows, so getIdentifier(..., getPackageName())
     * still finds the app's own resources.
     */
    static Result transform(File inApk, Map<String, byte[]> runtimeFiles, File outUnsigned, String renameTo)
            throws IOException {
        byte[] stub = runtimeFiles.get(STUB_KEY);
        if (stub == null) throw new IOException("runtimeFiles has no \"" + STUB_KEY + "\" entry");
        try {
            return run(inApk, stub, runtimeFiles, outUnsigned, renameTo);
        } catch (IOException | RuntimeException e) {
            // A half-written zip must never be mistaken for a finished one by the signing step.
            Files.deleteIfExists(outUnsigned.toPath());
            throw e;
        }
    }

    /**
     * Authorities the app names by string can only be renamed to something the string table already has room for:
     * the DEX holds the name in place, its records packed and sorted. For those the rename keeps the length and the
     * position (the last label's first two characters become "rb"), and the DEX literals are rewritten to match; a
     * name that cannot be rewritten that way is left with the generic rename and reported.
     */
    private static Map<String, String> authorityOverrides(ZipFile zip, AxmlEditor ed, String old, String neu, Result r)
            throws IOException {
        List<String> auths = ed.providerAuthorities();
        if (auths.isEmpty()) return Collections.emptyMap();
        List<byte[]> dexes = new ArrayList<>();
        for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements(); ) {
            ZipEntry e = en.nextElement();
            if (e.isDirectory() || !e.getName().matches("classes\\d*\\.dex")) continue;
            try {
                dexes.add(readFully(zip, e));
            } catch (IOException ignored) {
                // The copy loop reports an unreadable DEX itself; a scan that cannot read it decides nothing.
            }
        }
        Map<String, String> out = new HashMap<>();
        for (String a : auths) {
            if (AxmlEditor.renamedName(old, neu, a).length() <= a.length()) continue;  // the generic name fits
            String alias = sameLengthAlias(a);
            if (alias == null) continue;
            boolean named = false, rewritable = true;
            for (byte[] d : dexes) {
                try {
                    if (!DexScanner.containsStringPart(d, a)) continue;
                    named = true;
                    if (!DexScanner.canReplaceInPlace(d, a, alias)) {
                        rewritable = false;
                        break;
                    }
                } catch (IOException ignored) {
                }
            }
            if (!named) continue;
            if (!rewritable) {
                r.report.add("provider authority " + a + " is named in a DEX that cannot take " + alias + " in place; "
                        + "left as " + AxmlEditor.renamedName(old, neu, a));
                continue;
            }
            out.put(a, alias);
            r.report.add("provider authority " + a + " is named in a DEX; renamed in place to " + alias);
        }
        return out;
    }

    /** The same length as {@code a}, with "rb" in place of the last label's first two characters, or null. */
    private static String sameLengthAlias(String a) {
        int dot = a.lastIndexOf('.');
        if (dot < 0) return null;
        String label = a.substring(dot + 1);
        if (label.length() < 3) return null;
        return a.substring(0, dot + 1) + "rb" + label.substring(2);
    }

    private static Result run(File inApk, byte[] stub, Map<String, byte[]> runtimeFiles, File out, String renameTo)
            throws IOException {
        Result r = new Result();
        try (ZipFile zip = new ZipFile(inApk)) {
            ZipEntry me = zip.getEntry(MANIFEST);
            if (me == null) throw new IOException("no " + MANIFEST + " in " + inApk);
            byte[] manifest;
            manifest = readFully(zip, me);
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
            if (renameTo != null) {
                if (r.packageName == null) throw refused("manifest names no package to rename");
                String neu = renameTo.startsWith(".") ? r.packageName + renameTo : renameTo;
                // An authority a DEX names by string has to be renamed to something that fits the bytes the string
                // table already holds (see authorityOverrides), so that is decided before the manifest is written.
                ed.setAuthorityOverrides(authorityOverrides(zip, ed, r.packageName, neu, r));
                List<String> changes = ed.renamePackage(neu);
                r.renamedFrom = r.packageName;
                r.packageName = neu;
                edited = true;
                r.report.add("package " + r.renamedFrom + " -> " + neu + " (installs beside the original)");
                for (String c : changes) r.report.add("rename: " + c);
                // The shared user belongs to the original's signer: a copy signed with another key cannot join it.
                if (ed.removeAttribute("manifest", "sharedUserId")) {
                    ed.removeAttribute("manifest", "sharedUserLabel");
                    r.report.add("sharedUserId removed (the copy is signed with another key; it runs as its own user)");
                }
            }
            // A cap on the platform version keeps a copy off current Android; the copy targets it on purpose.
            if (ed.removeAttribute("uses-sdk", "maxSdkVersion")) {
                edited = true;
                r.report.add("uses-sdk maxSdkVersion removed (it would block installing on current Android)");
            }
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
            // Never below the install floor nor below the (new) minSdk; never lowered (LibraryRules, shared with the
            // library, which must know the target a copy will get). A native control is a diagnostic install on a
            // device whose Play Protect blocks old targets: current (34 makes registerReceiver without RECEIVER_* fatal).
            // An explicitly asked-for target wins in a native control too (diagnosing a game that needs < 31).
            int setting = targetFloor > 0 ? targetFloor : (nativeControl ? 33 : 0);
            // A game that calls registerReceiver the old way would throw at 34+: such a copy stays at 33 (LibraryRules).
            boolean receivers = registersReceivers(zip);
            int floorT = LibraryRules.outputTarget(oldT, oldMin, setting, receivers);
            if (floorT != LibraryRules.outputTarget(oldT, oldMin, setting)) {
                ed.addMetaDataString(RECEIVERS_META, "1");
                r.report.add("targetSdk kept at " + floorT + ": the game calls registerReceiver without the RECEIVER_* flags "
                        + "Android 14 requires from target 34");
            }
            if (oldT == null && oldMin != null && floorT == oldMin) {
                // An absent targetSdk means "= minSdk"; writing the floor here would lower it.
                r.newTargetSdk = oldMin;
                r.report.add("targetSdk absent (defaults to minSdk " + oldMin + "), left unchanged");
            } else if (oldT != null && floorT == oldT) {
                r.newTargetSdk = oldT;
                r.report.add("targetSdk " + oldT + " already high enough, left unchanged");
            } else {
                ed.setTargetSdk(floorT);
                edited = true;
                r.newTargetSdk = floorT;
                r.report.add("targetSdk " + (oldT == null ? "absent" : oldT) + " -> " + floorT);
            }
            // targetSdk compatibility shims (ShimPlan): decided from the target the original ran on.
            ShimPlan plan = ShimPlan.of(ShimPlan.effectiveTarget(oldT, oldMin), r.newTargetSdk);
            byte[] shimDex = runtimeFiles.get(ShimPlan.SHIM_DEX_KEY);
            // The shim provider goes into every copy that runs Java: it applies the shims (when any are needed) and it
            // is how the copy reads its RetroBug settings from the launcher at start (SettingsProvider).
            boolean addShims = false;
            if (plan.shims.isEmpty()) r.report.add("shims: none needed (original target " + plan.originalTarget + ")");
            if (Boolean.FALSE.equals(ed.hasCode())) {
                if (!plan.shims.isEmpty())
                    r.report.add("shims: NOT added — application hasCode=\"false\" (no Java to run them); needed: " + plan.shims);
            } else if (shimDex == null) {
                if (!plan.shims.isEmpty())
                    r.report.add("shims: NOT added — this launcher build has no shim dex; needed: " + plan.shims);
            } else {
                ed.addProvider(ShimPlan.PROVIDER_CLASS, r.packageName + ShimPlan.AUTHORITY_SUFFIX, Integer.MAX_VALUE);
                ed.addQueriesProvider(ShimPlan.SETTINGS_AUTHORITY);
                if (runtimeFiles.containsKey(ShimPlan.LAUNCHER_CERT_KEY)) {
                    // Save export/import for the Data tab; it answers only the launcher with that certificate.
                    ed.addProvider(ShimPlan.SAVES_PROVIDER_CLASS, r.packageName + ShimPlan.SAVES_AUTHORITY_SUFFIX, 0, true);
                    r.report.add("saves: " + ShimPlan.SAVES_PROVIDER_CLASS + " (exported; answers only the RetroBug launcher)");
                }
                edited = true;
                addShims = true;
            }
            if (debuggable) {  // diagnostics only (`-e debuggable 1`): simpleperf and run-as on the repack
                ed.setDebuggable();
                edited = true;
                r.report.add("debuggable: android:debuggable=\"true\" set for diagnostics (profiling, run-as) — not for players");
            }
            if (r.newTargetSdk >= 31) {  // API 31+ refuses an intent-filtered component with no explicit exported
                int n = ed.setExportedDefaults();
                if (n > 0) {
                    edited = true;
                    r.report.add("exported: " + n + " component(s) with intent filters given android:exported=\"true\" (their default before API 31)");
                }
            }
            // API 28 took the Apache HTTP client off the default classpath; an app built before it got it for free.
            if (ShimPlan.effectiveTarget(oldT, oldMin) < 28 && r.newTargetSdk >= 28
                    && ed.addUsesLibrary("org.apache.http.legacy", false)) {
                edited = true;
                r.report.add("uses-library: org.apache.http.legacy (required=false), on the classpath before API 28");
            }
            // The runtime stamp, for the library's "Update available": a meta-data entry, never the versionName (games
            // read their own versionName — SMB Bounce loads assets/dex/main-<versionName>.jar).
            ed.addMetaDataInt(RUNTIME_META, RUNTIME_STAMP);
            edited = true;
            r.report.add("stamp: <meta-data " + RUNTIME_META + "=" + RUNTIME_STAMP + ">");
            // M6 patches: applied to the guest libraries' bytes before anything is written (the manifest below carries the
            // fingerprint of the requested set).
            Map<String, byte[]> patched = new HashMap<>();
            for (Patch p : patches) {
                ZipEntry le = nativeControl ? null : zip.getEntry(guestDir + p.lib);
                if (le == null) {
                    r.report.add("patch " + p.id + ": not applied — the game has no " + guestDir + p.lib);
                    continue;
                }
                byte[] base = patched.get(p.lib);
                if (base == null) {
                    base = ElfRepair.repair(readFully(zip, le));
                }
                Patch.Applied a = p.apply(base);
                if (a.ok) {
                    patched.put(p.lib, a.bytes);
                    r.report.add("patch " + p.id + ": applied to " + p.lib + " (" + p.edits.size() + " edit(s))");
                } else {
                    r.report.add("patch " + p.id + ": not applied — " + a.why);
                }
            }
            // The requested set (not only the ones that fit): the library compares it with the player's choice, and a
            // patch that does not fit this build must not leave the game "out of date" forever (the report says why).
            if (!patches.isEmpty()) ed.addMetaDataString(PATCHES_META, Patch.fingerprint(patches));
            // M6 mods: which mod supplies each replaced or added path (a later id wins), stamped like the patches.
            Map<String, Mod> modFor = new TreeMap<>();
            for (Mod m : mods) for (String path : m.files()) modFor.put(path, m);
            if (!mods.isEmpty()) ed.addMetaDataString(MODS_META, Mod.fingerprint(mods));
            // M6 aspect: every activity pinned to one shape (min = max ratio), not resizeable, so Android letterboxes it.
            if (aspect.equals("fit169") || aspect.equals("fit43")) {
                float ratio = aspect.equals("fit169") ? 16f / 9f : 4f / 3f;
                int bits = Float.floatToIntBits(ratio);
                int n = ed.setTypedAttr("activity", "maxAspectRatio", 0x01010560, AxmlEditor.TYPE_FLOAT, bits);
                ed.setTypedAttr("activity", "minAspectRatio", 0x0101059b, AxmlEditor.TYPE_FLOAT, bits);
                ed.setTypedAttr("activity", "resizeableActivity", 0x010104f6, AxmlEditor.TYPE_INT_BOOLEAN, 0);
                ed.setTypedAttr("application", "resizeableActivity", 0x010104f6, AxmlEditor.TYPE_INT_BOOLEAN, 0);
                ed.addMetaDataString(ASPECT_META, aspect);
                r.report.add("aspect: " + n + " activit" + (n == 1 ? "y" : "ies") + " pinned to " + (aspect.equals("fit169") ? "16:9" : "4:3")
                        + " (letterboxed by Android)");
            }
            if (edited) manifest = ed.toBytes();

            List<ZipEntry> guest = new ArrayList<>();
            List<String> sigDropped = new ArrayList<>();
            Set<String> names = new HashSet<>();
            Set<String> refs = new TreeSet<>();
            Map<String, NativeMethods.Native> natives = new TreeMap<>();  // every classesN.dex's, by JNI name
            Map<String, byte[]> libBytes = new HashMap<>();
            try (ZipOutputStream zo = new ZipOutputStream(Files.newOutputStream(out.toPath()))) {
                putBytes(zo, MANIFEST, manifest, ZipEntry.DEFLATED);
                names.add(MANIFEST);
                for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements(); ) {
                    ZipEntry e = en.nextElement();
                    String n = e.getName();
                    if (e.isDirectory() || n.equals(MANIFEST)) continue;
                    if (isSignatureFile(n)) { sigDropped.add(n); continue; }
                    if (nativeControl && n.startsWith("lib/")) {  // the control run keeps the 32-bit libraries as they are
                        claim(names, n);
                        if (n.endsWith(".so")) {  // except for zeroed section-header fields, which the platform refuses
                            byte[] lib;
                            lib = readFully(zip, e);
                            byte[] fixed = ElfRepair.repair(lib);
                            if (fixed != lib) r.report.add("elf: " + n + " — e_shentsize/e_shstrndx restored (zeroed by a protection wrapper)");
                            putBytes(zo, n, fixed, e.getMethod() == ZipEntry.STORED ? ZipEntry.STORED : ZipEntry.DEFLATED);
                        } else {
                            copy(zip, e, zo);
                        }
                        continue;
                    }
                    if (n.startsWith("lib/")) {
                        if (n.startsWith(guestDir)) { guest.add(e); continue; }
                        r.report.add("dropped " + n + (n.startsWith(ARM) ? " (lib/armeabi-v7a/ supplies the 32-bit set)"
                                : " (not a 32-bit ARM lib)"));
                        continue;
                    }
                    claim(names, n);
                    Mod mod = modFor.get(n);
                    if (mod != null) {  // M6: the mod's file in place of the game's, compressed the way the original was
                        putBytes(zo, n, mod.read(n), e.getMethod() == ZipEntry.STORED ? ZipEntry.STORED : ZipEntry.DEFLATED);
                        r.report.add("mod " + mod.id + ": replaced " + n);
                        continue;
                    }
                    if (n.matches("classes\\d*\\.dex")) {
                        byte[] dex;
                        dex = readFully(zip, e);
                        try {
                            refs.addAll(DexScanner.methodRefs(dex));
                        } catch (IOException ex) {
                            r.report.add("risk scan: " + n + " unreadable — " + ex.getMessage());
                        }
                        try {
                            natives.putAll(NativeMethods.natives(dex));
                        } catch (IOException ex) {
                            r.report.add("natives: " + n + " unreadable (its natives get no generated stub export) — "
                                    + ex.getMessage());
                        }
                        // An authority the app names by literal (a game opening its own provider) has to follow the
                        // rename the manifest just got, or the lookup resolves to the original install.
                        boolean dexEdited = false;
                        for (Map.Entry<String, String> auth : ed.authorityRenames().entrySet()) {
                            if (auth.getKey().equals(auth.getValue())) continue;
                            int k;
                            try {
                                k = DexScanner.replaceStringPart(dex, auth.getKey(), auth.getValue());
                            } catch (IOException ex) {
                                r.report.add(n + ": authority literal: " + ex.getMessage() + " — left as it is");
                                break;
                            }
                            if (k > 0) {
                                dexEdited = true;
                                r.report.add(n + ": " + k + " string(s) follow the authority rename " + auth.getKey() +
                                        " -> " + auth.getValue());
                            }
                        }
                        if (dexEdited) {
                            try {
                                DexScanner.refreshDigests(dex);  // ART refuses a DEX whose checksum no longer matches
                            } catch (IOException ex) {
                                r.report.add(n + ": digests: " + ex.getMessage());
                            }
                        }
                        putBytes(zo, n, dex, e.getMethod() == ZipEntry.STORED ? ZipEntry.STORED : ZipEntry.DEFLATED);
                        continue;
                    }
                    if (r.renamedFrom != null && n.equals(ARSC)) {
                        byte[] arsc;
                        arsc = readFully(zip, e);
                        int k = renameArscPackage(arsc, r.renamedFrom, r.packageName);
                        r.report.add(ARSC + ": " + (k == 0 ? "no package named " + r.renamedFrom + ", left unchanged"
                                : "package name -> " + r.packageName));
                        putBytes(zo, n, arsc, ZipEntry.STORED);  // API 30+ installs only a STORED, aligned one
                        continue;
                    }
                    if (n.startsWith("assets/") && e.getSize() >= 52 && e.getSize() < (64L << 20)) {
                        // A library shipped as an asset and dlopen'd from files/ (a protection engine) gets the same
                        // section-header repair as lib/; any other asset is copied byte for byte.
                        byte[] data;
                        data = readFully(zip, e);
                        byte[] fixed = ElfRepair.repair(data);
                        if (fixed != data) r.report.add("elf: " + n + " — e_shentsize/e_shstrndx restored (zeroed by a protection wrapper)");
                        putBytes(zo, n, fixed, e.getMethod() == ZipEntry.STORED ? ZipEntry.STORED : ZipEntry.DEFLATED);
                        continue;
                    }
                    copy(zip, e, zo);
                }
                // M6 mods: files the game does not have. New assets are added; a new res/ file is not (nothing in
                // resources.arsc would point at it), and is reported.
                for (Map.Entry<String, Mod> added : modFor.entrySet()) {
                    String path = added.getKey();
                    if (names.contains(path)) continue;
                    if (!path.startsWith("assets/")) {
                        r.report.add("mod " + added.getValue().id + ": skipped " + path + " (only existing res/ files can be replaced)");
                        continue;
                    }
                    claim(names, path);
                    putBytes(zo, path, added.getValue().read(path), ZipEntry.DEFLATED);
                    r.report.add("mod " + added.getValue().id + ": added " + path);
                }
                r.risks = plan.riskReport(refs, addShims ? plan.shims : Collections.<String>emptyList());
                r.report.addAll(r.risks);
                if (nativeControl) {
                    r.report.add("native control: 32-bit libraries kept in lib/, no RetroBug runtime — runs natively on a 32-bit-capable device only");
                } else if (guest.isEmpty()) throw refused("no 32-bit ARM native libs (lib/armeabi-v7a or lib/armeabi)");
                if (!sigDropped.isEmpty())
                    r.report.add("dropped " + sigDropped.size() + " META-INF entries (old signature no longer valid): "
                            + sigDropped);

                List<String> stubs = new ArrayList<>();
                for (ZipEntry e : guest) {
                    String name = e.getName().substring(guestDir.length());
                    r.guestLibs.add(name);
                    String dest = GUEST_DIR + name;
                    claim(names, dest);
                    byte[] lib;
                    lib = readFully(zip, e);
                    byte[] fixed = ElfRepair.repair(lib);  // zeroed header fields hide the JNI exports from the stub writer
                    if (fixed != lib) {
                        r.report.add("elf: " + name + " — e_shentsize/e_shstrndx restored (zeroed by a protection wrapper)");
                        lib = fixed;
                    }
                    if (patched.containsKey(name)) lib = patched.get(name);  // M6 patches (already repaired, then edited)
                    putBytes(zo, dest, lib, ZipEntry.DEFLATED);
                    libBytes.put(name, lib);
                    // Only .so files are loaded by name through the linker; anything else (gdbserver, data
                    // blobs) is carried along for the guest but gets no stub the platform would try to load.
                    if (name.endsWith(".so") && name.indexOf('/') < 0) stubs.add(name);
                    else r.report.add(name + ": no stub (not a top-level .so); kept only under " + GUEST_DIR);
                }
                Collections.sort(stubs);
                for (String s : stubs) {
                    claim(names, ARM64 + s);
                    putBytes(zo, ARM64 + s, stubFor(s, libBytes.get(s), natives, stub, r), ZipEntry.DEFLATED);
                }
                r.stubs = stubs;

                if (addShims) {
                    // The next free classesN.dex: the platform loads classes.dex, classes2.dex, ... up to the
                    // first gap, so the shim's must continue the run.
                    String dexName = "classes.dex";
                    for (int i = 2; names.contains(dexName); i++) dexName = "classes" + i + ".dex";
                    claim(names, dexName);
                    putBytes(zo, dexName, shimDex, ZipEntry.DEFLATED);
                    claim(names, ShimPlan.CONFIG_ENTRY);
                    byte[] cert = runtimeFiles.get(ShimPlan.LAUNCHER_CERT_KEY);
                    String config = plan.config()
                            + (cert == null ? "" : "launcherCert=" + new String(cert, StandardCharsets.UTF_8).trim() + "\n");
                    putBytes(zo, ShimPlan.CONFIG_ENTRY, config.getBytes(StandardCharsets.UTF_8), ZipEntry.DEFLATED);
                    r.shims = new ArrayList<>(plan.shims);
                    r.shimDex = dexName;
                    r.report.add("shims: " + plan.shims + " (in-process targetSdk " + plan.originalTarget + ") — provider "
                            + ShimPlan.PROVIDER_CLASS + " in " + dexName);
                }

                for (Map.Entry<String, byte[]> f : runtimeFiles.entrySet()) {
                    if (nativeControl && (f.getKey().startsWith("lib/") || f.getKey().startsWith("assets/rb32"))) continue;
                    if (f.getKey().equals(STUB_KEY) || f.getKey().equals(ShimPlan.SHIM_DEX_KEY)
                            || f.getKey().equals(ShimPlan.LAUNCHER_CERT_KEY)) continue;
                    claim(names, f.getKey());
                    putBytes(zo, f.getKey(), f.getValue(), ZipEntry.DEFLATED);
                }
            }
        }
        return r;
    }

    /**
     * The 64-bit stand-in for {@code lib}: a generated stub (StubElfWriter) exporting every {@code Java_*} name the
     * library defines that a DEX native carries, so ART binds them by its own lookup; or the prebuilt stub (which
     * only boots the library) when the library exports none and no {@code JNI_OnLoad}, or cannot be read.
     */
    static byte[] stubFor(String libName, byte[] lib, Map<String, NativeMethods.Native> natives, byte[] prebuilt,
                          Result r) {
        List<String> report = r.report;
        Set<String> defined;
        try {
            defined = StubElfWriter.jniExportsOf(lib);
        } catch (IOException ex) {
            report.add(libName + ": prebuilt stub (its exports are unreadable: " + ex.getMessage() + ")");
            return prebuilt;
        }
        List<StubElfWriter.Export> exports = new ArrayList<>();
        int unbound = 0;
        for (String sym : defined) {
            if (sym.equals(StubElfWriter.ONLOAD)) continue;
            NativeMethods.Native n = natives.get(sym);
            if (n == null) { unbound++; continue; }  // no Java declaration names it: ART never looks it up
            exports.add(new StubElfWriter.Export(sym, NativeMethods.shortyOf(n.descriptor), n.isStatic));
        }
        if (exports.isEmpty() && !defined.contains(StubElfWriter.ONLOAD)) {
            report.add(libName + ": prebuilt stub (no Java_* the DEX declares, no JNI_OnLoad)");
            return prebuilt;
        }
        report.add(libName + ": generated stub, " + exports.size() + " Java_* export(s) + JNI_OnLoad"
                + (unbound == 0 ? "" : "; " + unbound + " Java_* with no native declaration in the DEX left out"));
        List<String> names = new ArrayList<>();
        names.add(StubElfWriter.ONLOAD);
        for (StubElfWriter.Export e : exports) names.add(e.symbol);
        r.stubExports.put(libName, names);
        return StubElfWriter.write(libName, exports);
    }

    /**
     * Renames, in place, every top-level ResTable_package chunk of {@code arsc} named {@code old} to {@code neu}
     * (the name is a fixed char16[128] field, so nothing moves). Returns how many were renamed; a table it cannot
     * walk is left as it is (0). Layout: frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h.
     */
    static int renameArscPackage(byte[] arsc, String old, String neu) throws IOException {
        if (neu.length() > 127) throw refused("package name too long for resources.arsc: " + neu);
        if (arsc.length < 12 || le16(arsc, 0) != 0x0002) return 0;
        int n = 0;
        for (int pos = le16(arsc, 2); pos + 8 <= arsc.length; ) {
            int type = le16(arsc, pos), size = le32(arsc, pos + 4);
            if (size < 8 || size > arsc.length - pos) break;
            if (type == 0x0200 && size >= 12 + 256) {
                StringBuilder name = new StringBuilder();
                for (int i = 0; i < 128 && le16(arsc, pos + 12 + 2 * i) != 0; i++) name.append((char) le16(arsc, pos + 12 + 2 * i));
                if (name.toString().equals(old)) {
                    for (int i = 0; i < 128; i++) {
                        int ch = i < neu.length() ? neu.charAt(i) : 0;
                        arsc[pos + 12 + 2 * i] = (byte) ch;
                        arsc[pos + 13 + 2 * i] = (byte) (ch >> 8);
                    }
                    n++;
                }
            }
            pos += size;
        }
        return n;
    }

    private static int le16(byte[] b, int o) { return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8; }
    private static int le32(byte[] b, int o) { return le16(b, o) | le16(b, o + 2) << 16; }

    private static void claim(Set<String> names, String n) throws IOException {
        if (!names.add(n)) throw refused("duplicate output entry " + n);
    }

    // Fixed timestamp: the same input and runtime must give byte-identical output.
    private static void stamp(ZipEntry e) {
        // Calendar fields in the default zone: ZipEntry stores local DOS time, so this is 2008-01-01 00:00 in every zone.
        e.setTime(new GregorianCalendar(2008, 0, 1, 0, 0, 0).getTimeInMillis());
    }

    /** An entry's bytes in one array of exactly its size when the directory gives it (a 56 MB libil2cpp.so is then
     *  56 MB of heap, not the 3x a growing buffer needs on its way there). */
    private static byte[] readFully(ZipFile zip, ZipEntry e) throws IOException {
        try (InputStream in = zip.getInputStream(e)) {
            long size = e.getSize();
            if (size < 0 || size > Integer.MAX_VALUE - 8) return readFully(in);
            byte[] data = new byte[(int) size];
            int at = 0;
            for (int k; at < data.length && (k = in.read(data, at, data.length - at)) > 0; ) at += k;
            if (at != data.length || in.read() >= 0) throw new IOException(e.getName() + ": size differs from the zip directory");
            return data;
        }
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

    /** True when any classesN.dex references a registerReceiver method (Context, ContextWrapper, Activity...). */
    static boolean registersReceivers(ZipFile zip) {
        for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements(); ) {
            ZipEntry e = en.nextElement();
            if (!e.getName().matches("classes\\d*\\.dex")) continue;
            try {
                for (String ref : DexScanner.methodRefs(readFully(zip, e))) if (ref.contains("->registerReceiver")) return true;
            } catch (IOException ex) {
                // unreadable dex: the risk scan reports it later
            }
        }
        return false;
    }

    /**
     * The old JAR/APK signature files: META-INF/MANIFEST.MF, *.SF, *.RSA, *.DSA, *.EC and SIG-* at the top of META-INF.
     * Everything else there stays — META-INF/services/* is how libraries find their plug-ins at runtime (ServiceLoader).
     */
    static boolean isSignatureFile(String n) {
        if (!n.startsWith("META-INF/") || n.indexOf('/', "META-INF/".length()) >= 0) return false;
        String f = n.substring("META-INF/".length()).toUpperCase(java.util.Locale.ROOT);
        return f.equals("MANIFEST.MF") || f.endsWith(".SF") || f.endsWith(".RSA") || f.endsWith(".DSA") || f.endsWith(".EC")
                || f.startsWith("SIG-");
    }

    /**
     * Copies an entry keeping its compression method, except resources.arsc, which is always STORED: a package
     * targeting API 30+ is refused unless its resources.arsc is uncompressed and 4-byte aligned (RepackSigner aligns
     * STORED entries), and some games ship it compressed. Streamed: an asset can be larger than the heap (Among Us
     * ships a 172 MB bundle); a STORED entry takes its size and CRC from the source's directory.
     */
    private static void copy(ZipFile zip, ZipEntry src, ZipOutputStream zo) throws IOException {
        final boolean stored = src.getMethod() == ZipEntry.STORED || src.getName().equals(ARSC);
        if (stored && (src.getMethod() != ZipEntry.STORED || src.getSize() < 0 || src.getCrc() < 0)) {
            byte[] data;  // a compressed resources.arsc (small): its CRC is computed from the bytes
            data = readFully(zip, src);
            putBytes(zo, src.getName(), data, ZipEntry.STORED);
            return;
        }
        ZipEntry e = new ZipEntry(src.getName());
        e.setMethod(stored ? ZipEntry.STORED : ZipEntry.DEFLATED);
        if (stored) { e.setSize(src.getSize()); e.setCompressedSize(src.getSize()); e.setCrc(src.getCrc()); }
        stamp(e);
        zo.putNextEntry(e);
        byte[] buf = new byte[1 << 16];
        try (InputStream in = zip.getInputStream(src)) {
            for (int k; (k = in.read(buf)) > 0; ) zo.write(buf, 0, k);
        }
        zo.closeEntry();
    }
}
