// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// `rb::host::Boot` — the in-process boot, platform-free. See rb/host/boot.h. The code is the Android
// runtime's (`android/jni/runtime_boot.cpp`) with the Java implementation, host import answers, platform
// services and reporting moved behind `BootConfig` callbacks.

#include "rb/host/boot.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

#include "rb/abi/jni_forward.h"
#include "rb/exec/jit.h"
#include "rb/loader/zip.h"
#include "rb/runtime/ggl.h"
#include "rb/runtime/guest_thread.h"
#include "rb/runtime/inbound.h"

namespace rb::host {

namespace {

using rb::runtime::GuestProcess;

constexpr std::uint32_t kDlModuleBase = 0xC0D00000u;  ///< | index: a library a guest dlopen loaded
constexpr std::uint32_t kDlGlobal = 0xC0D8FFFFu;      ///< dlopen(NULL) / a system library: search everything
constexpr std::uint32_t kRtldDefault = 0xFFFFFFFFu;   ///< bionic LP32 RTLD_DEFAULT
constexpr std::uint32_t kRtldNext = 0xFFFFFFFEu;      ///< bionic LP32 RTLD_NEXT

std::string Hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%x", v);
  return buf;
}

}  // namespace

Boot::Boot(BootConfig config) : cfg_(std::move(config)) {}

Boot::~Boot() {
  StopMonitor();
  inbound_.reset();
  jni_.reset();
  java_.reset();
  proc_.reset();
}

void Boot::Live(const std::string& line) {
  if (cfg_.log) cfg_.log(line);
  std::lock_guard<std::mutex> lk(report_mu_);
  if (report_ != nullptr) {
    *report_ += line;
    *report_ += '\n';
  }
}

void Boot::Say(const std::string& line) { Live(line); }

void* Boot::BindNative(rb::exec::GuestAddr fn, const std::string& name, const std::string& sig, std::string* err) {
  const std::string shorty = rb::runtime::ShortyOf(sig);
  if (shorty.empty()) {
    if (err != nullptr) *err = "'" + sig + "' is not a JNI method signature";
    return nullptr;
  }
  if (!inbound_) {
    if (err != nullptr) *err = "the JNI bridge is not up";
    return nullptr;
  }
  return inbound_->BindPool(rb::runtime::InboundTarget{fn, shorty, name + sig}, err);
}

bool Boot::BootBridge(std::string* out) {
  if (!cfg_.make_java) {
    Say("FATAL: rb_runtime_boot: no Java implementation was provided, so the JNI bridge cannot reach Java");
    return false;
  }
  GuestProcess& p = *proc_;
  std::string err;
  rb::exec::GuestAddr page = 0;
  {
    rb::runtime::Ggl::Held hold(p.ggl());
    page = p.MapFree(rb::mem::kGuestPage, rb::mem::kRead | rb::mem::kWrite, &err);
  }
  if (page == 0) {
    Say("FATAL: rb_runtime_boot: no guest page for the JNI bridge: " + err);
    return false;
  }
  java_ = cfg_.make_java(&err);
  if (!java_) {
    Say("FATAL: rb_runtime_boot: " + (err.empty() ? std::string{"no Java implementation"} : err));
    return false;
  }
  jni_ = std::make_unique<rb::abi::ForwardingJni>(p.memory(), p.abi(), page, *java_, &err);
  if (!jni_->ok()) {
    Say("FATAL: rb_runtime_boot: " + err);
    return false;
  }
  jni_->set_allocator([&p](std::uint32_t bytes, std::string* e) {
    rb::runtime::Ggl::Held hold(p.ggl());
    return p.MapFree(bytes, rb::mem::kRead | rb::mem::kWrite, e);
  });
  // Java's thread APIs take a Linux tid; the guest's tids are the engine's, so `Process.setThreadPriority` on
  // a guest thread throws "Given thread N does not exist". Resolve through the thread-bound tid map instead.
  rb::abi::ForwardingJni::SetGuestTidResolver(
      [](std::int32_t guest_tid) { return rb::runtime::HostTidOfGuest(guest_tid); });
  inbound_ = std::make_unique<rb::runtime::Inbound>(p, *jni_, *java_, cfg_.ctor_tier);
  inbound_->set_log([this](const std::string& line) { Say(line); });
  // A fault report carries the platform's own state too: no other layer knows the host-call ring.
  if (cfg_.fault_state) p.set_fault_suffix(cfg_.fault_state);
  jni_->set_native_binder([this](rb::exec::GuestAddr fn, const std::string& name, const std::string& sig,
                                 std::string* e) -> void* { return BindNative(fn, name, sig, e); });
  inbound_->set_on_exit([this](int status) {
    Say("jni: a guest native called exit(" + std::to_string(status) + "); the app exits, as on a device");
    if (cfg_.on_guest_exit) cfg_.on_guest_exit(status);
    else ::_exit(status);
  });
  Say("jni: forwarding JNI bridge up — JNIEnv at " + Hex(jni_->env()) + ", JavaVM at " + Hex(jni_->vm()) +
      ", natives on Tier " + (cfg_.ctor_tier == 2 ? "J" : "I"));
  (void)out;
  return true;
}

