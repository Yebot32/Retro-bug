// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// DEX fixture with no risky platform call (the "must not report" side of the risk-scan rows).
package com.retrobug.fixture;

public class CleanFixture {
    public static String greet(String who) { return "hello " + who.trim(); }
}
