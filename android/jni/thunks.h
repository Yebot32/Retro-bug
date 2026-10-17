// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The thunk tables (M4): for each library a guest imports from (libEGL, libGLESv2, libGLESv1_CM, libandroid,
// libjnigraphics), every entry point the generic caller (`rb::abi::ThunkCaller`, core/abi/thunk.h) can forward
// from its signature alone. Calls a signature cannot express (strings returned, pointer arrays, out-arrays of
// host pointers, JNI objects) are hand-written in HostSlots and not listed here, so a name in neither stays a
// named trap.
#pragma once

#include <string>

namespace rb::launcher {

struct ThunkEntry {
  const char* lib;   ///< the device library that exports it ("libGLESv2.so", ...)
  const char* spec;  ///< "<ret>:<args>" in ThunkCaller's letters
};

/// The table entry for `name`, or nullptr. A GLES 1.x name the 2.0 table also has resolves to the 2.0 entry: both
/// libraries dispatch through the calling thread's current context, so either reaches the right driver.
const ThunkEntry* FindThunk(const std::string& name);

/// `name` in `lib` on this device (dlopen'd once, RTLD_NOW), falling back to eglGetProcAddress for GL extension
/// entry points; nullptr when neither has it.
void* HostSymbol(const char* lib, const std::string& name);

}  // namespace rb::launcher
