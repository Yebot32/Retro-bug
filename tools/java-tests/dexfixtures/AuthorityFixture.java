// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// DEX fixture whose code names a provider authority by literal, as a game opening its own provider does
// (Rolling Sky's ServicePreferencesHelper). The repack has to rename that literal with the manifest's authority.
package com.retrobug.fixture;

public class AuthorityFixture {
    public static final String IPC = "content://com.retrobug.fixture.ipc_provider";

    public static String uri() { return IPC; }
}
