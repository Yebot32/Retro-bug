// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See thunks_android.h. Every special runs with the GGL held (HostSlots binds them kHoldsGgl), which also guards
// the per-object state kept here.
#include "thunks_android.h"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/bitmap.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>

namespace rb::launcher {

namespace {

std::string Hex(std::uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%x", v);
  return b;
}

struct State {
  HostSlots& slots;
  abi::ForwardingJni& fwd;
  JavaVM* vm;
  std::map<void*, std::pair<std::uint32_t, std::uint32_t>> buffers;  ///< AAsset* -> guest copy (addr, mapped size)
  std::map<void*, std::uint32_t> dir_names;                           ///< AAssetDir* -> 1 KiB guest name buffer
  struct Lock {
    std::uint32_t guest = 0, mapped = 0;
    void* host = nullptr;
    std::size_t bytes = 0;
  };
  std::map<std::uint32_t, Lock> bitmaps;  ///< guest bitmap handle -> its locked pixels' guest shadow

  State(HostSlots& s, abi::ForwardingJni& f, JavaVM* v) : slots(s), fwd(f), vm(v) {}

  runtime::GuestProcess& p() { return slots.process(); }
  mem::GuestMemory& mem() { return slots.process().memory(); }

  JNIEnv* Env(std::string* why) {
    JNIEnv* env = nullptr;
    if (vm != nullptr && vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (vm != nullptr && vm->AttachCurrentThread(&env, nullptr) == JNI_OK) return env;
    *why = "no JNIEnv on this thread";
    return nullptr;
  }
  /// The guest's handle for a Java object, as the calling thread's real jobject.
  bool Object(std::uint32_t handle, jobject* out, std::string* why) {
    std::string e;
    const abi::HostRef r = fwd.FromGuest(handle, &e);
    if (!e.empty()) {
      *why = "Java object " + Hex(handle) + ": " + e;
      return false;
    }
    *out = reinterpret_cast<jobject>(r);
    return true;
  }
  bool Handle(std::uint32_t h, void** out, const char* what, std::string* why) {
    if (!slots.handles().Get(h, out) || *out == nullptr) {
      *why = std::string{what} + " " + Hex(h) + " was never issued (or is closed)";
      return false;
    }
    return true;
  }
  /// `bytes` of fresh guest memory (page-rounded), 0 when none is left.
  std::uint32_t Alloc(std::size_t bytes, std::uint32_t* mapped) {
    *mapped = static_cast<std::uint32_t>((bytes + mem::kGuestPage - 1) & ~std::size_t{mem::kGuestPage - 1});
    if (*mapped == 0) *mapped = mem::kGuestPage;
    std::string e;
    return p().MapFree(*mapped, mem::kRead | mem::kWrite, &e);
  }
  void Free(std::uint32_t at, std::uint32_t mapped) {
    std::string e;
    if (at != 0) mem().Unmap(at, mapped, &e);
  }
};

}  // namespace

void RegisterAndroidThunks(HostSlots& slots, abi::ForwardingJni& fwd, JavaVM* vm) {
  auto st = std::make_shared<State>(slots, fwd, vm);

  // ---- assets
  slots.AddSpecial("AAssetManager_fromJava", [st](exec::MachineState& s, std::string* why) {
    JNIEnv* env = st->Env(why);
    jobject am = nullptr;
    if (env == nullptr || !st->Object(s.r[1], &am, why)) return false;
    AAssetManager* m = AAssetManager_fromJava(env, am);
    s.r[0] = st->slots.handles().Put(m);
    return true;
  });
  slots.AddSpecial("AAsset_getBuffer", [st](exec::MachineState& s, std::string* why) {
    void* a = nullptr;
    if (!st->Handle(s.r[0], &a, "AAsset", why)) return false;
    if (const auto it = st->buffers.find(a); it != st->buffers.end()) {
      s.r[0] = it->second.first;
      return true;
    }
    auto* asset = static_cast<AAsset*>(a);
    const void* host = AAsset_getBuffer(asset);
    const off64_t len = AAsset_getLength64(asset);
    if (host == nullptr || len < 0 || len > (off64_t{1} << 30)) {
      s.r[0] = 0;
      return true;  // NULL is the device's own answer when the asset cannot be mapped
    }
    std::uint32_t mapped = 0;
    const std::uint32_t at = st->Alloc(static_cast<std::size_t>(len), &mapped);
    if (at == 0 || (len != 0 && !st->mem().Write(at, host, static_cast<std::size_t>(len)))) {
      *why = "AAsset_getBuffer: no guest memory for a " + std::to_string(len) + "-byte copy";
      return false;
    }
    st->buffers[a] = {at, mapped};
    s.r[0] = at;
    return true;
  });
  slots.AddSpecial("AAsset_close", [st](exec::MachineState& s, std::string* why) {
    void* a = nullptr;
    if (!st->Handle(s.r[0], &a, "AAsset", why)) return false;
    if (const auto it = st->buffers.find(a); it != st->buffers.end()) {
      st->Free(it->second.first, it->second.second);
      st->buffers.erase(it);
    }
    AAsset_close(static_cast<AAsset*>(a));
    st->slots.handles().Forget(a);
    return true;
  });
  // off_t is 32-bit for the guest, 64-bit on the host: the out-parameters are written as the guest's width.
  const auto open_fd = [st](bool wide) {
    return [st, wide](exec::MachineState& s, std::string* why) {
      void* a = nullptr;
      if (!st->Handle(s.r[0], &a, "AAsset", why)) return false;
      off64_t start = 0, length = 0;
      const int fd = AAsset_openFileDescriptor64(static_cast<AAsset*>(a), &start, &length);
      if (wide) {
        if (s.r[1] != 0) st->mem().Write(s.r[1], &start, 8);
        if (s.r[2] != 0) st->mem().Write(s.r[2], &length, 8);
      } else {
        const std::int32_t st32 = static_cast<std::int32_t>(start), len32 = static_cast<std::int32_t>(length);
        if (s.r[1] != 0) st->mem().Write(s.r[1], &st32, 4);
        if (s.r[2] != 0) st->mem().Write(s.r[2], &len32, 4);
      }
      s.r[0] = static_cast<std::uint32_t>(fd);
      return true;
    };
  };
  slots.AddSpecial("AAsset_openFileDescriptor", open_fd(false));
  slots.AddSpecial("AAsset_openFileDescriptor64", open_fd(true));
  slots.AddSpecial("AAssetDir_getNextFileName", [st](exec::MachineState& s, std::string* why) {
    void* d = nullptr;
    if (!st->Handle(s.r[0], &d, "AAssetDir", why)) return false;
    const char* name = AAssetDir_getNextFileName(static_cast<AAssetDir*>(d));
    if (name == nullptr) {
      s.r[0] = 0;
      return true;
    }
    std::uint32_t& buf = st->dir_names[d];
    std::uint32_t mapped = 0;
    if (buf == 0) buf = st->Alloc(1024, &mapped);
    const std::string n = std::string{name}.substr(0, 1023);
    if (buf == 0 || !st->mem().Write(buf, n.c_str(), n.size() + 1)) {
      *why = "AAssetDir_getNextFileName: no guest memory for the name";
      return false;
    }
    s.r[0] = buf;
    return true;
  });
  slots.AddSpecial("AAssetDir_close", [st](exec::MachineState& s, std::string* why) {
    void* d = nullptr;
    if (!st->Handle(s.r[0], &d, "AAssetDir", why)) return false;
    if (const auto it = st->dir_names.find(d); it != st->dir_names.end()) {
      st->Free(it->second, 1024 <= mem::kGuestPage ? mem::kGuestPage : 1024);
      st->dir_names.erase(it);
    }
    AAssetDir_close(static_cast<AAssetDir*>(d));
    st->slots.handles().Forget(d);
    return true;
  });

  // ---- native windows
  slots.AddSpecial("ANativeWindow_fromSurface", [st](exec::MachineState& s, std::string* why) {
    JNIEnv* env = st->Env(why);
    jobject surface = nullptr;
    if (env == nullptr || !st->Object(s.r[1], &surface, why)) return false;
    s.r[0] = st->slots.handles().Put(ANativeWindow_fromSurface(env, surface));
    return true;
  });

  // ---- jnigraphics: the pixels are host memory; the guest works on a shadow copied in at lock, out at unlock.
  slots.AddSpecial("AndroidBitmap_getInfo", [st](exec::MachineState& s, std::string* why) {
    JNIEnv* env = st->Env(why);
    jobject bmp = nullptr;
    if (env == nullptr || !st->Object(s.r[1], &bmp, why)) return false;
    AndroidBitmapInfo info{};
    const int r = AndroidBitmap_getInfo(env, bmp, &info);
    if (r == ANDROID_BITMAP_RESULT_SUCCESS && s.r[2] != 0) st->mem().Write(s.r[2], &info, sizeof info);
    s.r[0] = static_cast<std::uint32_t>(r);
    return true;
  });
  slots.AddSpecial("AndroidBitmap_lockPixels", [st](exec::MachineState& s, std::string* why) {
    JNIEnv* env = st->Env(why);
    jobject bmp = nullptr;
    if (env == nullptr || !st->Object(s.r[1], &bmp, why)) return false;
    AndroidBitmapInfo info{};
    void* host = nullptr;
    int r = AndroidBitmap_getInfo(env, bmp, &info);
    if (r == ANDROID_BITMAP_RESULT_SUCCESS) r = AndroidBitmap_lockPixels(env, bmp, &host);
    if (r != ANDROID_BITMAP_RESULT_SUCCESS) {
      s.r[0] = static_cast<std::uint32_t>(r);
      return true;
    }
    State::Lock l;
    l.host = host;
    l.bytes = static_cast<std::size_t>(info.stride) * info.height;
    l.guest = st->Alloc(l.bytes, &l.mapped);
    if (l.guest == 0 || !st->mem().Write(l.guest, host, l.bytes)) {
      AndroidBitmap_unlockPixels(env, bmp);
      *why = "AndroidBitmap_lockPixels: no guest memory for a " + std::to_string(l.bytes) + "-byte shadow";
      return false;
    }
    st->bitmaps[s.r[1]] = l;
    if (s.r[2] != 0) st->mem().Write(s.r[2], &l.guest, 4);
    s.r[0] = ANDROID_BITMAP_RESULT_SUCCESS;
    return true;
  });
  slots.AddSpecial("AndroidBitmap_unlockPixels", [st](exec::MachineState& s, std::string* why) {
    JNIEnv* env = st->Env(why);
    jobject bmp = nullptr;
    if (env == nullptr || !st->Object(s.r[1], &bmp, why)) return false;
    const auto it = st->bitmaps.find(s.r[1]);
    if (it == st->bitmaps.end()) {
      *why = "AndroidBitmap_unlockPixels: bitmap " + Hex(s.r[1]) + " was not locked through this handle";
      return false;
    }
    st->mem().Read(it->second.guest, it->second.host, it->second.bytes);  // the guest's writes go back
    st->Free(it->second.guest, it->second.mapped);
    st->bitmaps.erase(it);
    s.r[0] = static_cast<std::uint32_t>(AndroidBitmap_unlockPixels(env, bmp));
    return true;
  });
}

}  // namespace rb::launcher
