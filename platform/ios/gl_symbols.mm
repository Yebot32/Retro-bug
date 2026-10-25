// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The framework's GLES entry points on iOS: see gl_symbols.h.

#include "gl_symbols.h"

#include <dlfcn.h>

#import <OpenGLES/ES1/gl.h>

namespace rb::platform::ios {

void* GlSymbol(const std::string& name) { return dlsym(RTLD_DEFAULT, name.c_str()); }

}  // namespace rb::platform::ios
