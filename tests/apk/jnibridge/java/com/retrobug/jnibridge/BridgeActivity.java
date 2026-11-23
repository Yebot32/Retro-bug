// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The JNI bridge test app (M3 Task 6). Each row drives libjnibridge.so (tests/apk/jnibridge/jni/bridge_app.c) and
// logs `BRIDGE: <row>=<value>`, then `BRIDGE: done <n> rows`. Run natively on a 32-bit-capable device the values
// are the oracle (tools/run_bridge_oracle.sh); repacked, the same 32-bit code runs through RetroBug's JNI bridge
// and every row must print the same value (tools/run_bridge_check.sh). A row that throws prints its exception.
package com.retrobug.jnibridge;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

public class BridgeActivity extends Activity {
    static final String TAG = "BRIDGE";

    static { System.loadLibrary("jnibridge"); }

    // ---- natives, all in libjnibridge.so
    static native String strings(String a, String b);
    static native String arrays(int[] ints, double[] doubles, Object[] objects);
    static native int callbacks(int depth);
    static native String exceptions();
    static native void makeGlobals(Object o);
    static native String checkGlobals();
    static native String weakCleared();
    static native int add(int a, int b);
    static native String fields(Holder h);
    static native String over(int x);
    static native String over(String s);

    /** Its one native is bound by the library's JNI_OnLoad (RegisterNatives), not by name. */
    static final class Registered {
        static native int nativeAdd(int a, int b);
    }

    /**
     * Gumball's JNIGLSurface shape: instance natives whose C saves `this` in a static on entry and never restores
     * it, and a Java callback (loadTextureImage) that calls a native back (jniTextureReady), so the outer native
     * goes on with the nested one's `this`, kept past that native's return.
     */
    static final class Surface {
        native String outer(String name);
        native void inner(String name);
        void load(String name) { inner(name); }
        String label() { return "surface"; }
    }

    static final class Holder {
        int i = 7;
        long j = 1L << 40;
        double d = 0.5;
        String s = "held";
        static int counter = 3;
        static String label = "static";
    }

    // ---- Java called back from native code
    static int up(int depth) { return depth == 0 ? 1 : 10 * callbacks(depth - 1) + depth; }
    static void boom() { throw new IllegalStateException("boom"); }

    static String hash(String s) {
        int h = 0x811c9dc5;
        for (int i = 0; i < s.length(); i++) h = (h ^ s.charAt(i)) * 0x01000193;
        return s.length() + ":" + Integer.toHexString(h);
    }

    private int rows = 0;

    private void row(String name, Row r) {
        String v;
        try {
            v = r.run();
        } catch (Throwable t) {
            v = "THREW " + t;
        }
        rows++;
        Log.i(TAG, name + "=" + v);
    }

    interface Row { String run() throws Exception; }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        new Thread(new Runnable() {
            @Override public void run() { runAll(); }
        }, "bridge-rows").start();
    }

    void runAll() {
        row("strings", new Row() { public String run() {
            StringBuilder big = new StringBuilder();
            for (int i = 0; i < 65536; i++) big.append((char) (1 + (i * 7919) % 0xd7ff));
            String a = "a\u0000b😀é", s = big.toString();
            return strings(a, s) + " java " + hash(a) + " " + hash(s);
        }});
        row("arrays", new Row() { public String run() {
            int[] ints = {1, 2, 3, 4};
            double[] ds = {0.25, 0.5};
            Object[] os = {"x", "y"};
            String n = arrays(ints, ds, os);
            return n + " ints " + ints[0] + "," + ints[1] + "," + ints[2] + "," + ints[3] + " doubles " + ds[0] + ","
                    + ds[1] + " objects " + os[0] + "," + os[1];
        }});
        row("callbacks", new Row() { public String run() { return String.valueOf(callbacks(3)); }});
        row("exceptions", new Row() { public String run() {
            try {
                return "no throw: " + exceptions();
            } catch (IllegalArgumentException e) {
                return e.getClass().getName() + ": " + e.getMessage();
            }
        }});
        row("globals", new Row() { public String run() throws Exception {
            makeGlobals(new StringBuilder("referent"));
            for (int i = 0; i < 3; i++) { Runtime.getRuntime().gc(); Thread.sleep(50); }
            String alive = checkGlobals();  // deletes the global: the weak one is all that is left
            String cleared = "no";
            for (int i = 0; i < 20 && cleared.equals("no"); i++) {
                Runtime.getRuntime().gc();
                Thread.sleep(50);
                cleared = weakCleared();
            }
            return alive + " weak cleared after GC: " + cleared;
        }});
        row("threads", new Row() { public String run() throws Exception {
            final long[] sums = new long[8];
            Thread[] ts = new Thread[8];
            for (int t = 0; t < 8; t++) {
                final int id = t;
                ts[t] = new Thread(new Runnable() {
                    @Override public void run() {
                        long s = 0;
                        for (int k = 0; k < 100; k++) s += add(id * 1000, k);
                        sums[id] = s;
                    }
                });
                ts[t].start();
            }
            long total = 0;
            for (int t = 0; t < 8; t++) { ts[t].join(); total += sums[t]; }
            return String.valueOf(total);
        }});
        row("register_natives", new Row() { public String run() { return String.valueOf(Registered.nativeAdd(2, 3)); }});
        row("fields_and_statics", new Row() { public String run() {
            Holder h = new Holder();
            String n = fields(h);
            return n + " after i=" + h.i + " j=" + h.j + " d=" + h.d + " s=" + h.s + " counter=" + Holder.counter
                    + " label=" + Holder.label;
        }});
        row("overloads", new Row() { public String run() { return over(41) + " " + over("s"); }});
        row("nested_this", new Row() { public String run() { return new Surface().outer("font.png"); }});
        Log.i(TAG, "done " + rows + " rows");
    }
}