bool Boot::BootScope(const std::string& apk, rb::loader::ZipReader& zip, std::string* out) {
  (void)apk;
  (void)out;
  rb::runtime::GuestProcessOptions o;
  o.window_bytes = cfg_.window_bytes;
  o.biased = true;
  o.unresolved_trap = cfg_.unresolved_trap;
  o.ctor_tier = cfg_.ctor_tier;
  o.enable_jit = cfg_.enable_jit;
  o.log = [this](const std::string& s) { Live(s); };
  o.host_resolver = [this](const std::string& name) -> std::optional<rb::exec::GuestAddr> {
    return cfg_.resolve_host ? cfg_.resolve_host(name) : std::nullopt;
  };
  std::string err;
  auto proc = GuestProcess::Create(o, &err);
  if (!proc) {
    Say("FATAL: " + err);
    return false;
  }
  proc_ = std::move(proc);
  if (!BootBridge(out)) {
    DropScope();
    return false;
  }
  if (cfg_.on_bridge) cfg_.on_bridge(*this, *proc_);
  if (cfg_.monitor) StartMonitor();
  std::vector<std::uint8_t> sys[3];
  const char* const names[3] = {"libc.so", "libm.so", "libz.so"};
  for (int i = 0; i < 3; ++i) {
    const std::string entry = std::string{"assets/rb32sys/"} + names[i];
    auto bytes = zip.Read(entry, &err);
    if (!bytes) {
      Say("FATAL: rb_runtime_boot: " + entry + ": " + err);
      DropScope();
      return false;
    }
    sys[i] = std::move(*bytes);
    Say("rb_runtime_boot: " + entry + " = " + std::to_string(sys[i].size()) + " bytes");
  }
  if (!proc_->LoadScope(sys[0], sys[1], sys[2], {}, &err)) {
    Say("FATAL: " + err);
    DropScope();
    return false;
  }
  Say("scope: booted once — libc.so, libm.so, libz.so in a " +
      std::to_string(proc_->memory().window_bytes() >> 20) +
      " MiB biased window, unresolved imports bound to " + Hex(cfg_.unresolved_trap) +
      "; process-lifetime, reused by every later rb_runtime_boot");
  return true;
}

