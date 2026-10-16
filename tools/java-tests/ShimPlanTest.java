// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for ShimPlan (which targetSdk shims a game gets, the provider's config, the risk report) and
// DexScanner (method references out of a real d8-built DEX). Each decision is checked on both sides of the
// platform gate it mirrors, so a wrong threshold fails a row whichever way it is wrong.
//
// args: <fixture dir holding RiskyFixture.dex and CleanFixture.dex>
package com.retrobug.launcher;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;

public class ShimPlanTest {
    static int checks = 0, failures = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static boolean has(List<String> lines, String a, String b) {
        for (String l : lines) if (l.contains(a) && l.contains(b)) return true;
        return false;
    }

    static void plans() {
        check(ShimPlan.effectiveTarget(null, 8) == 8, "absent target -> minSdk");
        check(ShimPlan.effectiveTarget(null, null) == 1, "absent both -> 1");
        check(ShimPlan.effectiveTarget(22, 8) == 22, "declared target wins");
        List<String> s8 = ShimPlan.of(8, 24).shims;
        check(s8.equals(Arrays.asList("target-sdk", "strict-network", "file-uri", "permissions")), "target 8: " + s8);
        check(ShimPlan.of(10, 24).shims.contains("strict-network"), "10 < 11: strict-network");
        check(!ShimPlan.of(11, 24).shims.contains("strict-network"), "11: no strict-network");
        check(ShimPlan.of(22, 24).shims.contains("permissions"), "22 < 23: permissions");
        check(!ShimPlan.of(23, 24).shims.contains("permissions"), "23: no permissions");
        check(ShimPlan.of(23, 24).shims.equals(Arrays.asList("target-sdk", "file-uri")), "23: " + ShimPlan.of(23, 24).shims);
        check(ShimPlan.of(24, 24).shims.isEmpty(), "24 -> 24: none");
        check(ShimPlan.of(30, 30).shims.isEmpty(), "30 left at 30: none");
        check(ShimPlan.of(8, 24).config().equals("targetSdk=8\nshims=target-sdk,strict-network,file-uri,permissions\n"),
                "config: " + ShimPlan.of(8, 24).config());
    }

    static Set<String> refs(String... r) { return new TreeSet<>(Arrays.asList(r)); }

    /** The report when every shim of the plan was added. */
    static List<String> report(int original, int target, Set<String> refs) {
        ShimPlan p = ShimPlan.of(original, target);
        return p.riskReport(refs, p.shims);
    }

    static void risks() {
        Set<String> bind = refs("Lcom/game/Billing;->bindService", "Landroid/content/Context;->unbindService");
        List<String> l = report(8, 24, bind);
        check(l.size() == 1 && has(l, "implicit service", "covered by shim target-sdk"), "bindService at 8: " + l);
        check(!has(l, "unbindService", ""), "unbindService is not bindService: " + l);
        List<String> none = ShimPlan.of(8, 24).riskReport(bind, java.util.Collections.<String>emptyList());
        check(has(none, "implicit service", "NOT covered (shim target-sdk not added)"), "shims not added: " + none);
        check(report(21, 24, bind).isEmpty(), "bindService at 21: already past the gate");
        check(report(30, 30, bind).isEmpty(), "no raise: nothing reported");
        Set<String> net = refs("Ljava/net/URL;->openConnection");
        check(has(report(10, 24, net), "NetworkOnMainThread", "strict-network"), "network at 10 reported");
        check(report(11, 24, net).isEmpty(), "network at 11: not reported");
        List<String> gone = report(22, 24, refs("Landroid/app/Notification;->setLatestEventInfo"));
        check(has(gone, "NoSuchMethodError", "NOT covered (no shim)"), "removed API: " + gone);
        check(report(8, 24, refs("Ljava/lang/String;->trim")).isEmpty(), "harmless refs: nothing");
    }

    static void dex(File fixtures) throws IOException {
        Set<String> risky = DexScanner.methodRefs(Files.readAllBytes(new File(fixtures, "RiskyFixture.dex").toPath()));
        check(risky.contains("Landroid/content/Context;->bindService"), "risky dex: bindService ref: " + risky);
        check(risky.contains("Landroid/net/Uri;->fromFile"), "risky dex: Uri.fromFile ref");
        check(risky.contains("Ljava/net/Socket;->connect"), "risky dex: Socket.connect ref");
        check(risky.contains("Lcom/retrobug/fixture/RiskyFixture;->billing"), "risky dex: its own method");
        List<String> l = report(8, 24, risky);
        check(has(l, "implicit service", "target-sdk") && has(l, "FileUriExposed", "file-uri")
                && has(l, "NetworkOnMainThread", "strict-network"), "risky dex report: " + l);
        Set<String> clean = DexScanner.methodRefs(Files.readAllBytes(new File(fixtures, "CleanFixture.dex").toPath()));
        check(clean.contains("Ljava/lang/String;->trim"), "clean dex: String.trim ref: " + clean);
        check(report(8, 24, clean).isEmpty(), "clean dex report empty");
        try {
            DexScanner.methodRefs("PK\3\4 not a dex at all, long enough to pass the size check ............................................".getBytes("UTF-8"));
            check(false, "non-DEX bytes should be refused");
        } catch (IOException e) {
            check(e.getMessage().contains("not a DEX"), "non-DEX message: " + e.getMessage());
        }
        byte[] cut = Arrays.copyOf(Files.readAllBytes(new File(fixtures, "RiskyFixture.dex").toPath()), 0x200);
        try { DexScanner.methodRefs(cut); check(false, "truncated DEX should be refused"); }
        catch (IOException e) { check(true, "truncated DEX refused"); }
    }

    public static void main(String[] a) throws Exception {
        plans();
        risks();
        dex(new File(a[0]));
        System.out.println("ShimPlanTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
