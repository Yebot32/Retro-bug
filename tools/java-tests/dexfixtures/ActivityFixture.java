// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// DEX fixture for the NativeActivity walk (C31): launcher activities as real games declare them — a plain Java one, one
// that inherits from a base class in the same DEX, and chains that reach android.app.NativeActivity the way Prime31's
// and Unity's do (com.prime31.UnityPlayerNativeActivity -> com.unity3d.player.UnityPlayerNativeActivity -> NativeActivity).
// Compiled against android.jar and dexed with d8 by tools/run_java_tests.sh, never loaded or run.
package rb.fx;

import android.app.Activity;
import android.app.NativeActivity;

public class ActivityFixture {
    public static class Plain extends Activity {}

    public static class Base extends Activity {}

    public static class Derived extends Base {}

    public static class Native extends NativeActivity {}

    public static class NativeDerived extends Native {}

    /** Neither a NativeActivity nor a class the DEX defines: the walk has to stop without answering. */
    public static class Unrelated {}

    private ActivityFixture() {}
}