const rb::loader::GuestModule* Boot::LoadWithNeeded(rb::loader::ZipReader& zip, const std::string& lib,
                                                    std::set<std::string>& in_progress, std::string* out) {
  (void)out;
  if (const auto hit = loaded_.find(lib); hit != loaded_.end()) return hit->second;
  if (!in_progress.insert(lib).second) {
    Say("FATAL: rb_runtime_boot: a DT_NEEDED cycle reaches " + lib + " again");
    return nullptr;
  }
  std::string err;
  auto image = zip.Read("assets/rb32/" + lib, &err);
  if (!image) {
    Say("FATAL: rb_runtime_boot: assets/rb32/" + lib + ": " + err);
    return nullptr;
  }
  Say("rb_runtime_boot: assets/rb32/" + lib + " = " + std::to_string(image->size()) + " bytes");
  std::string host_side;
  for (const std::string& n : rb::loader::NeededSonames(*image)) {
    if (n == "libc.so" || n == "libm.so" || n == "libz.so" || loaded_.count(n) != 0) continue;
    // libstdc++ comes in on demand, where the launcher's Inspect passes it to LoadScope: this scope is booted
    // once for every library the process will ever load, so it joins later the way a device's linker loads a
    // DT_NEEDED — after the scope, before its user, its constructors run with the user's.
    if (n == "libstdc++.so" && zip.Has("assets/rb32sys/libstdc++.so")) {
      auto cxx = zip.Read("assets/rb32sys/libstdc++.so", &err);
      touched_ = true;
      const rb::loader::GuestModule* m = cxx ? proc_->LoadApp(*cxx, n, &err) : nullptr;
      if (m == nullptr) {
        Say("FATAL: rb_runtime_boot: " + lib + " needs libstdc++.so, which did not load: " + err);
        return nullptr;
      }
      loaded_[n] = m;
      Say("needs: " + lib + " -> libstdc++.so, loaded from assets/rb32sys/ on demand");
    } else if (zip.Has("assets/rb32/" + n)) {
      Say("needs: " + lib + " -> " + n + ", a sibling in assets/rb32/, loaded first");
      if (LoadWithNeeded(zip, n, in_progress, out) == nullptr) return nullptr;
    } else {
      host_side += (host_side.empty() ? "" : ", ") + n;
    }
  }
  if (!host_side.empty()) {
    Say("needs: " + lib + " -> " + host_side + " — no image in the APK; its imports are answered by the"
        " host (dl*, liblog) or trapped by name at " + Hex(cfg_.unresolved_trap));
  }
  const std::size_t unanswered_before = proc_->unanswered().size();
  touched_ = true;
  const rb::loader::GuestModule* m = proc_->LoadApp(*image, lib, &err);
  if (m == nullptr) {
    Say("FATAL: " + err);
    return nullptr;
  }
  loaded_[lib] = m;
  const auto& un = proc_->unanswered();
  if (un.size() > unanswered_before) {
    std::string names;
    for (std::size_t i = unanswered_before; i < un.size(); ++i) names += (names.empty() ? "" : ", ") + un[i];
    Say("imports: " + lib + ": " + std::to_string(un.size() - unanswered_before) +
        " nothing answers, each bound to its own trap slot from " + Hex(cfg_.unresolved_trap) +
        " (a call faults naming it; weak ones are 0): " + names);
  }
  return m;
}

