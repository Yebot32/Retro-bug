// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The shared import→signature table: see core/gfx/thunk_table.cpp.
#pragma once

#include <string>

namespace rb::gfx {

struct ThunkEntry {
  const char* lib;  ///< the device library that exports it ("libGLESv2.so", ...)
  const char* spec; ///< "<ret>:<args>" in rb::abi::ThunkCaller's letters
};

/// The table entry for `name`, or nullptr. A GLES 1.x name the 2.0 table also has resolves to the 2.0 entry: both
/// libraries dispatch through the calling thread's current context, so either reaches the right backend.
const ThunkEntry* FindThunk(const std::string& name);

}  // namespace rb::gfx
