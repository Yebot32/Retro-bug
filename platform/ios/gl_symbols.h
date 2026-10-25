// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The framework's GLES entry point for `name` (gl_symbols.mm).
#pragma once

#include <string>

namespace rb::platform::ios {

/// OpenGLES.framework is linked into the app, so its exported entry points are in the process image;
/// dlsym(RTLD_DEFAULT) is how the guest's gl* imports find them on iOS (there is nothing to dlopen).
void* GlSymbol(const std::string& name);

}  // namespace rb::platform::ios
