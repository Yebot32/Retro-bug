// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "native_activity.h"

#include <android/configuration.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/native_activity.h>

#include <cstdlib>
#include <map>
#include <memory>
#include <string_view>
#include <type_traits>

#include "host_slots.h"
#include "rb/abi/jni_forward.h"
#include "rb/host/boot.h"
#include "thunks.h"

namespace rb::launcher {
namespace {
using abi::Value32;
using exec::GuestAddr;
using runtime::Ggl;

// ANativeActivity and its callback table from the A32 NDK ABI. All fields are four bytes.
struct Activity32 {
  GuestAddr callbacks, vm, env, activity, internal_path, external_path;
  std::int32_t sdk;
  GuestAddr instance, assets, obb_path;
};
static_assert(sizeof(Activity32) == 40);
enum Callback : unsigned {
  Start, Resume, Save, Pause, Stop, Destroy, Focus, WindowCreated, WindowResized, WindowRedraw,
  WindowDestroyed, QueueCreated, QueueDestroyed, ContentRect, Configuration, LowMemory, CallbackCount,
};
static_assert(sizeof(ANativeActivityCallbacks) == CallbackCount * sizeof(void*));
constexpr std::uint32_t kScratchOffset = sizeof(Activity32) + CallbackCount * sizeof(GuestAddr);
constexpr std::size_t kMaxSavedState = 1024 * 1024;

// One handle per live framework activity. Access is serialized by the guest lock.
std::map<GuestAddr, ANativeActivity*>& Activities() {
  static auto* activities = new std::map<GuestAddr, ANativeActivity*>;
  return *activities;
}

struct Activity {
  HostSlots& slots;
  host::Boot& boot;
  ANativeActivity* host;
  GuestAddr address = 0;
  GuestAddr java_object = 0;
  bool failed = false;

  Activity(HostSlots& slots, host::Boot& boot, ANativeActivity* host) : slots(slots), boot(boot), host(host) {}

  runtime::GuestProcess& process() { return slots.process(); }
  mem::GuestMemory& memory() { return process().memory(); }
  GuestAddr scratch() const { return address + kScratchOffset; }

  ~Activity() {
    Ggl::Held hold(process().ggl());
    if (java_object) boot.jni()->ReleaseGuestGlobal(java_object);
    if (address) {
      Activities().erase(address);
      std::string ignored;
      memory().Unmap(address, mem::kGuestPage, &ignored);
    }
  }

  std::uint32_t Call(GuestAddr fn, const char* name, abi::Args32 args, const char* shorty = "V") {
    if (!fn || failed) return 0;
    boot.jni()->PushFrame();
    exec::RunResult result;
    const auto value = process().abi().CallGuest(fn, args, abi::Shorty::For(shorty),
                                                runtime::GuestProcess::kInitBudget, &result);
    boot.jni()->PopFrame(0);
    if (result.reason == exec::StopReason::Returned) return value.w();
    failed = true;
    boot.Live(std::string{"native activity: "} + name + " stopped: " + result.detail);
    ANativeActivity_finish(host);
    return 0;
  }

