// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The tests' name for the shared table-driven Java. The implementation moved to `rb::java::TableJava`
// (core/host/java/table_java.cpp) when the iOS port needed the same thing as a runtime shim (S1); this
// alias keeps the core lanes' spelling, so a test that defines classes and bodies still reads the same.
#pragma once

#include "rb/java/table_java.h"

namespace rb::test {

using FakeJava = rb::java::TableJava;

}  // namespace rb::test