bool Boot::BootOne(const std::string& apk, const std::string& lib, std::string* out) {
  if (apk_.empty()) apk_ = apk;  // a guest dlopen reads its libraries from here
  if (apk.empty()) {
    Say("FATAL: rb_runtime_boot: no APK path was given");
    return false;
  }
  if (lib.empty() || lib.find('/') != std::string::npos) {
    Say("FATAL: rb_runtime_boot: '" + lib + "' is not a library file name");
    return false;
  }
  std::string err;
  auto zip = rb::loader::ZipReader::OpenFile(apk, &err);
  if (!zip) {
    Say("FATAL: rb_runtime_boot: cannot read the APK: " + err);
    return false;
  }
  if (!proc_) {
    if (!BootScope(apk, *zip, out)) return false;
  } else {
    Say("scope: reused — booted by an earlier rb_runtime_boot in this process (" +
        std::to_string(proc_->constructors_run()) + " constructors run so far); " + lib + " loads against it");
    // A later boot can arrive on a different Java thread (each System.loadLibrary runs on its caller's thread).
    // The main guest thread stays with the host thread that booted the scope: this one is adopted on its first
    // guest call (its own pthread, stack and TLS), as on a device.
  }
  GuestProcess& p = *proc_;
  std::set<std::string> in_progress;
  const rb::loader::GuestModule* app = LoadWithNeeded(*zip, lib, in_progress, out);
  if (app == nullptr) {
    failure_ = lib + " did not load";
    Say("RESULT: stopped during startup — " + lib + " did not load (the FATAL line above says why)");
    return false;
  }
  std::string failed;
  touched_ = true;
  if (!p.RunConstructors(&failed)) {
    failure_ = "constructor " + failed;
    Say("RESULT: stopped during startup — constructor " + failed);
    return false;
  }
  if (app->init.addr == 0 && app->init_array.empty()) {
    Say("constructors: " + lib + " carries no DT_INIT_ARRAY and no DT_INIT");
  }
  std::size_t java_entries = 0;
  for (const std::string& name : app->ExportNames()) {
    if (name.compare(0, 5, "Java_") == 0) ++java_entries;
  }
  const auto onload = app->FindExport("JNI_OnLoad");
  if (!onload) {
    Say("JNI_OnLoad: " + lib + " exports none; its " + std::to_string(java_entries) +
        " Java_* entry points are called by its own Java");
    Say("RESULT: startup finished with no fault — every constructor returned (" + lib +
        " has no JNI_OnLoad). Boot-only: nothing drives the game yet");
    return true;
  }
  // The guest JNI_OnLoad gets the bridge's JavaVM, synchronously on this (the loading) thread, inside a local
  // frame of its own as ART gives one.
  Say("JNI_OnLoad: guest JavaVM at " + Hex(jni_->vm()) + ", JNIEnv at " + Hex(jni_->env()));
  p.abi().set_executor(p.engine(cfg_.ctor_tier));
  rb::exec::RunResult rr{};
  rb::exec::MachineState exit{};
  jni_->PushFrame();
  // Initialisation, not a bounded call: a Unity title does its metadata and scripting setup here, and the
  // 4M-step call budget stopped Nutty Fluffies halfway through it (C27). See GuestProcess::kInitBudget.
  const rb::abi::Ret32 ret = p.abi().CallGuest(
      onload->tagged(), {rb::abi::Value32::Ptr(jni_->vm()), rb::abi::Value32::Ptr(0)}, rb::abi::Shorty::For("II"),
      GuestProcess::kInitBudget, &rr, &exit);
  jni_->PopFrame(0);
  if (rr.reason != rb::exec::StopReason::Returned) {
    Say("JNI_OnLoad: did not return on Tier " + std::string{cfg_.ctor_tier == 2 ? "J" : "I"} + " after " +
        std::to_string(rr.instructions_executed) + " steps: " + rr.detail + "; pc " + Hex(exit.pc) + " lr " +
        Hex(exit.r[14]));
    failure_ = lib + " JNI_OnLoad did not return: " + rr.detail;
    Say("RESULT: the constructors finished, then JNI_OnLoad stopped — the line above says where");
    return false;
  }
  Say("JNI_OnLoad: returned " + Hex(ret.w()) + " on Tier " + std::string{cfg_.ctor_tier == 2 ? "J" : "I"} + ", " +
      std::to_string(rr.instructions_executed) + " steps");
  Say("RESULT: startup finished with no fault — every constructor and JNI_OnLoad returned. Boot-only: " +
      std::to_string(java_entries) + " Java_* entry points wait for the game's own Java");
  return true;
}

BootResult Boot::BootLibrary(const std::string& apk, const std::string& lib) {
  std::lock_guard<std::mutex> lk(boot_mu_);  // held until return: one boot at a time
  boot_owner_.store(std::this_thread::get_id());
  struct OwnerReset {
    std::atomic<std::thread::id>* owner;
    ~OwnerReset() { owner->store(std::thread::id{}); }
  } owner_reset{&boot_owner_};

  BootResult res;
  if (booted_.count(lib) != 0) {
    Live("rb_runtime_boot: " + lib + " already booted in this process");
    res.ok = true;
    res.already = true;
    return res;
  }
  std::string report;
  {
    std::lock_guard<std::mutex> rl(report_mu_);
    report_ = &report;
  }
  Say("rb_runtime_boot: lib=" + (lib.empty() ? std::string{"(none)"} : lib) +
      " apk=" + (apk.empty() ? std::string{"(none)"} : apk));
  touched_ = false;
  failure_.clear();
  if (!poisoned_.empty()) {
    Say("FATAL: rb_runtime_boot: refused — the guest process faulted in an earlier boot: " + poisoned_ +
        ". It is half-initialized, so no guest work runs in this process again");
    Say("RESULT: stopped during startup — the guest process faulted in an earlier boot (above)");
  } else {
    try {
      res.ok = BootOne(apk, lib, &report);
      if (cfg_.report_suffix && proc_) cfg_.report_suffix(&report, *proc_);
    } catch (const std::exception& e) {
      Say(std::string{"FATAL: unhandled exception while booting: "} + e.what());
      Say("RESULT: stopped during startup — a host exception (above)");
      failure_ = std::string{"a host exception: "} + e.what();
      touched_ = touched_ || proc_ != nullptr;
    } catch (...) {
      Say("FATAL: unhandled exception of a non-standard type while booting");
      Say("RESULT: stopped during startup — a host exception (above)");
      failure_ = "a host exception of a non-standard type";
      touched_ = touched_ || proc_ != nullptr;
    }
    if (res.ok) {
      booted_.insert(lib);
      std::lock_guard<std::mutex> pl(publish_mu_);
      published_proc_ = proc_.get();
      published_libs_[lib] = loaded_[lib];
    } else if (touched_ && proc_) {
      // Guest state changed before the failure: nothing in this process may run guest code again.
      poisoned_ = lib + ": " + (failure_.empty() ? std::string{"see its report"} : failure_);
      Say("rb_runtime_boot: the guest process is now refused for every later boot (" + poisoned_ + ")");
    }
    // A failure before any guest state changed (no APK, a missing entry, a scope that did not boot and was
    // discarded) leaves nothing behind, so a later call may try again.
  }
  {
    std::lock_guard<std::mutex> rl(report_mu_);
    report_ = nullptr;
  }
  res.report = std::move(report);
  return res;
}

