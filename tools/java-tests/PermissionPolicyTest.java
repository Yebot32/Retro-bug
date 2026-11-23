// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Host tests for PermissionPolicy: of the dangerous permissions an old game declares and does not hold, the copy asks
// only for the ones a game plays with (microphone, camera). Ads/analytics permissions (location, phone, contacts,
// accounts, SMS, calendar) are not asked for, and storage cannot be granted at a modern target anyway.
package com.retrobug.shim;

import java.util.Arrays;
import java.util.Collections;
import java.util.List;

public class PermissionPolicyTest {
    static int checks = 0, failures = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    public static void main(String[] a) {
        List<String> declared = Arrays.asList(
                "android.permission.ACCESS_FINE_LOCATION", "android.permission.ACCESS_COARSE_LOCATION",
                "android.permission.READ_PHONE_STATE", "android.permission.CALL_PHONE", "android.permission.GET_ACCOUNTS",
                "android.permission.READ_CONTACTS", "android.permission.SEND_SMS", "android.permission.READ_CALENDAR",
                "android.permission.WRITE_EXTERNAL_STORAGE", "android.permission.READ_EXTERNAL_STORAGE",
                "android.permission.POST_NOTIFICATIONS", "android.permission.ACCESS_BACKGROUND_LOCATION",
                "android.permission.RECORD_AUDIO", "android.permission.CAMERA");
        List<String> ask = PermissionPolicy.worthAsking(declared, 35);
        check(ask.equals(Arrays.asList("android.permission.RECORD_AUDIO", "android.permission.CAMERA")),
                "only microphone and camera are asked for: " + ask);
        check(PermissionPolicy.worthAsking(Collections.<String>emptyList(), 35).isEmpty(), "nothing declared: nothing asked");
        check(PermissionPolicy.worthAsking(Arrays.asList("android.permission.WRITE_EXTERNAL_STORAGE"), 28)
                .equals(Arrays.asList("android.permission.WRITE_EXTERNAL_STORAGE")),
                "storage is still worth asking at a target where it can be granted (< 30)");
        check(PermissionPolicy.worthAsking(Arrays.asList("android.permission.READ_EXTERNAL_STORAGE"), 33).isEmpty(),
                "storage at 33+: never granted, not asked");
        check(PermissionPolicy.worthAsking(Arrays.asList("com.vendor.permission.CUSTOM_DANGEROUS"), 35).isEmpty(),
                "unknown dangerous permissions are not asked for");
        System.out.println("PermissionPolicyTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
