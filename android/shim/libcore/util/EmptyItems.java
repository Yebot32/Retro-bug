// RetroBug contributors. SPDX-License-Identifier: Apache-2.0
package libcore.util;

/** Empty arrays used by the bundled legacy ArrayMap implementation. */
public final class EmptyItems {
    private EmptyItems() {}

    public static final int[] INT = new int[0];
    public static final Object[] OBJECT = new Object[0];
}
