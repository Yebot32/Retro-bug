// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Halfbrick's Mortar engine (Fruit Ninja vs. Skittles and the other Mortar titles). The native library
// calls a few static methods on SoftKeyboard while it initialises — Init() first — and an absent method
// makes it abort the process (exit_group) instead of continuing. iOS has no soft keyboard, so the class
// is defined here as no-ops before any library loads.
#pragma once

#include "rb/java/table_java.h"

namespace rb::java {

void InstallHalfbrick(TableJava& java);

}  // namespace rb::java