std::vector<std::string> Boot::BundledLibraries(const std::string& apk) {
  std::vector<std::string> names;
  std::string err;
  auto zip = rb::loader::ZipReader::OpenFile(apk, &err);
  if (!zip) return names;
  const std::string prefix = "assets/rb32/";
  for (const std::string& entry : zip->Names()) {
    if (entry.rfind(prefix, 0) != 0 || entry.size() <= prefix.size() + 3 ||
        entry.compare(entry.size() - 3, 3, ".so") != 0) {
      continue;
    }
    const std::string base = entry.substr(prefix.size());
    if (base.find('/') == std::string::npos) names.push_back(base);
  }
  std::sort(names.begin(), names.end());
  return names;
}

bool Boot::BundlesDlgl(const std::string& apk) {
  std::string err;
  auto zip = rb::loader::ZipReader::OpenFile(apk, &err);
  return zip != nullptr && zip->Has("assets/rb32/libdlgl.so");
}

BootResult Boot::BootJavaLibraries(const std::string& apk, const std::string& lib) {
  // Java's own order, from AndroidKitKatActivity.<clinit>: System.loadLibrary("dlgl") then
  // System.loadLibrary("androidkitkat"). The second library is the app: its JNI_OnLoad constructs Main and
  // stores it in DLApp::_currentInstance, which libdlgl only reads. A DLGL APK ships both, and a library
  // whose DT_NEEDED reaches one already in the order is part of that same Java load sequence.
  std::vector<std::string> order;
  const auto add = [&order](const std::string& name) {
    if (std::find(order.begin(), order.end(), name) == order.end()) order.push_back(name);
  };
  add(lib);
  if (BundlesDlgl(apk)) {
    if (lib != "libdlgl.so") add("libdlgl.so");
    std::string err;
    auto zip = rb::loader::ZipReader::OpenFile(apk, &err);
    if (zip) {
      const auto bundled = BundledLibraries(apk);
      for (bool changed = true; changed;) {
        changed = false;
        for (const std::string& name : bundled) {
          if (std::find(order.begin(), order.end(), name) != order.end()) continue;
          auto image = zip->Read("assets/rb32/" + name, &err);
          if (!image) continue;
          for (const std::string& need : rb::loader::NeededSonames(*image)) {
            if (std::find(order.begin(), order.end(), need) != order.end()) {
              add(name);
              changed = true;
              break;
            }
          }
        }
      }
    }
    // Boot dlgl first, whatever the caller picked: its JNI_OnLoad sets the JavaVM/JNIEnv the engine's Java
    // calls use, and registers the natives the DLGL surface calls.
    std::stable_sort(order.begin(), order.end(), [](const std::string& a, const std::string& b) {
      return a == "libdlgl.so" && b != "libdlgl.so";
    });
  }
  if (order.size() > 1) {
    std::string line = "java: loads";
    for (const std::string& name : order) line += " " + name;
    Live(line + " (the APK's Java loads every library it ships, in this order)");
  }
  BootResult all;
  for (const std::string& name : order) {
    const BootResult one = BootLibrary(apk, name);
    all.ok = one.ok;
    all.already = one.already;
    if (!one.report.empty()) {
      if (!all.report.empty() && all.report.back() != '\n') all.report += '\n';
      all.report += one.report;
    }
    if (!one.ok) return all;
  }
  return all;
}

