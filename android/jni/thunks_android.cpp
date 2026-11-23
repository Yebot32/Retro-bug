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
#include <android/sensor.h>

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace rb::launcher {

namespace {

std::string Hex(std::uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%x", v);
  return b;
}

struct SensorCallback {
  HostSlots& slots;
  abi::ForwardingJni& jni;
  exec::GuestAddr function, data;
  bool active = true;
  bool reported = false;
};

// A looper may already have queued a callback when its sensor queue is destroyed. Pass Android an
// opaque ID so a late delivery can look it up without dereferencing a released object.
struct SensorCallbacks {
  std::mutex mutex;
  std::uintptr_t next = 1;
  std::map<std::uintptr_t, std::shared_ptr<SensorCallback>> callbacks;
};

SensorCallbacks& SensorDispatch() {
  static auto* dispatch = new SensorCallbacks();
  return *dispatch;
}

int DispatchSensorEvent(int fd, int events, void* opaque) {
  auto& dispatch = SensorDispatch();
  std::shared_ptr<SensorCallback> callback;
  {
    std::lock_guard<std::mutex> lock(dispatch.mutex);
    const auto it = dispatch.callbacks.find(reinterpret_cast<std::uintptr_t>(opaque));
    if (it == dispatch.callbacks.end()) return 0;
    callback = it->second;
  }
  auto& process = callback->slots.process();
  runtime::Ggl::Held hold(process.ggl());
  if (!callback->active) return 0;
  callback->jni.PushFrame();
  exec::RunResult result{};
  const auto value = process.abi().CallGuestToCompletion(
      callback->function, {abi::Value32::S32(fd), abi::Value32::S32(events), abi::Value32::Ptr(callback->data)},
      abi::Shorty::For("I"), runtime::GuestProcess::kThreadTurnBudget, &result);
  callback->jni.PopFrame(0);
  if (result.reason != exec::StopReason::Returned) {
    callback->slots.Live("sensors: guest event callback " + Hex(callback->function) + " stopped: " + result.detail);
    callback->active = false;
  } else if (!callback->reported) {
    callback->slots.Live("sensors: delivered guest event callback " + Hex(callback->function));
    callback->reported = true;
  }
  if (value.w() == 0) callback->active = false;
  return callback->active ? 1 : 0;
}

struct State {
  HostSlots& slots;
  abi::ForwardingJni& fwd;
  JavaVM* vm;
  std::map<void*, std::pair<std::uint32_t, std::uint32_t>> buffers;  ///< AAsset* -> guest copy (addr, mapped size)
  std::map<void*, std::uint32_t> dir_names;                           ///< AAssetDir* -> 1 KiB guest name buffer
  struct SensorQueue {
    ASensorManager* manager;
    std::uintptr_t callback;
  };
  std::map<ASensorEventQueue*, SensorQueue> sensor_queues;
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

  static_assert(sizeof(ASensorEvent) == 104, "ASensorEvent: the guest's layout is passed in place");
  slots.AddSpecial("ASensorManager_createEventQueue", [st](exec::MachineState& s, std::string* why) {
    void* manager = nullptr;
    void* looper = nullptr;
    if (!st->Handle(s.r[0], &manager, "ASensorManager", why)) return false;
    if (!st->Handle(s.r[1], &looper, "ALooper", why)) return false;
    std::uint32_t data = 0;
    if (!st->mem().Read(s.r[13], &data, sizeof(data))) {
      *why = "sensor event queue data argument is not readable";
      return false;
    }
    auto& dispatch = SensorDispatch();
    std::uintptr_t cookie = 0;
    if (s.r[3]) {
      const auto callback = std::make_shared<SensorCallback>(SensorCallback{st->slots, st->fwd, s.r[3], data});
      std::lock_guard<std::mutex> lock(dispatch.mutex);
      cookie = dispatch.next++;
      dispatch.callbacks.emplace(cookie, callback);
    }
    auto* sensor_manager = static_cast<ASensorManager*>(manager);
    auto* queue = ASensorManager_createEventQueue(
        sensor_manager, static_cast<ALooper*>(looper), static_cast<int>(s.r[2]), cookie ? DispatchSensorEvent : nullptr,
        reinterpret_cast<void*>(cookie ? cookie : static_cast<std::uintptr_t>(data)));
    if (queue) {
      st->sensor_queues.emplace(queue, State::SensorQueue{sensor_manager, cookie});
      if (cookie) st->slots.Live("sensors: registered guest event callback " + Hex(s.r[3]));
    } else if (cookie) {
      std::lock_guard<std::mutex> lock(dispatch.mutex);
      dispatch.callbacks.at(cookie)->active = false;
      dispatch.callbacks.erase(cookie);
    }
    s.r[0] = queue ? st->slots.handles().Put(queue) : 0;
    return true;
  });
  slots.AddSpecial("ASensorManager_destroyEventQueue", [st](exec::MachineState& s, std::string* why) {
    void* manager = nullptr;
    void* handle = nullptr;
    if (!st->Handle(s.r[0], &manager, "ASensorManager", why) ||
        !st->Handle(s.r[1], &handle, "ASensorEventQueue", why)) return false;
    auto* queue = static_cast<ASensorEventQueue*>(handle);
    const auto it = st->sensor_queues.find(queue);
    if (it == st->sensor_queues.end() || it->second.manager != manager) {
      *why = "sensor event queue does not belong to this manager";
      return false;
    }
    const auto result = ASensorManager_destroyEventQueue(it->second.manager, queue);
    if (result == 0) {
      if (it->second.callback) {
        auto& dispatch = SensorDispatch();
        std::lock_guard<std::mutex> lock(dispatch.mutex);
        dispatch.callbacks.at(it->second.callback)->active = false;
        dispatch.callbacks.erase(it->second.callback);
      }
      st->sensor_queues.erase(it);
      st->slots.handles().Forget(queue);
    }
    s.r[0] = static_cast<std::uint32_t>(result);
    return true;
  });

  // ASensorManager_getSensorList(manager, &list): the host answers an `ASensorList` — an array of `ASensor const*`
  // it owns — through an out-pointer, and the guest can only hold 4-byte handles. So the host's array is copied
  // once into a guest array of handles and *list points at that. The guest array is never freed: a sensor list is
  // static for the process's lifetime, and a second call would hand back the same handles from the handle table.
  slots.AddSpecial("ASensorManager_getSensorList", [st](exec::MachineState& s, std::string* why) {
    void* manager = nullptr;
    if (s.r[0] != 0 && !st->Handle(s.r[0], &manager, "ASensorManager", why)) return false;
    ASensorList host_list = nullptr;
    const int n = ASensorManager_getSensorList(static_cast<ASensorManager*>(manager), &host_list);
    std::uint32_t guest_list = 0;
    if (n > 0 && host_list != nullptr) {
      std::uint32_t mapped = 0;
      guest_list = st->Alloc(4u * static_cast<std::size_t>(n), &mapped);
      if (guest_list == 0) {
        *why = "ASensorManager_getSensorList: no guest memory for " + std::to_string(n) + " sensor handles";
        return false;
      }
      for (int i = 0; i < n; ++i) {
        const std::uint32_t h = st->slots.handles().Put(const_cast<ASensor*>(host_list[i]));
        if (!st->mem().Write(guest_list + 4u * static_cast<std::uint32_t>(i), &h, 4)) {
          *why = "ASensorManager_getSensorList: the guest handle array is not writable";
          return false;
        }
      }
    }
    if (s.r[1] != 0 && !st->mem().Write(s.r[1], &guest_list, 4)) {
      *why = "ASensorManager_getSensorList: the list out-pointer is not writable";
      return false;
    }
    s.r[0] = static_cast<std::uint32_t>(n);
    return true;
  });
  // ASensor_getName / ASensor_getVendor return a host `const char*` the guest would dereference as a guest address.
  // One copy per call, like glGetString: the strings are short, static and asked for once per sensor.
  const auto sensor_string = [st](const char* what, const char* (*get)(ASensor const*)) {
    return [st, what, get](exec::MachineState& s, std::string* why) {
      void* a = nullptr;
      if (!st->Handle(s.r[0], &a, "ASensor", why)) return false;
      const char* text = get(static_cast<ASensor const*>(a));
      if (text == nullptr) {
        s.r[0] = 0;  // NULL is the device's own answer for a sensor it cannot describe
        return true;
      }
      const std::string copy = text;
      std::uint32_t mapped = 0;
      const std::uint32_t at = st->Alloc(copy.size() + 1, &mapped);
      if (at == 0 || !st->mem().Write(at, copy.c_str(), copy.size() + 1)) {
        *why = std::string{what} + ": no guest memory for the string";
        return false;
      }
      s.r[0] = at;
      return true;
    };
  };
  slots.AddSpecial("ASensor_getName", sensor_string("ASensor_getName", ASensor_getName));
  slots.AddSpecial("ASensor_getVendor", sensor_string("ASensor_getVendor", ASensor_getVendor));

  // ---- ART's JNI "potentially blocking region" hooks. Unity's libunity and libil2cpp reference both *weakly* and
  // call them on JNI transitions, so with nothing answering them the reference is null — and a call through it
  // stops the process on an instruction fetch at 0x0, naming no symbol at all (measured: Angry Birds Epic,
  // 2026-10-18, "nativeRender()Z stopped: fault: instruction fetch failed at 0x0", lr inside libunity). On a
  // device they resolve to ART's own bookkeeping, which has no meaning here: there is no ART monitor watching a
  // guest's JNI calls to mark a thread as about to block. They answer as the no-ops they are to a guest.
  for (const char* name : {"__google_potentially_blocking_region_begin", "__google_potentially_blocking_region_end"}) {
    slots.AddSpecial(name, [](exec::MachineState& s, std::string*) {
      s.r[0] = 0;
      return true;
    });
  }

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
