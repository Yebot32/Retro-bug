// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Which missing dangerous permissions a converted old game is asked about at its first activity. A game built before
// Android 6 got every declared permission at install; asking for all of them again meant three to five dialogs per
// game, nearly all for ads and analytics (location, phone state, accounts, contacts, SMS) that players refuse and that
// gameplay does not need (refused device IDs already read as null). So only what games play with is asked for: the
// microphone and the camera; and storage while the target still lets it be granted (below 30). Pure Java: host tested.
package com.retrobug.shim;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

final class PermissionPolicy {
    private PermissionPolicy() {}

    static final List<String> PLAY = Arrays.asList("android.permission.RECORD_AUDIO", "android.permission.CAMERA");
    static final List<String> STORAGE = Arrays.asList("android.permission.READ_EXTERNAL_STORAGE",
            "android.permission.WRITE_EXTERNAL_STORAGE");

    /** The permissions among `missing` worth a dialog, in their declared order, for a copy running at `target`. */
    static List<String> worthAsking(List<String> missing, int target) {
        List<String> out = new ArrayList<>();
        for (String p : missing) {
            if (PLAY.contains(p) || (STORAGE.contains(p) && target < 30)) out.add(p);
        }
        return out;
    }
}