std::uint32_t Boot::DlHandle(const rb::loader::GuestModule* m) {
  for (std::size_t i = 0; i < dl_handles_.size(); ++i) {
    if (dl_handles_[i] == m) return kDlModuleBase | static_cast<std::uint32_t>(i);
  }
  dl_handles_.push_back(m);
  return kDlModuleBase | static_cast<std::uint32_t>(dl_handles_.size() - 1);
}

std::uint32_t Boot::DlOpen(const std::string& path, std::string* why) {
  const std::thread::id owner = boot_owner_.load();
  if (owner != std::thread::id{} && owner != std::this_thread::get_id()) {
    *why = "another library is starting on another thread";
    return 0;
  }
  if (path.empty()) return kDlGlobal;  // dlopen(NULL): the "main program" — dlsym on it searches everything
  const std::string base = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
  if (proc_) {
    for (const auto& m : proc_->modules()) {
      if (m->soname == base) return DlHandle(m.get());  // already placed (the scope, or an app)
    }
  }
  if (const auto hit = loaded_.find(base); hit != loaded_.end()) return DlHandle(hit->second);
  static const char* const kHost[] = {"libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libEGL.so", "libOpenSLES.so",
                                      "libandroid.so", "liblog.so", "libjnigraphics.so", "libdl.so"};
  for (const char* h : kHost) {
    if (base == h) return kDlGlobal;  // answered by name through the host's import answers
  }
  std::string err;
  auto zip = rb::loader::ZipReader::OpenFile(apk_, &err);
  if (!zip) {
    *why = "the game's APK cannot be read (" + err + ")";
    return 0;
  }
  if (!zip->Has("assets/rb32/" + base)) {
    *why = base + " is not one of the game's 32-bit libraries";
    return 0;
  }
  std::set<std::string> in_progress;
  const rb::loader::GuestModule* m = LoadWithNeeded(*zip, base, in_progress, nullptr);
  if (m == nullptr) {
    *why = base + " did not load (the runtime log says why)";
    return 0;
  }
  std::string failed;
  if (!proc_->RunConstructors(&failed)) {
    *why = base + ": constructor " + failed;
    return 0;
  }
  Live("dl: dlopen(\"" + path + "\") loaded " + base + " from assets/rb32/");
  return DlHandle(m);
}

std::optional<std::uint32_t> Boot::DlSym(std::uint32_t handle, const std::string& name) {
  if ((handle & 0xFFFF0000u) == kDlModuleBase && (handle & 0xFFFFu) < dl_handles_.size()) {
    if (auto e = dl_handles_[handle & 0xFFFFu]->FindExport(name)) return e->tagged();
  } else if (handle != kDlGlobal && handle != kRtldDefault && handle != kRtldNext) {
    return std::nullopt;  // not a handle this runtime gave out
  }
  if (proc_) {
    for (const auto& m : proc_->modules()) {
      if (auto e = m->FindExport(name)) return e->tagged();
    }
  }
  auto at = cfg_.resolve_host ? cfg_.resolve_host(name) : std::nullopt;
  if (!at) {  // named once each: a NULL the game may call later (an engine probing for optional entry points)
    static std::set<std::string> said;
    static std::mutex said_mu;
    bool first = false;
    {
      std::lock_guard<std::mutex> lk(said_mu);
      first = said.insert(name).second;
    }
    if (first) Live("dl: dlsym(\"" + name + "\") = NULL (no guest export, no host answer)");
  }
  return at;
}

rb::runtime::GuestProcess* Boot::process() {
  std::lock_guard<std::mutex> lk(publish_mu_);
  return published_proc_;
}

rb::abi::HostJava* Boot::java() { return java_.get(); }

rb::abi::ForwardingJni* Boot::jni() { return jni_.get(); }

rb::runtime::Inbound* Boot::inbound() { return inbound_.get(); }

const rb::loader::GuestModule* Boot::module(const std::string& lib) {
  std::lock_guard<std::mutex> lk(publish_mu_);
  const auto it = published_libs_.find(lib);
  return it == published_libs_.end() ? nullptr : it->second;
}

bool Boot::booted(const std::string& lib) const { return booted_.count(lib) != 0; }

std::string Boot::poisoned() const { return poisoned_; }