  std::uint32_t Notify(Callback callback, const char* name, std::optional<Value32> argument = {},
                       const char* shorty = "V") {
    Ggl::Held hold(process().ggl());
    GuestAddr fn = 0;
    if (!memory().Read(address + sizeof(Activity32) + callback * sizeof(fn), &fn, sizeof(fn))) return 0;
    std::vector<Value32> args{Value32::Ptr(address)};
    if (argument) args.push_back(*argument);
    return Call(fn, name, abi::Args32{args}, shorty);
  }
};

Activity& Of(ANativeActivity* activity) { return *static_cast<Activity*>(activity->instance); }

template <Callback callback, typename T>
void NotifyHandle(ANativeActivity* host, T* object) {
  auto& a = Of(host);
  a.Notify(callback, callback <= WindowDestroyed ? "window callback" : "input queue callback",
           Value32::U32(a.slots.handles().Put(object)));
  if constexpr (callback == WindowDestroyed || callback == QueueDestroyed) a.slots.handles().Forget(object);
}

ANativeActivityCallbacks Callbacks() {
  ANativeActivityCallbacks c{};
  c.onStart = [](ANativeActivity* a) { Of(a).Notify(Start, "onStart"); };
  c.onResume = [](ANativeActivity* a) { Of(a).Notify(Resume, "onResume"); };
  c.onPause = [](ANativeActivity* a) { Of(a).Notify(Pause, "onPause"); };
  c.onStop = [](ANativeActivity* a) { Of(a).Notify(Stop, "onStop"); };
  c.onDestroy = [](ANativeActivity* a) {
    std::unique_ptr<Activity> owned(&Of(a));
    owned->Notify(Destroy, "onDestroy");
    a->instance = nullptr;
  };
  c.onWindowFocusChanged = [](ANativeActivity* a, int focused) {
    Of(a).Notify(Focus, "onWindowFocusChanged", Value32::S32(focused));
  };
  c.onNativeWindowCreated = &NotifyHandle<WindowCreated, ANativeWindow>;
  c.onNativeWindowResized = &NotifyHandle<WindowResized, ANativeWindow>;
  c.onNativeWindowRedrawNeeded = &NotifyHandle<WindowRedraw, ANativeWindow>;
  c.onNativeWindowDestroyed = &NotifyHandle<WindowDestroyed, ANativeWindow>;
  c.onInputQueueCreated = &NotifyHandle<QueueCreated, AInputQueue>;
  c.onInputQueueDestroyed = &NotifyHandle<QueueDestroyed, AInputQueue>;
  c.onContentRectChanged = [](ANativeActivity* host, const ARect* rect) {
    auto& a = Of(host);
    Ggl::Held hold(a.process().ggl());
    if (a.memory().Write(a.scratch(), rect, sizeof(*rect))) {
      a.Notify(ContentRect, "onContentRectChanged", Value32::Ptr(a.scratch()));
    }
  };
  c.onConfigurationChanged = [](ANativeActivity* a) { Of(a).Notify(Configuration, "onConfigurationChanged"); };
  c.onLowMemory = [](ANativeActivity* a) { Of(a).Notify(LowMemory, "onLowMemory"); };
  c.onSaveInstanceState = [](ANativeActivity* host, std::size_t* out_size) -> void* {
    auto& a = Of(host);
    Ggl::Held hold(a.process().ggl());
    *out_size = 0;
    std::uint32_t size = 0;
    if (!a.memory().Write(a.scratch(), &size, sizeof(size))) return nullptr;
    const GuestAddr saved = a.Notify(Save, "onSaveInstanceState", Value32::Ptr(a.scratch()), "I");
    void* state = nullptr;
    if (saved && a.memory().Read(a.scratch(), &size, sizeof(size)) && size && size <= kMaxSavedState) {
      state = std::malloc(size);
      if (state && a.memory().Read(saved, state, size)) {
        *out_size = size;
      } else {
        std::free(state);
        state = nullptr;
      }
    }
    if (saved) {
      if (const auto free = a.process().libc()->FindExport("free")) {
        a.Call(free->tagged(), "free saved state", {Value32::Ptr(saved)});
      }
    }
    return state;
  };
  return c;
}

}  // namespace

void StartNativeActivity(HostSlots& slots, host::Boot& boot, GuestAddr entry,
                         ANativeActivity* host, const void* saved_state, std::size_t saved_size) {
  auto a = std::make_unique<Activity>(slots, boot, host);
  Ggl::Held hold(a->process().ggl());
  const auto fail = [&](const std::string& why) {
    boot.Live("native activity: could not start: " + why);
    ANativeActivity_finish(host);
  };
  std::string err;
  a->address = a->process().MapFree(mem::kGuestPage, mem::kRead | mem::kWrite, &err);
  if (!a->address) return fail(err);
  a->java_object = boot.jni()->ToGuestGlobal(reinterpret_cast<abi::HostRef>(host->clazz), &err);
  if (!a->java_object) return fail(err);
  const auto path = [&](const char* name, const char* value) {
    return value ? slots.GuestString(value) : GuestAddr{0};
  };
  const Activity32 guest{a->address + static_cast<GuestAddr>(sizeof(Activity32)), boot.jni()->vm(), boot.jni()->env(),
                         a->java_object,
                         path("nativeActivity.internalDataPath", host->internalDataPath),
                         path("nativeActivity.externalDataPath", host->externalDataPath), host->sdkVersion, 0,
                         slots.handles().Put(host->assetManager), path("nativeActivity.obbPath", host->obbPath)};
  if (!a->memory().Write(a->address, &guest, sizeof(guest))) return fail("could not write guest activity");
  if (saved_size > kMaxSavedState || (saved_size && !saved_state)) return fail("invalid saved state");
  GuestAddr saved = 0;
  if (saved_size) {
    saved = a->process().MapFree(saved_size, mem::kRead | mem::kWrite, &err);
    if (!saved) return fail(err);
    if (!a->memory().Write(saved, saved_state, saved_size)) {
      a->memory().Unmap(saved, saved_size, &err);
      return fail("could not copy saved state");
    }
  }
  Activities().emplace(a->address, host);
  host->instance = a.release();
  *host->callbacks = Callbacks();
  boot.Live("native activity: calling guest ANativeActivity_onCreate");
  auto& activity = Of(host);
  activity.Call(entry, "ANativeActivity_onCreate",
                {Value32::Ptr(activity.address), Value32::Ptr(saved), Value32::U32(saved_size)});
  if (saved) activity.memory().Unmap(saved, saved_size, &err);
}

void RegisterNativeActivityThunks(HostSlots& slots) {
  const auto activity = [](GuestAddr address) -> ANativeActivity* {
    const auto it = Activities().find(address);
    return it == Activities().end() ? nullptr : it->second;
  };
  const auto add = [&](const char* name, auto fn) {
    slots.AddSpecial(name, [activity, fn](exec::MachineState& s, std::string* why) {
      auto* host = activity(s.r[0]);
      if (!host) { *why = "unknown guest ANativeActivity"; return false; }
      fn(host, s);
      return true;
    });
  };
  add("ANativeActivity_finish", [](auto* a, const auto&) { ANativeActivity_finish(a); });
  add("ANativeActivity_setWindowFormat", [](auto* a, const auto& s) { ANativeActivity_setWindowFormat(a, s.r[1]); });
  add("ANativeActivity_setWindowFlags", [](auto* a, const auto& s) { ANativeActivity_setWindowFlags(a, s.r[1], s.r[2]); });
  add("ANativeActivity_showSoftInput", [](auto* a, const auto& s) { ANativeActivity_showSoftInput(a, s.r[1]); });
  add("ANativeActivity_hideSoftInput", [](auto* a, const auto& s) { ANativeActivity_hideSoftInput(a, s.r[1]); });

  auto& handles = slots.handles();
  auto& process = slots.process();
  auto& memory = process.memory();
  const auto get = [&handles](GuestAddr address, auto*& object, std::string* why) {
    void* host = nullptr;
    if (!handles.Get(address, &host) || !host) {
      *why = "invalid native object handle";
      return false;
    }
    object = static_cast<std::remove_reference_t<decltype(object)>>(host);
    return true;
  };
  // Java-created threads can already own an Android looper. Only poll a looper RetroBug prepared
  // for a guest thread, so the game's polling does not dispatch system UI events.
  static thread_local ALooper* guest_looper = nullptr;
  slots.AddSpecial("ALooper_prepare", [&handles](exec::MachineState& s, std::string*) {
    if (!ALooper_forThread()) guest_looper = ALooper_prepare(s.r[0]);
    s.r[0] = handles.Put(ALooper_forThread());
    return true;
  });
  slots.AddSpecial("ALooper_forThread", [&handles](exec::MachineState& s, std::string*) {
    s.r[0] = handles.Put(ALooper_forThread());
    return true;
  });
  slots.AddSpecial("ALooper_wake", [get](exec::MachineState& s, std::string* why) {
    if (!s.r[0]) return true;  // a Java-driven game may have no native looper
    ALooper* looper = nullptr;
    if (!get(s.r[0], looper, why)) return false;
    ALooper_wake(looper);
    return true;
  });
  for (bool all : {false, true}) {
    slots.AddSpecial(all ? "ALooper_pollAll" : "ALooper_pollOnce",
                     [&process, &memory, all](exec::MachineState& s, std::string* why) {
      int result = ALOOPER_POLL_TIMEOUT, fd = 0, events = 0;
      void* data = nullptr;
      if (guest_looper) {
        Ggl::Released released(process.ggl());
        do { result = ALooper_pollOnce(static_cast<int>(s.r[0]), &fd, &events, &data); }
        while (all && result == ALOOPER_POLL_CALLBACK);
      }
      const auto address = static_cast<GuestAddr>(reinterpret_cast<std::uintptr_t>(data));
      if (result >= 0 && ((s.r[1] && !memory.Write(s.r[1], &fd, sizeof(fd))) ||
                          (s.r[2] && !memory.Write(s.r[2], &events, sizeof(events))) ||
                          (s.r[3] && !memory.Write(s.r[3], &address, sizeof(address))))) {
        *why = "unwritable ALooper poll result";
        return false;
      }
      s.r[0] = static_cast<std::uint32_t>(result);
      return true;
    });
  }
  slots.AddSpecial("AInputQueue_attachLooper", [get, &memory](exec::MachineState& s, std::string* why) {
    AInputQueue* queue = nullptr;
    ALooper* looper = nullptr;
    if (!get(s.r[0], queue, why) || !get(s.r[1], looper, why)) return false;
    if (s.r[3]) { *why = "guest ALooper callback is not supported; use a poll identifier"; return false; }
    std::uint32_t data = 0;  // fifth AAPCS32 argument
    // The data points at the guest's poll source. The host looper only stores and returns it.
    if (!memory.Read(s.r[13], &data, sizeof(data))) {
      *why = "unreadable AInputQueue_attachLooper data argument";
      return false;
    }
    AInputQueue_attachLooper(queue, looper, s.r[2], nullptr,
                            reinterpret_cast<void*>(static_cast<std::uintptr_t>(data)));
    return true;
  });
  slots.AddSpecial("AInputQueue_getEvent", [get, &handles, &memory](exec::MachineState& s, std::string* why) {
    AInputQueue* queue = nullptr;
    if (!get(s.r[0], queue, why)) return false;
    AInputEvent* event = nullptr;
    const int result = AInputQueue_getEvent(queue, &event);
    if (result >= 0) {
      const auto handle = handles.Put(event);
      if (!memory.Write(s.r[1], &handle, sizeof(handle))) {
        AInputQueue_finishEvent(queue, event, 0);
        handles.Forget(event);
        *why = "unwritable AInputEvent result";
        return false;
      }
    }
    s.r[0] = static_cast<std::uint32_t>(result);
    return true;
  });
  slots.AddSpecial("AInputQueue_preDispatchEvent", [get, &handles](exec::MachineState& s, std::string* why) {
    AInputQueue* queue = nullptr;
    AInputEvent* event = nullptr;
    if (!get(s.r[0], queue, why) || !get(s.r[1], event, why)) return false;
    s.r[0] = AInputQueue_preDispatchEvent(queue, event);
    if (s.r[0]) handles.Forget(event);
    return true;
  });
  slots.AddSpecial("AInputQueue_finishEvent", [get, &handles](exec::MachineState& s, std::string* why) {
    AInputQueue* queue = nullptr;
    AInputEvent* event = nullptr;
    if (!get(s.r[0], queue, why) || !get(s.r[1], event, why)) return false;
    AInputQueue_finishEvent(queue, event, s.r[2]);
    handles.Forget(event);
    return true;
  });
  slots.AddSpecial("AConfiguration_delete", [get, &handles](exec::MachineState& s, std::string* why) {
    AConfiguration* config = nullptr;
    if (!get(s.r[0], config, why)) return false;
    handles.Forget(config);
    AConfiguration_delete(config);
    return true;
  });
  const std::pair<const char*, const char*> functions[] = {
      {"AConfiguration_new", "H:"}, {"AConfiguration_fromAssetManager", "v:hh"},
      {"AInputQueue_detachLooper", "v:h"}, {"AInputQueue_hasEvents", "i:h"},
      {"AInputEvent_getType", "i:h"}, {"AInputEvent_getDeviceId", "i:h"}, {"AInputEvent_getSource", "i:h"},
      {"AMotionEvent_getAction", "i:h"}, {"AMotionEvent_getFlags", "i:h"}, {"AMotionEvent_getMetaState", "i:h"},
      {"AMotionEvent_getEdgeFlags", "i:h"}, {"AMotionEvent_getPointerCount", "u:h"},
      {"AMotionEvent_getPointerId", "i:hu"}, {"AMotionEvent_getDownTime", "l:h"},
      {"AMotionEvent_getEventTime", "l:h"}, {"AMotionEvent_getXPrecision", "f:h"}, {"AMotionEvent_getYPrecision", "f:h"},
      {"AMotionEvent_getX", "f:hu"}, {"AMotionEvent_getY", "f:hu"}, {"AMotionEvent_getSize", "f:hu"},
      {"AMotionEvent_getPressure", "f:hu"}, {"AMotionEvent_getOrientation", "f:hu"},
      {"AMotionEvent_getToolMajor", "f:hu"}, {"AMotionEvent_getToolMinor", "f:hu"},
      {"AMotionEvent_getTouchMajor", "f:hu"}, {"AMotionEvent_getTouchMinor", "f:hu"},
      {"AMotionEvent_getHistorySize", "u:h"}, {"AMotionEvent_getHistoricalEventTime", "l:hu"},
      {"AMotionEvent_getHistoricalPressure", "f:huu"}, {"AMotionEvent_getHistoricalSize", "f:huu"},
      {"AMotionEvent_getHistoricalX", "f:huu"}, {"AMotionEvent_getHistoricalY", "f:huu"},
      {"AMotionEvent_getHistoricalRawX", "f:huu"}, {"AMotionEvent_getHistoricalRawY", "f:huu"},
      {"AMotionEvent_getHistoricalTouchMajor", "f:huu"}, {"AMotionEvent_getHistoricalTouchMinor", "f:huu"},
      {"AMotionEvent_getHistoricalToolMajor", "f:huu"}, {"AMotionEvent_getHistoricalToolMinor", "f:huu"},
      {"AMotionEvent_getHistoricalOrientation", "f:huu"},
  };
  for (const auto& [name, signature] : functions) {
    if (void* fn = HostSymbol("libandroid.so", name)) {
      const auto forward = [&handles, &memory, signature, fn](exec::MachineState& s, std::string* why) {
        return abi::ThunkCaller::Call(signature, fn, s, memory, handles, why);
      };
      slots.AddSpecial(name, forward);
      const std::string_view symbol{name};
      // Old Unity also looks up the original A32 C++ names for these now-public NDK functions.
      if (symbol.starts_with("AMotionEvent_getHistorical") && std::string_view{signature} == "f:huu") {
        slots.AddSpecial("_Z" + std::to_string(symbol.size()) + name + "P11AInputEventjj", forward);
      }
    }
  }
}
}  // namespace rb::launcher
