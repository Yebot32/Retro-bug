// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
// A game that registers a broadcast receiver the pre-Android-13 way (no RECEIVER_EXPORTED/NOT_EXPORTED flag): at
// targetSdk 34+ that call throws SecurityException, so the converter keeps such a copy at target 33.
package rb;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;

public class ReceiverFixture {
    public static void listen(Context c) {
        c.registerReceiver(new BroadcastReceiver() {
            @Override public void onReceive(Context x, Intent i) {}
        }, new IntentFilter("rb.fixture.PING"));
    }
}