void Boot::StartMonitor() {
  // A statistical guest profile: the pc at every slice boundary (each kSliceSteps guest steps), bucketed by
  // 64 bytes; every 10 s the top buckets and the instruction rate go to the log.
  struct Profile {
    std::mutex mu;
    std::map<std::uint32_t, std::uint32_t> hits;
    std::uint64_t slices = 0;
  };
  auto prof = std::make_shared<Profile>();
  proc_->abi().set_slice_probe([prof](std::uint32_t pc) {
    std::lock_guard<std::mutex> lk(prof->mu);
    ++prof->hits[pc & ~0x3fu];
    ++prof->slices;
  });
  auto* jit = dynamic_cast<rb::exec::JitExecutor*>(&proc_->engine(2));
  // The liveness supervisor: a window with no slice is a stall, and the one thing a black screen never says is
  // which thread is waiting on what.
  monitor_ = std::thread([this, prof, jit] {
    std::uint64_t last_t = 0, last_i = 0, last_activity = 0;
    int quiet_windows = 0;
    for (int round = 0; round < 60; ++round) {
      {
        std::unique_lock<std::mutex> lk(monitor_mu_);
        if (monitor_cv_.wait_for(lk, std::chrono::seconds(10), [this] { return monitor_stop_; })) return;
      }
      std::vector<std::pair<std::uint32_t, std::uint32_t>> top;
      std::uint64_t slices = 0;
      {
        std::lock_guard<std::mutex> lk(prof->mu);
        top.assign(prof->hits.begin(), prof->hits.end());
        slices = prof->slices;
        prof->hits.clear();
        prof->slices = 0;
      }
      std::sort(top.begin(), top.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
      std::string line = "prof: " + std::to_string(slices) + " slices (" +
                         std::to_string(slices * rb::abi::AbiContext::kSliceSteps / 10'000'000) +
                         " M guest instr/s) top:";
      for (std::size_t i = 0; i < top.size() && i < 15; ++i) {
        line += " " + Hex(top[i].first) + "x" + std::to_string(top[i].second);
      }
      if (jit != nullptr) {
        const std::uint64_t t = jit->translated(), inv = jit->invalidations();
        line += " | translated " + std::to_string(t - last_t) + " instr, " + std::to_string(inv - last_i) +
                " invalidations";
        last_t = t;
        last_i = inv;
      }
      if (slices != 0) {
        quiet_windows = 0;
        Live(line);
      } else if (proc_ != nullptr && proc_->abi().host_activity() != last_activity) {
        // Alive, just outside long JIT stints: a render loop that calls JNI or a GL thunk every few thousand
        // instructions never retires a whole slice. Saying "stalled" here was the C46 investigation's biggest
        // false trail — the game was swapping frames at 35 fps while this line said it was dead.
        last_activity = proc_->abi().host_activity();
        quiet_windows = 0;
        Live("alive: no slice in 10 s, " + std::to_string(last_activity) + " host call(s) so far — sub-slice "
             "stints, not a stall");
      } else if (proc_ != nullptr && !proc_->monitor().group_exited()) {
        // No guest instruction retired for 10 s: dump every guest thread's pc and what it waits on. Three windows
        // up front, then every sixth, so a long stall does not flood the log.
        ++quiet_windows;
        if (quiet_windows == 1) {
          rb::abi::ForwardingJni::SetCallTrace([this](const std::string& l) { Live(l); }, 300);
        }
        if (quiet_windows == 2 && cfg_.java_thread_dump) {
          static std::atomic<bool> dumped{false};
          if (!dumped.exchange(true)) {
            for (const std::string& l : cfg_.java_thread_dump()) Live("stall:   " + l);
          }
        }
        if (cfg_.frame_stats) {
          const std::string frames = cfg_.frame_stats();
          if (!frames.empty()) Live("stall: egl: " + frames);
        }
        if (quiet_windows <= 3 || quiet_windows % 6 == 0) {
          Live("stall: no guest instruction retired in the last 10 s (window " + std::to_string(quiet_windows) + "):");
          for (const std::string& l : proc_->StallReport()) Live("stall:   " + l);
        }
      }
    }
  });
}

void Boot::StopMonitor() {
  {
    std::lock_guard<std::mutex> lk(monitor_mu_);
    monitor_stop_ = true;
  }
  monitor_cv_.notify_all();
  if (monitor_.joinable()) monitor_.join();
  monitor_stop_ = false;
}

void Boot::DropScope() {
  StopMonitor();
  inbound_.reset();
  jni_.reset();
  java_.reset();
  proc_.reset();
}

}  // namespace rb::host
