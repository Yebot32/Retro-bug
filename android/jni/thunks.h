// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The Android symbol source for the shared thunk table (M4). The table itself — which entry points forward
// and with what signature — lives in `rb::gfx` (core/gfx/thunk_table.cpp) so the iOS port answers the same
// names; this file is the part that is Android: dlopen the device's driver, then eglGetProcAddress for the
// extensions it does not export.
#pragma once

#include <string>

#include "rb/gfx/thunk_table.h"

namespace rb::launcher {

using rb::gfx::FindThunk;
using rb::gfx::ThunkEntry;

/// `name` in `lib` on this device (dlopen'd once, RTLD_NOW), falling back to eglGetProcAddress for GL extension
/// entry points; nullptr when neither has it.
void* HostSymbol(const char* lib, const std::string& name);

}  // namespace rb::launcher
