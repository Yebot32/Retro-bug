// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// DEX fixture for NativeMethods: natives of every naming case JNI mangling has — an underscore, arrays and
// references in the long name, an overload (long names only), a nested class ($), a non-ASCII name, static and
// instance. Never loaded or run.
package rb.fx;

public class NativeFixture {
    static native void a();
    native int b(int x, long y);
    static native String c_d(String s, int[] arr, Object[][] o);
    native void over(int x);
    native void over(String s);
    static native void café();
    native void prims(int i, long j, float f, double d, boolean z, byte b, char c, short s);
    void notNative() {}

    public class Inner {
        native boolean z();
    }
}
