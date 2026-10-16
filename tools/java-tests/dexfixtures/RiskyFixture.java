// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// DEX fixture for DexScanner / ShimPlan.riskReport: compiled against android.jar and dexed with d8 by
// tools/run_java_tests.sh, never run. Each call is one a pre-24 game makes and a targetSdk shim covers.
package com.retrobug.fixture;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;

import java.io.File;
import java.net.Socket;

public class RiskyFixture {
    public static void billing(Context c) {
        c.bindService(new Intent("com.android.vending.billing.MarketBillingService.BIND"), null, Context.BIND_AUTO_CREATE);
    }
    public static Uri share(File f) { return Uri.fromFile(f); }
    public static void ping(Socket s) throws Exception { s.connect(null); }
}
