// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See thunks.h. The signature table moved to core/gfx (shared with the iOS port); this file is the Android
// symbol source it resolves names against.

#include "thunks.h"

#include <EGL/egl.h>
#include <dlfcn.h>

#include <map>
#include <mutex>

namespace rb::launcher {

void* HostSymbol(const char* lib, const std::string& name) {
  static std::mutex mu;
  static std::map<std::string, void*> libs;
  void* h = nullptr;
  {
    std::lock_guard<std::mutex> lk(mu);
    auto it = libs.find(lib);
    if (it == libs.end()) it = libs.emplace(lib, dlopen(lib, RTLD_NOW | RTLD_LOCAL)).first;
    h = it->second;
  }
  void* fn = h != nullptr ? dlsym(h, name.c_str()) : nullptr;
  if (fn == nullptr && name.rfind("gl", 0) == 0) fn = reinterpret_cast<void*>(eglGetProcAddress(name.c_str()));
  return fn;
}

}  // namespace rb::launcher
