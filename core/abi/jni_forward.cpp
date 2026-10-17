// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "rb/abi/jni_forward.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "rb/abi/jni_args.h"
#include "rb/runtime/ggl.h"

namespace rb::abi {
namespace {

std::atomic<std::uint64_t> g_serial{1};

std::string Hex(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%x", v);
  return buf;
}

constexpr const char* kCallTypes[] = {"Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double", "Void"};
constexpr char kCallKinds[] = "LZBCSIJFDV";  // the Call<T>Method families, in table order
constexpr char kFieldKinds[] = "LZBCSIJFD";  // the Get/Set<T>Field families
constexpr const char* kForms[] = {"", "V", "A"};
constexpr const char* kArrayTypes[] = {"Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double"};

/// The standard JNI 1.6 function-table name of entry `i` (the indices tests/corpus/jni-offsets.txt measured).
std::string EntryName(std::uint32_t i) {
  static const char* const kSingles[] = {
      nullptr, nullptr, nullptr, nullptr, "GetVersion", "DefineClass", "FindClass", "FromReflectedMethod",
      "FromReflectedField", "ToReflectedMethod", "GetSuperclass", "IsAssignableFrom", "ToReflectedField", "Throw",
      "ThrowNew", "ExceptionOccurred", "ExceptionDescribe", "ExceptionClear", "FatalError", "PushLocalFrame",
      "PopLocalFrame", "NewGlobalRef", "DeleteGlobalRef", "DeleteLocalRef", "IsSameObject", "NewLocalRef",
      "EnsureLocalCapacity", "AllocObject", "NewObject", "NewObjectV", "NewObjectA", "GetObjectClass", "IsInstanceOf",
      "GetMethodID"};
  if (i < 4) return "reserved" + std::to_string(i);
  if (i <= 33) return kSingles[i];
  if (i <= 63) return std::string{"Call"} + kCallTypes[(i - 34) / 3] + "Method" + kForms[(i - 34) % 3];
  if (i <= 93) return std::string{"CallNonvirtual"} + kCallTypes[(i - 64) / 3] + "Method" + kForms[(i - 64) % 3];
  if (i == 94) return "GetFieldID";
  if (i <= 103) return std::string{"Get"} + kCallTypes[i - 95] + "Field";
  if (i <= 112) return std::string{"Set"} + kCallTypes[i - 104] + "Field";
  if (i == 113) return "GetStaticMethodID";
  if (i <= 143) return std::string{"CallStatic"} + kCallTypes[(i - 114) / 3] + "Method" + kForms[(i - 114) % 3];
  if (i == 144) return "GetStaticFieldID";
  if (i <= 153) return std::string{"GetStatic"} + kCallTypes[i - 145] + "Field";
  if (i <= 162) return std::string{"SetStatic"} + kCallTypes[i - 154] + "Field";
  static const char* const kStrings[] = {"NewString", "GetStringLength", "GetStringChars", "ReleaseStringChars",
                                         "NewStringUTF", "GetStringUTFLength", "GetStringUTFChars",
                                         "ReleaseStringUTFChars", "GetArrayLength", "NewObjectArray",
                                         "GetObjectArrayElement", "SetObjectArrayElement"};
  if (i <= 174) return kStrings[i - 163];
  if (i <= 182) return std::string{"New"} + kArrayTypes[i - 175] + "Array";
  if (i <= 190) return std::string{"Get"} + kArrayTypes[i - 183] + "ArrayElements";
  if (i <= 198) return std::string{"Release"} + kArrayTypes[i - 191] + "ArrayElements";
  if (i <= 206) return std::string{"Get"} + kArrayTypes[i - 199] + "ArrayRegion";
  if (i <= 214) return std::string{"Set"} + kArrayTypes[i - 207] + "ArrayRegion";
  static const char* const kTail[] = {"RegisterNatives", "UnregisterNatives", "MonitorEnter", "MonitorExit",
                                      "GetJavaVM", "GetStringRegion", "GetStringUTFRegion",
                                      "GetPrimitiveArrayCritical", "ReleasePrimitiveArrayCritical",
                                      "GetStringCritical", "ReleaseStringCritical", "NewWeakGlobalRef",
                                      "DeleteWeakGlobalRef", "ExceptionCheck", "NewDirectByteBuffer",
                                      "GetDirectBufferAddress", "GetDirectBufferCapacity", "GetObjectRefType"};
  if (i <= 232) return kTail[i - 215];
  return "entry" + std::to_string(i);
}

/// A field descriptor's kind letter: references and arrays are L.
bool FieldKind(const std::string& sig, char* kind) {
  if (sig.empty()) return false;
  const char c = sig[0];
  if (c == 'L' || c == '[') {
    *kind = 'L';
    return c == '[' || sig.back() == ';';
  }
  if (sig.size() != 1 || std::strchr("ZBCSIJFD", c) == nullptr) return false;
  *kind = c;
  return true;
}

/// UTF-16 to the bytes ART's GetStringUTFChars gives (tests/corpus/oracle-jnibridge.txt, the device's own ART):
/// U+0000 as C0 80, a well-formed surrogate pair as one 4-byte UTF-8 sequence, a lone surrogate as its 3-byte form.
std::string Mutf8(const char16_t* s, std::size_t n) {
  std::string out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
      const std::uint32_t cp = 0x10000 + ((c - 0xD800) << 10) + (static_cast<std::uint32_t>(s[++i]) - 0xDC00);
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (c != 0 && c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xE0 | (c >> 12)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

/// Guest bytes (NUL-terminated, so no 00 inside) to UTF-16, the way ART's NewStringUTF reads them: 1-, 2- and
/// 3-byte sequences (C0 80 and surrogate halves included) and standard 4-byte UTF-8 as a surrogate pair. Anything
/// else becomes U+FFFD, one per offending byte, and `*sanitized` is set.
std::u16string FromMutf8(const std::string& b, bool* sanitized) {
  std::u16string out;
  const auto cont = [&](std::size_t i) { return i < b.size() && (static_cast<unsigned char>(b[i]) & 0xC0) == 0x80; };
  for (std::size_t i = 0; i < b.size();) {
    const unsigned c = static_cast<unsigned char>(b[i]);
    if (c < 0x80) {
      out.push_back(static_cast<char16_t>(c));
      ++i;
    } else if ((c & 0xE0) == 0xC0 && cont(i + 1)) {
      out.push_back(static_cast<char16_t>(((c & 0x1F) << 6) | (static_cast<unsigned char>(b[i + 1]) & 0x3F)));
      i += 2;
    } else if ((c & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2)) {
      out.push_back(static_cast<char16_t>(((c & 0x0F) << 12) | ((static_cast<unsigned char>(b[i + 1]) & 0x3F) << 6) |
                                          (static_cast<unsigned char>(b[i + 2]) & 0x3F)));
      i += 3;
    } else if ((c & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
      const std::uint32_t cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(b[i + 1]) & 0x3F) << 12) |
                               ((static_cast<unsigned char>(b[i + 2]) & 0x3F) << 6) |
                               (static_cast<unsigned char>(b[i + 3]) & 0x3F);
      if (cp >= 0x10000 && cp <= 0x10FFFF) {
        out.push_back(static_cast<char16_t>(0xD800 + ((cp - 0x10000) >> 10)));
        out.push_back(static_cast<char16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF)));
      } else {
        out.push_back(char16_t{0xFFFD});
        *sanitized = true;
      }
      i += 4;
    } else {
      out.push_back(char16_t{0xFFFD});
      *sanitized = true;
      ++i;
    }
  }
  return out;
}

constexpr char kArrayKinds[] = "ZBCSIJFD";  // the New/Get/Release/Region<T>Array families, in table order

std::uint32_t ElemSize(char k) {
  switch (k) {
    case 'Z':
    case 'B': return 1;
    case 'C':
    case 'S': return 2;
    case 'J':
    case 'D': return 8;
    default: return 4;
  }
}

/// Entries JNI allows while an exception is pending (JNI spec, "Exceptions"): the rest are forwarded too, as release
/// ART does, but named.
bool ExceptionSafe(std::uint32_t i) {
  switch (i) {
    case 15: case 16: case 17: case 228:              // ExceptionOccurred/Describe/Clear/Check
    case 166: case 170: case 223: case 225:           // ReleaseStringChars/UTFChars, the criticals
    case 19: case 20: case 22: case 23: case 227:     // frames, deleting references
    case 218:                                         // MonitorExit
      return true;
    default: return i >= 191 && i <= 198;             // Release<T>ArrayElements
  }
}

/// Kinds a `Call<T>Method` family may be used for: its own, and any integral kind of at most 32 bits for another
/// (the value sits in r0 either way, as it does on a device with CheckJNI off).
bool Compatible(char family, char actual) {
  if (family == actual) return true;
  const auto narrow = [](char k) { return std::strchr("ZBCSI", k) != nullptr && k != '\0'; };
  return narrow(family) && narrow(actual);
}

}  // namespace

ForwardingJni::ForwardingJni(mem::GuestMemory& memory, AbiContext& abi, GuestAddr where, HostJava& java,
                             std::string* err)
    : memory_{memory}, abi_{abi}, java_{java}, where_{where}, serial_{g_serial.fetch_add(1)} {
  const auto fail = [&](const std::string& why) {
    err_ = "ForwardingJni: " + why;
    if (err != nullptr) *err = err_;
  };
  if (where % mem::kGuestPage != 0) {
    fail("the env page " + Hex(where) + " is not page-aligned");
    return;
  }
  runtime::Ggl::Held hold(abi_.ggl());  // slots are published under the GGL
  // The env word holds the table's address; the table holds one slot address per entry, implemented or not, so a
  // guest reaching an entry this bridge does not forward gets a refusal naming it rather than a branch to 0.
  const GuestAddr table = where + 16;
  if (!memory_.Write(where, &table, 4)) {
    fail("the env page " + Hex(where) + " is not writable guest memory");
    return;
  }
  for (std::uint32_t i = 0; i < kTableEntries; ++i) {
    const GuestAddr slot = abi_.SlotFor(kEnvSlotBase + i);
    if (!memory_.Write(table + 4 * i, &slot, 4)) {
      fail("writing the function table");
      return;
    }
    abi_.Bind(
        kEnvSlotBase + i,
        [this, i](exec::MachineState& s, std::string* e) {
          // Host-memory direct buffers cross with every call into Java and back (SyncShadows).
          if (small_shadows_.load(std::memory_order_relaxed) != 0) SyncShadows();
          const bool ok = Slot(i, s, e);
          if (small_shadows_.load(std::memory_order_relaxed) != 0) SyncShadows();
          return ok;
        },
        AbiContext::SlotLock::kReleasesGgl);
  }
  // The JavaVM: a word at kVmOffset holding its invoke-interface table's address, 3 reserved entries then 5.
  const GuestAddr vm_table = where + kVmOffset + 16;
  const GuestAddr vm_word = where + kVmOffset;
  if (!memory_.Write(vm_word, &vm_table, 4)) {
    fail("writing the JavaVM word");
    return;
  }
  for (std::uint32_t i = 0; i < kVmEntries; ++i) {
    const GuestAddr slot = abi_.SlotFor(kVmSlotBase + i);
    if (!memory_.Write(vm_table + 4 * i, &slot, 4)) {
      fail("writing the JavaVM table");
      return;
    }
    abi_.Bind(
        kVmSlotBase + i, [this, i](exec::MachineState& s, std::string* e) { return VmSlot(i, s, e); },
        AbiContext::SlotLock::kReleasesGgl);
  }
  ok_ = true;
}

ForwardingJni::~ForwardingJni() {
  if (!ok_) return;
  runtime::Ggl::Held hold(abi_.ggl());
  for (std::uint32_t i = 0; i < kTableEntries; ++i) {
    abi_.Bind(kEnvSlotBase + i, [i](exec::MachineState&, std::string* e) {
      if (e != nullptr) *e = "JNIEnv->" + EntryName(i) + ": the forwarding JNI bridge has been destroyed";
      return false;
    });
  }
  for (std::uint32_t i = 0; i < kVmEntries; ++i) {
    abi_.Bind(kVmSlotBase + i, [](exec::MachineState&, std::string* e) {
      if (e != nullptr) *e = "JavaVM: the forwarding JNI bridge has been destroyed";
      return false;
    });
  }
}

// ------------------------------------------------------------------------------------------------- handles

ForwardingJni::LocalTable::~LocalTable() {
  // The host thread is exiting: its locals are gone with it, and its thread number goes back to the pool.
  std::size_t live = 0;
  for (const HostRef r : refs) live += r != 0;
  shared->live.fetch_sub(live, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(shared->mu);
  shared->free_threads.push_back(thread);
  // Its ghosts are weak globals: an exiting thread may have no Java left to delete them with, so the bridge does.
  for (const HostRef g : ghosts) {
    if (g != 0) shared->orphans.push_back(g);
  }
}

ForwardingJni::LocalTable* ForwardingJni::Locals(std::string* err) {
  // Per host thread and per bridge instance (several bridges can live in one process, e.g. a test's).
  static thread_local std::vector<std::pair<std::uint64_t, std::unique_ptr<LocalTable>>> tables;
  for (auto& [serial, t] : tables) {
    if (serial == serial_) return t.get();
  }
  auto t = std::make_unique<LocalTable>();
  t->shared = shared_;
  {
    std::lock_guard<std::mutex> lk(shared_->mu);
    if (!shared_->free_threads.empty()) {
      t->thread = shared_->free_threads.back();
      shared_->free_threads.pop_back();
    } else if (shared_->next_thread < kMaxThreads) {
      t->thread = shared_->next_thread++;
    } else {
      if (err != nullptr) *err = "more than " + std::to_string(kMaxThreads) + " live host threads use this JNI bridge";
      t->shared = nullptr;  // nothing to give back
      return nullptr;
    }
  }
  t->free.emplace_back();  // the base frame's free slots
  tables.emplace_back(serial_, std::move(t));
  return tables.back().second.get();
}

GuestAddr ForwardingJni::ToGuestLocal(HostRef r, std::string* err) {
  if (r == 0) return 0;
  LocalTable* t = Locals(err);
  if (t == nullptr) return 0;
  // A deleted slot of the current frame first (a native that loops inside one call keeps reusing a few), else
  // a new one at the top.
  std::size_t n = 0;
  if (!t->free.back().empty()) {
    n = t->free.back().back();
    t->free.back().pop_back();
  } else {
    n = t->refs.size();
    if (n >= kMaxLocals) {
      if (err != nullptr) {
        *err = "more than " + std::to_string(kMaxLocals) + " live local references on one thread (a guest that "
               "never deletes its locals inside a long call)";
      }
      return 0;
    }
    t->refs.push_back(0);
    if (t->serials.size() <= n) t->serials.resize(n + 1, 0);
  }
  t->refs[n] = r;
  t->serials[n] = static_cast<std::uint8_t>((t->serials[n] + 1) & 0xfu);
  shared_->live.fetch_add(1, std::memory_order_relaxed);
  return kLocalBase | (t->thread << 20) | (static_cast<std::uint32_t>(n) << 6) |
         (static_cast<std::uint32_t>(t->serials[n]) << 2);
}

HostRef ForwardingJni::FromGuest(GuestAddr h, std::string* err) {
  const auto fail = [&](const std::string& why) -> HostRef {
    if (err != nullptr) *err = why;
    return 0;
  };
  if (h == 0) {
    if (err != nullptr) err->clear();
    return 0;
  }
  if ((h & 3u) != 0) return fail(Hex(h) + " is not a reference this bridge handed out");
  if (h >= kLocalBase) {
    LocalTable* t = Locals(err);
    if (t == nullptr) return 0;
    const std::uint32_t thread = (h >> 20) & 0xffu;
    const std::uint32_t n = (h >> 6) & 0x3fffu;
    const std::uint32_t serial = (h >> 2) & 0xfu;
    if (thread != t->thread) {
      return fail(Hex(h) + " is another thread's local reference (thread " + std::to_string(thread) +
                  "; this is thread " + std::to_string(t->thread) + ")");
    }
    if (n >= t->refs.size() || t->refs[n] == 0 || t->serials[n] != serial) {
      if (n < t->ghosts.size() && t->ghosts[n] != 0 && t->ghost_serials[n] == serial &&
          !java_.IsSameObject(t->ghosts[n], 0)) {  // a collected object is stale, as it is anywhere
        Note("a native's argument used after that native returned (a stale local): answered with its object, as "
             "ART reads it from the returned call's stack");
        return t->ghosts[n];
      }
      return fail(Hex(h) + " is a stale local reference (deleted, or the frame that made it has returned)");
    }
    return t->refs[n];
  }
  if (h >= kWeakBase) {
    const bool weak = h < kGlobalBase;
    std::lock_guard<std::mutex> lk(globals_mu_);
    const std::vector<HostRef>& table = weak ? weaks_ : globals_;
    const std::size_t n = (h - (weak ? kWeakBase : kGlobalBase)) / 4u;
    if (n >= table.size() || table[n] == 0) {
      return fail(Hex(h) + " is a stale " + (weak ? "weak global" : "global") + " reference (deleted)");
    }
    return table[n];
  }
  if (h >= kClassBase) {
    std::lock_guard<std::mutex> lk(classes_mu_);
    const std::size_t n = (h - kClassBase) / 4u;
    if (n >= classes_.size()) return fail(Hex(h) + " is not a class this bridge handed out");
    return classes_[n];
  }
  if (h >= kIdBase) return fail(Hex(h) + " is a method or field ID, not a reference");
  return fail(Hex(h) + " is not a reference this bridge handed out");
}

void ForwardingJni::PushFrame() {
  std::string e;
  if (LocalTable* t = Locals(&e)) {
    t->frames.push_back(t->refs.size());
    t->free.emplace_back();
    t->args_end.push_back(t->refs.size());  // no arguments until ArgumentsIn says so
  }
}

void ForwardingJni::ArgumentsIn() {
  std::string e;
  LocalTable* t = Locals(&e);
  if (t != nullptr && !t->args_end.empty()) t->args_end.back() = t->refs.size();
}

HostRef ForwardingJni::PopFrame(GuestAddr result, std::string* err) {
  const HostRef r = FromGuest(result, err);
  std::string e;
  LocalTable* t = Locals(&e);
  if (t == nullptr || t->frames.empty()) return r;
  const std::size_t start = t->frames.back();
  const std::size_t args_end = std::min(t->args_end.back(), t->refs.size());
  t->frames.pop_back();
  t->free.pop_back();
  t->args_end.pop_back();
  // The frame's arguments outlive it as ghosts (see ArgumentsIn): one weak global per slot (a stack read pins
  // nothing on ART either: a weak referent a native was handed must still be collectable), the newest one kept.
  std::vector<HostRef> dead;
  for (std::size_t n = start; n < args_end; ++n) {
    if (t->refs[n] == 0) continue;
    if (t->ghosts.size() <= n) {
      t->ghosts.resize(n + 1, 0);
      t->ghost_serials.resize(n + 1, 0);
    }
    if (t->ghosts[n] != 0) dead.push_back(t->ghosts[n]);
    t->ghosts[n] = java_.NewWeakGlobalRef(t->refs[n]);
    t->ghost_serials[n] = t->serials[n];
  }
  {
    std::lock_guard<std::mutex> lk(shared_->mu);
    dead.insert(dead.end(), shared_->orphans.begin(), shared_->orphans.end());
    shared_->orphans.clear();
  }
  for (const HostRef g : dead) java_.DeleteWeakGlobalRef(g);
  std::size_t live = 0;
  for (std::size_t n = start; n < t->refs.size(); ++n) live += t->refs[n] != 0;
  t->refs.resize(start);  // the slots' serials stay, so a handle into a popped frame stays stale
  shared_->live.fetch_sub(live, std::memory_order_relaxed);
  return r;
}

std::size_t ForwardingJni::live_globals() const {
  std::lock_guard<std::mutex> lk(globals_mu_);
  std::size_t n = 0;
  for (const HostRef r : globals_) n += r != 0;
  return n;
}

GuestAddr ForwardingJni::ClassHandle(HostRef cls, bool delete_local, std::string* err) {
  if (cls == 0) return 0;
  std::size_t index = 0;
  {
    // One stable handle per class: the same class reached twice (FindClass, GetObjectClass, GetSuperclass) gives
    // the same jclass, pinned by a real global reference so it stays valid across calls and threads.
    std::lock_guard<std::mutex> lk(classes_mu_);
    bool found = false;
    for (std::size_t i = 0; i < classes_.size() && !found; ++i) {
      if (java_.IsSameObject(classes_[i], cls)) {
        index = i;
        found = true;
      }
    }
    if (!found) {
      const HostRef pinned = java_.NewGlobalRef(cls);
      if (pinned == 0) {
        if (err != nullptr) *err = "NewGlobalRef of a class failed";
        return 0;
      }
      classes_.push_back(pinned);
      index = classes_.size() - 1;
    }
  }
  if (delete_local) java_.DeleteLocalRef(cls);
  return kClassBase + 4u * static_cast<std::uint32_t>(index);
}

GuestAddr ForwardingJni::IdHandle(HostId id, bool is_field, bool is_static, const std::string& name,
                                  const std::string& sig, std::string* err) {
  if (id == 0) return 0;
  IdRec rec;
  rec.id = id;
  rec.is_field = is_field;
  rec.is_static = is_static;
  rec.name = name + (is_field ? ":" : "") + sig;
  if (is_field ? !FieldKind(sig, &rec.kind) : !jni::ParseSignature(sig, &rec.params, &rec.kind)) {
    if (err != nullptr) *err = "'" + sig + "' is not a readable JNI " + (is_field ? "field" : "method") + " signature";
    return 0;
  }
  std::lock_guard<std::mutex> lk(ids_mu_);
  const auto key = std::make_pair(id, is_field);
  if (const auto it = id_index_.find(key); it != id_index_.end()) return kIdBase + 4u * static_cast<std::uint32_t>(it->second);
  ids_.push_back(std::move(rec));
  id_index_[key] = ids_.size() - 1;
  return kIdBase + 4u * static_cast<std::uint32_t>(ids_.size() - 1);
}

const ForwardingJni::IdRec* ForwardingJni::Id(GuestAddr h, std::string* err) {
  std::lock_guard<std::mutex> lk(ids_mu_);
  if (h < kIdBase || h >= kClassBase || (h & 3u) != 0 || (h - kIdBase) / 4u >= ids_.size()) {
    if (err != nullptr) *err = Hex(h) + " is not a method or field ID this bridge handed out";
    return nullptr;
  }
  return &ids_[(h - kIdBase) / 4u];  // a deque element: never moved or freed while the bridge lives
}

bool ForwardingJni::ReadCString(GuestAddr at, std::string* out, std::string* err) const {
  // No length cap of the bridge's own (ART has none: a game's NewStringUTF of a JSON blob is fine): read in
  // 256-byte chunks to the NUL, a chunk that crosses into unreadable memory byte by byte, so the refusal names
  // the first unreadable byte.
  out->clear();
  char chunk[256];
  for (std::uint64_t n = 0; at + n < memory_.window_bytes();) {
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof chunk, memory_.window_bytes() - (at + n)));
    std::size_t got = want;
    if (!memory_.Read(static_cast<GuestAddr>(at + n), chunk, want)) {
      for (got = 0; got < want && memory_.Read(static_cast<GuestAddr>(at + n + got), chunk + got, 1); ++got) {
      }
    }
    for (std::size_t k = 0; k < got; ++k) {
      if (chunk[k] == '\0') return true;
      out->push_back(chunk[k]);
    }
    n += got;
    if (got < want) break;
  }
  if (err != nullptr) *err = "the string at " + Hex(at) + " is unreadable at byte " + std::to_string(out->size()) + " (no NUL before it)";
  return false;
}

void ForwardingJni::Return64(exec::MachineState& s, std::uint64_t v) const {
  s.r[0] = static_cast<std::uint32_t>(v);
  s.r[1] = static_cast<std::uint32_t>(v >> 32);
}

bool ForwardingJni::ReturnKind(exec::MachineState& s, char kind, std::uint64_t bits, std::string* err) {
  switch (kind) {
    case 'V': return true;
    case 'L': {
      std::string e;
      s.r[0] = ToGuestLocal(static_cast<HostRef>(bits), &e);
      if (!e.empty()) {
        if (err != nullptr) *err = e;
        return false;
      }
      return true;
    }
    case 'Z': s.r[0] = static_cast<std::uint32_t>(bits & 0xffu); return true;
    case 'B': s.r[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(bits))); return true;
    case 'C': s.r[0] = static_cast<std::uint32_t>(bits & 0xffffu); return true;
    case 'S': s.r[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(bits))); return true;
    case 'I':
    case 'F': s.r[0] = static_cast<std::uint32_t>(bits); return true;  // softfp: a float returns in r0
    case 'J':
    case 'D': Return64(s, bits); return true;
    default: break;
  }
  if (err != nullptr) *err = std::string{"no return kind '"} + kind + "'";
  return false;
}

// ------------------------------------------------------------------------------------------------- entries

bool ForwardingJni::CallFamily(std::uint32_t i, exec::MachineState& s, std::string* err) {
  std::uint32_t base = 34;
  unsigned fixed = 3;
  bool is_static = false, nonvirtual = false, construct = false;
  if (i >= 28 && i <= 30) {
    base = 28;
    construct = true;
  } else if (i >= 64 && i <= 93) {
    base = 64;
    fixed = 4;
    nonvirtual = true;
  } else if (i >= 114 && i <= 143) {
    base = 114;
    is_static = true;
  }
  const char family = construct ? 'L' : kCallKinds[(i - base) / 3];
  const jni::ArgForm form = (i - base) % 3 == 0 ? jni::ArgForm::kVariadic
                            : (i - base) % 3 == 1 ? jni::ArgForm::kVaList
                                                  : jni::ArgForm::kJvalues;
  std::uint32_t mid = 0;
  if (!jni::ArgWord(s, memory_, fixed - 1, &mid)) {
    *err = "the method ID argument is unreadable";
    return false;
  }
  IdRec rec;
  {
    const IdRec* r = Id(mid, err);
    if (r == nullptr) return false;
    rec = *r;
  }
  if (rec.is_field) {
    *err = rec.name + " is a field ID, not a method ID";
    return false;
  }
  if (rec.is_static != is_static) {
    *err = rec.name + (rec.is_static ? " is a static method" : " is an instance method") + ", called as " +
           (is_static ? "static" : "an instance method");
    return false;
  }
  if (construct ? rec.kind != 'V' : !Compatible(family, rec.kind)) {
    *err = rec.name + " returns '" + std::string(1, rec.kind) + "', called through the '" + std::string(1, family) +
           "' family";
    return false;
  }
  std::vector<std::uint64_t> args;
  if (!jni::DecodeArgs(s, memory_, form, rec.params, fixed, &args, err)) {
    *err = rec.name + ": " + *err;
    return false;
  }
  for (std::size_t k = 0; k < args.size(); ++k) {
    if (rec.params[k] != 'L') continue;
    std::string e;
    args[k] = FromGuest(static_cast<GuestAddr>(args[k]), &e);
    if (!e.empty()) {
      *err = rec.name + " argument " + std::to_string(k + 1) + ": " + e;
      return false;
    }
  }
  std::string e;
  const HostRef target = FromGuest(s.r[1], &e);
  if (!e.empty()) {
    *err = rec.name + (is_static || construct ? " class: " : " receiver: ") + e;
    return false;
  }
  if (target == 0) {
    *err = rec.name + (is_static || construct ? " called on a null class" : " called on a null receiver");
    return false;
  }
  HostRef nv_cls = 0;
  if (nonvirtual) {
    nv_cls = FromGuest(s.r[2], &e);
    if (!e.empty() || nv_cls == 0) {
      *err = rec.name + " nonvirtual class: " + (e.empty() ? std::string{"null"} : e);
      return false;
    }
  }
  if (construct) return ReturnKind(s, 'L', java_.NewObject(target, rec.id, args), err);
  const std::uint64_t bits = java_.Call(rec.kind, is_static, target, nv_cls, rec.id, args);
  if (java_.ExceptionCheck() && MaskIdentifierRefusal(rec.name)) return ReturnKind(s, family, 0, err);
  return ReturnKind(s, family, bits, err);  // in the shape the guest's call expects
}

bool ForwardingJni::FieldFamily(std::uint32_t i, exec::MachineState& s, std::string* err) {
  const bool is_static = i >= 145;
  const std::uint32_t get_base = is_static ? 145 : 95;
  const bool is_set = i >= get_base + 9;
  const char kind = kFieldKinds[i - get_base - (is_set ? 9 : 0)];
  IdRec rec;
  {
    const IdRec* r = Id(s.r[2], err);
    if (r == nullptr) return false;
    rec = *r;
  }
  if (!rec.is_field || rec.is_static != is_static) {
    *err = rec.name + " is not " + (is_static ? "a static" : "an instance") + " field ID";
    return false;
  }
  if (!Compatible(kind, rec.kind)) {
    *err = rec.name + " has type '" + std::string(1, rec.kind) + "', accessed as '" + std::string(1, kind) + "'";
    return false;
  }
  std::string e;
  const HostRef target = FromGuest(s.r[1], &e);
  if (!e.empty() || target == 0) {
    *err = rec.name + (is_static ? " class: " : " object: ") + (e.empty() ? std::string{"null"} : e);
    return false;
  }
  if (!is_set) return ReturnKind(s, kind, java_.GetField(kind, is_static, target, rec.id), err);
  std::vector<std::uint64_t> v;
  if (!jni::DecodeArgs(s, memory_, jni::ArgForm::kFixed, {kind}, 3, &v, err)) return false;
  if (kind == 'L') {
    v[0] = FromGuest(static_cast<GuestAddr>(v[0]), &e);
    if (!e.empty()) {
      *err = rec.name + " value: " + e;
      return false;
    }
  }
  java_.SetField(kind, is_static, target, rec.id, v[0]);
  return true;
}

bool ForwardingJni::Slot(std::uint32_t i, exec::MachineState& s, std::string* err) {
  std::string why;
  const auto refuse = [&](const std::string& what) {
    refusals_.fetch_add(1, std::memory_order_relaxed);
    if (err != nullptr) *err = "JNIEnv->" + EntryName(i) + ": " + what;
    return false;
  };
  const auto ref = [&](unsigned arg, HostRef* out) {
    std::string e;
    *out = FromGuest(s.r[arg], &e);
    if (!e.empty()) why = e;
    return e.empty();
  };
  const auto name_arg = [&](unsigned arg, std::string* out) { return ReadCString(s.r[arg], out, &why); };
  HostRef a = 0, b = 0;
  if (!ExceptionSafe(i) && i != 4 && java_.ExceptionCheck()) {
    Note("JNIEnv->" + EntryName(i) + " called while an exception is pending (forwarded, as release ART does)");
  }
  switch (i) {
    case 4:  // GetVersion
      s.r[0] = 0x00010006u;
      return true;
    case 6: {  // FindClass
      std::string name;
      if (!name_arg(1, &name)) return refuse(why);
      const HostRef c = java_.FindClass(name);
      s.r[0] = ClassHandle(c, true, &why);
      if (c != 0 && s.r[0] == 0) return refuse(why);
      return true;
    }
    case 10:  // GetSuperclass
      if (!ref(1, &a)) return refuse(why);
      if (a == 0) return refuse("null class");
      s.r[0] = ClassHandle(java_.GetSuperclass(a), true, &why);
      return true;
    case 11:  // IsAssignableFrom
      if (!ref(1, &a) || !ref(2, &b)) return refuse(why);
      if (a == 0 || b == 0) return refuse("null class");
      s.r[0] = java_.IsAssignableFrom(a, b) ? 1u : 0u;
      return true;
    case 21: {  // NewGlobalRef
      if (s.r[1] >= kClassBase && s.r[1] < kWeakBase) {  // a class handle is already global
        if (!ref(1, &a)) return refuse(why);
        s.r[0] = s.r[1];
        return true;
      }
      if (!ref(1, &a)) return refuse(why);
      if (a == 0) {
        s.r[0] = 0;
        return true;
      }
      const HostRef g = java_.NewGlobalRef(a);
      std::lock_guard<std::mutex> lk(globals_mu_);
      std::size_t n = 0;
      if (!global_free_.empty()) {
        n = global_free_.back();
        global_free_.pop_back();
        globals_[n] = g;
      } else {
        if (globals_.size() >= (kLocalBase - kGlobalBase) / 4u) return refuse("the global reference table is full");
        globals_.push_back(g);
        n = globals_.size() - 1;
      }
      s.r[0] = kGlobalBase + 4u * static_cast<std::uint32_t>(n);
      return true;
    }
    case 22:  // DeleteGlobalRef
    case 227: {  // DeleteWeakGlobalRef
      const GuestAddr h = s.r[1];
      if (h == 0 || (h >= kClassBase && h < kWeakBase)) return true;  // classes stay pinned
      const bool weak = i == 227;
      const GuestAddr base = weak ? kWeakBase : kGlobalBase;
      const GuestAddr end = weak ? kGlobalBase : kLocalBase;
      // A bad handle to a Delete* is noted and ignored, as release ART logs "failed to find entry" and carries on
      // (a double delete is a common, harmless bug in old games); only a *use* of a bad handle is refused.
      const std::string what = "JNIEnv->" + EntryName(i) + "(" + Hex(h) + "): ";
      if (h < base || h >= end) {
        Note(what + (weak ? "not a weak global reference" : "not a global reference") + " — ignored, as ART does");
        return true;
      }
      HostRef r = 0;
      {
        std::lock_guard<std::mutex> lk(globals_mu_);
        std::vector<HostRef>& table = weak ? weaks_ : globals_;
        const std::size_t n = (h - base) / 4u;
        if (n >= table.size() || table[n] == 0) {
          Note(what + "already deleted — ignored, as ART does");
          return true;
        }
        r = table[n];
        table[n] = 0;
        (weak ? weak_free_ : global_free_).push_back(n);
      }
      if (weak) {
        java_.DeleteWeakGlobalRef(r);
      } else {
        java_.DeleteGlobalRef(r);
      }
      return true;
    }
    case 23: {  // DeleteLocalRef
      const GuestAddr h = s.r[1];
      if (h < kLocalBase) return true;  // null, a class handle, or (a guest bug ART also tolerates) a global
      if (!ref(1, &a)) {  // deleted, stale, or another thread's: noted and ignored, as ART does
        Note("JNIEnv->DeleteLocalRef(" + Hex(h) + "): " + why + " — ignored, as ART does");
        return true;
      }
      LocalTable* t = Locals(&why);
      if (t == nullptr) return refuse(why);
      const std::uint32_t n = (h >> 6) & 0x3fffu;
      if (n >= t->refs.size() || t->refs[n] == 0 || t->serials[n] != ((h >> 2) & 0xfu)) {
        // A ghost (a returned native's argument): ART's DeleteLocalRef of an argument does nothing either.
        Note("JNIEnv->DeleteLocalRef of a returned native's argument — ignored, as ART does");
        return true;
      }
      t->refs[n] = 0;
      // Reusable in the frame it belongs to; a slot below the current frame waits for its own frame to pop.
      if (t->frames.empty() || n >= t->frames.back()) t->free.back().push_back(n);
      shared_->live.fetch_sub(1, std::memory_order_relaxed);
      java_.DeleteLocalRef(a);
      return true;
    }
    case 24:  // IsSameObject
      if (!ref(1, &a) || !ref(2, &b)) return refuse(why);
      s.r[0] = java_.IsSameObject(a, b) ? 1u : 0u;
      return true;
    case 25:  // NewLocalRef
      if (!ref(1, &a)) return refuse(why);
      s.r[0] = a == 0 ? 0u : ToGuestLocal(java_.NewLocalRef(a), &why);
      if (a != 0 && s.r[0] == 0) return refuse(why);
      return true;
    case 26:  // EnsureLocalCapacity: the guest table grows on demand
      s.r[0] = 0;
      return true;
    case 27:  // AllocObject
      if (!ref(1, &a)) return refuse(why);
      if (a == 0) return refuse("null class");
      s.r[0] = ToGuestLocal(java_.AllocObject(a), &why);
      return true;
    case 31:  // GetObjectClass
      if (!ref(1, &a)) return refuse(why);
      if (a == 0) return refuse("null object");
      s.r[0] = ClassHandle(java_.GetObjectClass(a), true, &why);
      return true;
    case 32:  // IsInstanceOf
      if (!ref(1, &a) || !ref(2, &b)) return refuse(why);
      if (b == 0) return refuse("null class");
      s.r[0] = java_.IsInstanceOf(a, b) ? 1u : 0u;
      return true;
    case 33:     // GetMethodID
    case 94:     // GetFieldID
    case 113:    // GetStaticMethodID
    case 144: {  // GetStaticFieldID
      const bool is_field = i == 94 || i == 144;
      const bool is_static = i == 113 || i == 144;
      std::string name, sig;
      if (!ref(1, &a) || !name_arg(2, &name) || !name_arg(3, &sig)) return refuse(why);
      if (a == 0) return refuse("null class");
      const HostId id = is_field ? java_.GetFieldID(a, name, sig, is_static) : java_.GetMethodID(a, name, sig, is_static);
      s.r[0] = IdHandle(id, is_field, is_static, name, sig, &why);
      if (id != 0 && s.r[0] == 0) return refuse(why);
      return true;
    }
    case 226: {  // NewWeakGlobalRef
      if (!ref(1, &a)) return refuse(why);
      if (a == 0) {
        s.r[0] = 0;
        return true;
      }
      const HostRef w = java_.NewWeakGlobalRef(a);
      std::lock_guard<std::mutex> lk(globals_mu_);
      std::size_t n = 0;
      if (!weak_free_.empty()) {
        n = weak_free_.back();
        weak_free_.pop_back();
        weaks_[n] = w;
      } else {
        if (weaks_.size() >= (kGlobalBase - kWeakBase) / 4u) return refuse("the weak global reference table is full");
        weaks_.push_back(w);
        n = weaks_.size() - 1;
      }
      s.r[0] = kWeakBase + 4u * static_cast<std::uint32_t>(n);
      return true;
    }
    case 228:  // ExceptionCheck
      s.r[0] = java_.ExceptionCheck() ? 1u : 0u;
      return true;
    case 232: {  // GetObjectRefType: JNIInvalidRefType 0, Local 1, Global 2, WeakGlobal 3
      // ART answers from the reference's kind bits alone and does not check that it is still live; so does this.
      // A class handle is what FindClass/GetObjectClass hand out — locals, on a device.
      const GuestAddr h = s.r[1];
      s.r[0] = h == 0 || (h & 3u) != 0 || h < kClassBase ? 0u
               : h >= kLocalBase                         ? 1u
               : h >= kGlobalBase                        ? 2u
               : h >= kWeakBase                          ? 3u
                                                         : 1u;
      return true;
    }
    default: break;
  }
  if ((i >= 28 && i <= 30) || (i >= 34 && i <= 93) || (i >= 114 && i <= 143)) {
    return CallFamily(i, s, &why) ? true : refuse(why);
  }
  if ((i >= 95 && i <= 112) || (i >= 145 && i <= 162)) return FieldFamily(i, s, &why) ? true : refuse(why);
  bool ok = false;
  if ((i >= 163 && i <= 170) || i == 220 || i == 221 || i == 224 || i == 225) {
    ok = StringEntry(i, s, &why);
  } else if ((i >= 171 && i <= 214) || i == 222 || i == 223) {
    ok = ArrayEntry(i, s, &why);
  } else {
    ok = OtherEntry(i, s, &why);
  }
  return ok ? true : refuse(why);
}


// ------------------------------------------------------------------------------------------------- Task 2

std::vector<std::string> ForwardingJni::notes() const {
  std::lock_guard<std::mutex> lk(notes_mu_);
  return notes_;
}

namespace {

/// The platform's device-identifier reads. From API 29 they throw SecurityException for any app that is not
/// privileged; a game repacked to a modern targetSdk hits that where its original target answered without throwing.
bool IsDeviceIdentifierRead(const std::string& cls, const std::string& method) {
  if (cls == "android.telephony.TelephonyManager") {
    return method == "getDeviceId" || method == "getImei" || method == "getMeid" || method == "getSubscriberId" ||
           method == "getSimSerialNumber";
  }
  return cls == "android.os.Build" && method == "getSerial";
}

/// Frames the platform owns. The search for an identifier read stops at the first frame that is not one of these: an
/// app method that happens to share a name is not the platform's read.
bool IsPlatformFrame(const std::string& cls) {
  for (const char* p : {"android.", "com.android.", "java.", "javax.", "dalvik.", "libcore.", "sun."}) {
    if (cls.rfind(p, 0) == 0) return true;
  }
  return false;
}

std::string Ascii(const std::u16string& s) {
  std::string out;
  for (char16_t c : s) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  return out;
}

}  // namespace

bool ForwardingJni::MaskIdentifierRefusal(const std::string& called) {
  const HostRef exc = java_.ExceptionOccurred();
  if (exc == 0) return false;
  // Nothing else may be called with an exception pending: take it off, look, and put it back unless it matches.
  java_.ExceptionClear();
  bool match = false;
  std::string where;
  const HostRef sec = java_.FindClass("java/lang/SecurityException");
  if (sec != 0 && java_.IsInstanceOf(exc, sec)) {
    const HostRef thr = java_.FindClass("java/lang/Throwable");
    const HostRef ste = java_.FindClass("java/lang/StackTraceElement");
    const HostId trace = thr ? java_.GetMethodID(thr, "getStackTrace", "()[Ljava/lang/StackTraceElement;", false) : 0;
    const HostId cname = ste ? java_.GetMethodID(ste, "getClassName", "()Ljava/lang/String;", false) : 0;
    const HostId mname = ste ? java_.GetMethodID(ste, "getMethodName", "()Ljava/lang/String;", false) : 0;
    const HostRef frames =
        trace && cname && mname ? static_cast<HostRef>(java_.Call('L', false, exc, 0, trace, {})) : 0;
    const std::int32_t n = frames != 0 && !java_.ExceptionCheck() ? java_.GetArrayLength(frames) : 0;
    for (std::int32_t k = 0; k < n && k < 32 && !match; ++k) {
      const HostRef f = java_.GetObjectArrayElement(frames, k);
      if (f == 0 || java_.ExceptionCheck()) break;
      const HostRef cs = static_cast<HostRef>(java_.Call('L', false, f, 0, cname, {}));
      const HostRef ms = static_cast<HostRef>(java_.Call('L', false, f, 0, mname, {}));
      const std::string c = cs ? Ascii(java_.GetStringChars(cs)) : std::string{};
      const std::string m = ms ? Ascii(java_.GetStringChars(ms)) : std::string{};
      if (cs) java_.DeleteLocalRef(cs);
      if (ms) java_.DeleteLocalRef(ms);
      java_.DeleteLocalRef(f);
      if (!IsPlatformFrame(c)) break;  // the app's own code: past the platform's part of the stack
      if (IsDeviceIdentifierRead(c, m)) {
        match = true;
        where = c + "." + m;
      }
    }
    if (frames != 0) java_.DeleteLocalRef(frames);
    if (java_.ExceptionCheck()) java_.ExceptionClear();  // only our own inspection could have raised this
  }
  if (!match) {
    java_.Throw(exc);
    java_.DeleteLocalRef(exc);
    return false;
  }
  java_.DeleteLocalRef(exc);
  Note(called + ": " + where + " refused a device identifier (SecurityException, targetSdk 29+); the guest is answered "
       "null, as a phone without the permission answered the game");
  return true;
}

void ForwardingJni::Note(const std::string& note) {
  std::lock_guard<std::mutex> lk(notes_mu_);
  for (const std::string& n : notes_) {
    if (n == note) return;
  }
  if (notes_.size() < kMaxNotes) notes_.push_back(note);
}

bool ForwardingJni::ThrowNamed(const char* cls, const std::string& message) {
  const HostRef c = java_.FindClass(cls);
  if (c == 0) return false;
  java_.ThrowNew(c, message);
  java_.DeleteLocalRef(c);
  return true;
}

GuestAddr ForwardingJni::Alloc(std::uint32_t bytes, std::string* err) {
  // Power-of-two blocks from 16 bytes, carved from 1 MiB chunks; anything over 64 KiB gets whole pages of its own.
  // Freed blocks go to a free list by size and are reused; pages are never returned.
  std::uint32_t size = 16;
  while (size < bytes && size < (64u << 10)) size <<= 1;
  if (bytes > (64u << 10)) size = (bytes + mem::kGuestPage - 1) & ~(mem::kGuestPage - 1);
  {
    std::lock_guard<std::mutex> lk(arena_mu_);
    auto& free = free_by_size_[size];
    if (!free.empty()) {
      const GuestAddr at = free.back();
      free.pop_back();
      block_size_[at] = size;
      return at;
    }
    if (size <= (64u << 10) && chunk_next_ + size <= chunk_end_ && chunk_next_ != 0) {
      const GuestAddr at = chunk_next_;
      chunk_next_ += size;
      block_size_[at] = size;
      return at;
    }
  }
  if (!alloc_) {
    if (err != nullptr) *err = "no guest allocator is set on this JNI bridge, so it has nowhere to put a copy";
    return 0;
  }
  // Mapping takes the GGL: never with arena_mu_ held (a guest thread holding the GGL may be waiting for it).
  const std::uint32_t want = size > (64u << 10) ? size : (1u << 20);
  const GuestAddr got = alloc_(want, err);
  if (got == 0) return 0;
  std::lock_guard<std::mutex> lk(arena_mu_);
  if (size > (64u << 10)) {
    block_size_[got] = size;
    return got;
  }
  // The old chunk's tail is abandoned (at most 64 KiB); the new chunk serves this block first.
  chunk_next_ = got + size;
  chunk_end_ = got + want;
  block_size_[got] = size;
  return got;
}

void ForwardingJni::SyncShadows() {
  // Three-way, per 64-byte block, against the bytes as last synced: a block only the guest changed goes to the host,
  // one only Java changed goes to the guest, and one both changed keeps the guest's (the native is the writer of
  // the moment). So a Java writer and a guest writer on different parts of one buffer never undo each other.
  constexpr std::size_t kBlock = 64;
  if (small_shadows_.load(std::memory_order_relaxed) == 0) return;
  std::lock_guard<std::mutex> lk(shadows_mu_);
  for (auto& [host_ptr, sh] : shadows_) {
    if (sh.snap.empty()) continue;
    auto* host = static_cast<std::uint8_t*>(host_ptr);
    auto* guest = static_cast<std::uint8_t*>(memory_.Host(sh.guest));
    if (guest == nullptr) continue;
    for (std::size_t at = 0; at < sh.snap.size(); at += kBlock) {
      const std::size_t n = std::min(kBlock, sh.snap.size() - at);
      std::uint8_t* snap = sh.snap.data() + at;
      const bool guest_changed = std::memcmp(guest + at, snap, n) != 0;
      if (guest_changed) {
        std::memcpy(host + at, guest + at, n);
        std::memcpy(snap, guest + at, n);
      } else if (std::memcmp(host + at, snap, n) != 0) {
        std::memcpy(guest + at, host + at, n);
        std::memcpy(snap, host + at, n);
      }
    }
  }
}

void ForwardingJni::Free(GuestAddr at) {
  std::lock_guard<std::mutex> lk(arena_mu_);
  const auto it = block_size_.find(at);
  if (it == block_size_.end()) return;
  free_by_size_[it->second].push_back(at);
  block_size_.erase(it);
}

GuestAddr ForwardingJni::CopyOut(const void* data, std::uint32_t bytes, const Copy& rec, std::string* err) {
  const GuestAddr at = Alloc(bytes == 0 ? 1 : bytes, err);
  if (at == 0) return 0;
  if (bytes != 0 && !memory_.Write(at, data, bytes)) {
    Free(at);
    if (err != nullptr) *err = "writing the copy at " + Hex(at);
    return 0;
  }
  std::lock_guard<std::mutex> lk(arena_mu_);
  copies_[at] = rec;
  return at;
}

bool ForwardingJni::StringEntry(std::uint32_t i, exec::MachineState& s, std::string* err) {
  const auto write_is_copy = [&](unsigned arg) {
    const std::uint8_t one = 1;
    if (s.r[arg] != 0) memory_.Write(s.r[arg], &one, 1);
  };
  if (i == 167) {  // NewStringUTF(bytes)
    if (s.r[1] == 0) {
      s.r[0] = 0;
      return true;
    }
    std::string bytes;
    if (!ReadCString(s.r[1], &bytes, err)) return false;
    bool sanitized = false;
    const std::u16string chars = FromMutf8(bytes, &sanitized);
    if (sanitized) {
      Note("NewStringUTF: the guest's bytes were not valid Modified UTF-8; the bad bytes were sanitized to U+FFFD "
           "(first seen: " + std::to_string(bytes.size()) + " bytes)");
    }
    s.r[0] = ToGuestLocal(java_.NewString(chars), err);
    return true;
  }
  if (i == 163) {  // NewString(const jchar*, len)
    const std::int32_t n = static_cast<std::int32_t>(s.r[2]);
    if (n < 0) return ThrowNamed("java/lang/NegativeArraySizeException", std::to_string(n)), s.r[0] = 0, true;
    std::u16string chars(static_cast<std::size_t>(n), u'\0');
    if (n > 0 && !memory_.Read(s.r[1], chars.data(), static_cast<std::size_t>(n) * 2)) {
      *err = "the chars at " + Hex(s.r[1]) + " are unreadable";
      return false;
    }
    s.r[0] = ToGuestLocal(java_.NewString(chars), err);
    return true;
  }
  if (i == 166 || i == 170 || i == 225) {  // ReleaseStringChars / ReleaseStringUTFChars / ReleaseStringCritical
    Copy rec;
    {
      std::lock_guard<std::mutex> lk(arena_mu_);
      const auto it = copies_.find(s.r[2]);
      if (it == copies_.end() || it->second.kind != 0) {
        *err = Hex(s.r[2]) + " is not a string copy this bridge handed out (released twice?)";
        return false;
      }
      rec = it->second;
      copies_.erase(it);
    }
    java_.DeleteGlobalRef(rec.pinned);
    Free(s.r[2]);
    return true;
  }
  HostRef str = 0;
  str = FromGuest(s.r[1], err);
  if (!err->empty()) return false;
  if (str == 0) {
    *err = "null string";
    return false;
  }
  const std::u16string chars = java_.GetStringChars(str);
  switch (i) {
    case 164:  // GetStringLength
      s.r[0] = static_cast<std::uint32_t>(chars.size());
      return true;
    case 168:  // GetStringUTFLength
      s.r[0] = static_cast<std::uint32_t>(Mutf8(chars.data(), chars.size()).size());
      return true;
    case 165:     // GetStringChars
    case 224: {   // GetStringCritical
      Copy rec;
      rec.pinned = java_.NewGlobalRef(str);
      rec.length = static_cast<std::int32_t>(chars.size());
      rec.bytes = static_cast<std::uint32_t>(chars.size() * 2);
      s.r[0] = CopyOut(chars.data(), rec.bytes, rec, err);
      if (s.r[0] == 0) return false;
      write_is_copy(2);
      return true;
    }
    case 169: {  // GetStringUTFChars
      const std::string m = Mutf8(chars.data(), chars.size());
      std::string z = m;
      z.push_back('\0');
      Copy rec;
      rec.pinned = java_.NewGlobalRef(str);
      rec.bytes = static_cast<std::uint32_t>(z.size());
      s.r[0] = CopyOut(z.data(), rec.bytes, rec, err);
      if (s.r[0] == 0) return false;
      write_is_copy(2);
      return true;
    }
    case 220:     // GetStringRegion(s, start, len, jchar* buf)
    case 221: {   // GetStringUTFRegion(s, start, len, char* buf)
      const std::int32_t start = static_cast<std::int32_t>(s.r[2]);
      const std::int32_t len = static_cast<std::int32_t>(s.r[3]);
      std::uint32_t buf = 0;
      if (!jni::ArgWord(s, memory_, 4, &buf)) {
        *err = "the buffer argument is unreadable";
        return false;
      }
      if (start < 0 || len < 0 || static_cast<std::size_t>(start) + static_cast<std::size_t>(len) > chars.size()) {
        ThrowNamed("java/lang/StringIndexOutOfBoundsException",
                   "start " + std::to_string(start) + ", length " + std::to_string(len) + ", string length " +
                       std::to_string(chars.size()));
        return true;
      }
      bool wrote = true;
      if (i == 220) {
        wrote = len == 0 || memory_.Write(buf, chars.data() + start, static_cast<std::size_t>(len) * 2);
      } else {
        // Then a NUL, as the device's ART writes one (oracle-jnibridge.txt's strings row; the JNI spec is silent).
        const std::string m = Mutf8(chars.data() + start, static_cast<std::size_t>(len));
        wrote = memory_.Write(buf, m.c_str(), m.size() + 1);
      }
      if (!wrote) {
        *err = "the buffer at " + Hex(buf) + " is not writable";
        return false;
      }
      return true;
    }
    default: break;
  }
  *err = "not a string entry";
  return false;
}

bool ForwardingJni::ArrayEntry(std::uint32_t i, exec::MachineState& s, std::string* err) {
  if (i >= 175 && i <= 182) {  // New<T>Array(len)
    s.r[0] = ToGuestLocal(java_.NewArray(kArrayKinds[i - 175], static_cast<std::int32_t>(s.r[1]), 0, 0), err);
    return true;
  }
  if (i == 172) {  // NewObjectArray(len, cls, init)
    HostRef cls = FromGuest(s.r[2], err);
    if (!err->empty()) return false;
    HostRef init = FromGuest(s.r[3], err);
    if (!err->empty()) return false;
    if (cls == 0) {
      *err = "null element class";
      return false;
    }
    s.r[0] = ToGuestLocal(java_.NewArray('L', static_cast<std::int32_t>(s.r[1]), cls, init), err);
    return true;
  }
  // Releases: the guest address of the copy identifies it; the array argument must be the one it came from.
  if ((i >= 191 && i <= 198) || i == 223) {
    const GuestAddr at = s.r[2];
    const std::uint32_t mode = s.r[3];
    Copy rec;
    {
      std::lock_guard<std::mutex> lk(arena_mu_);
      const auto it = copies_.find(at);
      if (it == copies_.end() || it->second.kind == 0) {
        *err = Hex(at) + " is not an array copy this bridge handed out (released twice?)";
        return false;
      }
      rec = it->second;
      if (mode != 1) copies_.erase(it);  // JNI_COMMIT keeps it
    }
    if (mode != 2) {  // 0 and JNI_COMMIT copy back; JNI_ABORT does not
      std::vector<std::uint8_t> bytes(rec.bytes);
      if (rec.bytes != 0 && !memory_.Read(at, bytes.data(), bytes.size())) {
        *err = "the copy at " + Hex(at) + " is unreadable";
        return false;
      }
      java_.SetArrayRegion(rec.kind, rec.pinned, 0, rec.length, bytes.data());
    }
    if (mode != 1) {
      java_.DeleteGlobalRef(rec.pinned);
      Free(at);
    }
    return true;
  }
  HostRef arr = FromGuest(s.r[1], err);
  if (!err->empty()) return false;
  if (arr == 0) {
    *err = "null array";
    return false;
  }
  if (i == 171) {  // GetArrayLength
    s.r[0] = static_cast<std::uint32_t>(java_.GetArrayLength(arr));
    return true;
  }
  if (i == 173) {  // GetObjectArrayElement(a, index)
    s.r[0] = ToGuestLocal(java_.GetObjectArrayElement(arr, static_cast<std::int32_t>(s.r[2])), err);
    return true;
  }
  if (i == 174) {  // SetObjectArrayElement(a, index, value)
    const HostRef v = FromGuest(s.r[3], err);
    if (!err->empty()) return false;
    java_.SetObjectArrayElement(arr, static_cast<std::int32_t>(s.r[2]), v);
    return true;
  }
  if ((i >= 183 && i <= 190) || i == 222) {  // Get<T>ArrayElements(a, isCopy) / GetPrimitiveArrayCritical
    const char kind = i == 222 ? java_.ArrayKind(arr) : kArrayKinds[i - 183];
    if (kind == 0 || kind == 'L') {
      *err = "not a primitive array";
      return false;
    }
    Copy rec;
    rec.kind = kind;
    rec.length = java_.GetArrayLength(arr);
    rec.bytes = static_cast<std::uint32_t>(rec.length) * ElemSize(kind);
    std::vector<std::uint8_t> bytes(rec.bytes);
    if (!java_.GetArrayRegion(kind, arr, 0, rec.length, bytes.data())) {
      s.r[0] = 0;
      return true;  // the array is not of that kind: ART would have aborted; JNI leaves the exception
    }
    rec.pinned = java_.NewGlobalRef(arr);
    s.r[0] = CopyOut(bytes.data(), rec.bytes, rec, err);
    if (s.r[0] == 0) return false;
    if (s.r[2] != 0) {
      const std::uint8_t one = 1;
      memory_.Write(s.r[2], &one, 1);
    }
    return true;
  }
  if (i >= 199 && i <= 214) {  // Get/Set<T>ArrayRegion(a, start, len, buf)
    const bool set = i >= 207;
    const char kind = kArrayKinds[i - (set ? 207 : 199)];
    const std::int32_t start = static_cast<std::int32_t>(s.r[2]);
    const std::int32_t len = static_cast<std::int32_t>(s.r[3]);
    std::uint32_t buf = 0;
    if (!jni::ArgWord(s, memory_, 4, &buf)) {
      *err = "the buffer argument is unreadable";
      return false;
    }
    if (len <= 0 || start < 0) {
      if (len < 0 || start < 0) {
        ThrowNamed("java/lang/ArrayIndexOutOfBoundsException",
                   "start " + std::to_string(start) + ", length " + std::to_string(len));
      }
      return true;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(len) * ElemSize(kind));
    if (set) {
      if (!memory_.Read(buf, bytes.data(), bytes.size())) {
        *err = "the buffer at " + Hex(buf) + " is unreadable";
        return false;
      }
      java_.SetArrayRegion(kind, arr, start, len, bytes.data());  // out of range: Java's exception, pending
      return true;
    }
    if (java_.GetArrayRegion(kind, arr, start, len, bytes.data()) && !memory_.Write(buf, bytes.data(), bytes.size())) {
      *err = "the buffer at " + Hex(buf) + " is not writable";
      return false;
    }
    return true;
  }
  *err = "not an array entry";
  return false;
}

bool ForwardingJni::OtherEntry(std::uint32_t i, exec::MachineState& s, std::string* err) {
  const auto ref = [&](unsigned arg, HostRef* out) {
    *out = FromGuest(s.r[arg], err);
    return err->empty();
  };
  HostRef a = 0, b = 0;
  switch (i) {
    case 5:  // DefineClass: ART does not support it either, and answers null
      Note("DefineClass is not supported on Android; answered null, as ART does");
      s.r[0] = 0;
      return true;
    case 7:    // FromReflectedMethod
    case 8: {  // FromReflectedField
      if (!ref(1, &a)) return false;
      if (a == 0) {
        *err = "null member";
        return false;
      }
      std::string name, sig;
      bool is_static = false;
      const bool is_field = i == 8;
      const HostId id = java_.FromReflected(a, is_field, &name, &sig, &is_static);
      s.r[0] = IdHandle(id, is_field, is_static, name, sig, err);
      return id == 0 || s.r[0] != 0;
    }
    case 9:     // ToReflectedMethod(cls, mid, isStatic)
    case 12: {  // ToReflectedField(cls, fid, isStatic)
      if (!ref(1, &a)) return false;
      IdRec rec;
      {
        const IdRec* r = Id(s.r[2], err);
        if (r == nullptr) return false;
        rec = *r;
      }
      s.r[0] = ToGuestLocal(java_.ToReflected(a, rec.id, i == 12, (s.r[3] & 0xffu) != 0), err);
      return true;
    }
    case 13:  // Throw(throwable)
      if (!ref(1, &a)) return false;
      s.r[0] = static_cast<std::uint32_t>(java_.Throw(a));
      return true;
    case 14: {  // ThrowNew(cls, message)
      if (!ref(1, &a)) return false;
      if (a == 0) {
        *err = "null class";
        return false;
      }
      std::string message;
      if (s.r[2] != 0 && !ReadCString(s.r[2], &message, err)) return false;
      s.r[0] = static_cast<std::uint32_t>(java_.ThrowNew(a, message));
      return true;
    }
    case 15:  // ExceptionOccurred
      s.r[0] = ToGuestLocal(java_.ExceptionOccurred(), err);
      return true;
    case 16:  // ExceptionDescribe
      java_.ExceptionDescribe();
      return true;
    case 17:  // ExceptionClear
      java_.ExceptionClear();
      return true;
    case 18: {  // FatalError: ART aborts the process; here the guest call stops, named
      std::string message;
      ReadCString(s.r[1], &message, nullptr);
      *err = "the guest called FatalError(\"" + message + "\") — on a device ART aborts the process here";
      return false;
    }
    case 19: {  // PushLocalFrame(capacity)
      const std::int32_t rc = java_.PushLocalFrame(static_cast<std::int32_t>(s.r[1]));
      if (rc == 0) PushFrame();
      s.r[0] = static_cast<std::uint32_t>(rc);
      return true;
    }
    case 20: {  // PopLocalFrame(result)
      if (!ref(1, &a)) return false;
      LocalTable* t = Locals(err);
      if (t == nullptr) return false;
      if (t->frames.empty()) {
        *err = "PopLocalFrame without a matching PushLocalFrame";
        return false;
      }
      PopFrame(0);
      s.r[0] = ToGuestLocal(java_.PopLocalFrame(a), err);
      return true;
    }
    case 215: {  // RegisterNatives(cls, const JNINativeMethod*, n)
      if (!ref(1, &a)) return false;
      if (a == 0) {
        *err = "null class";
        return false;
      }
      if (!binder_) {
        *err = "no native binder is set on this JNI bridge, so a guest native cannot be bound to a Java method";
        return false;
      }
      std::vector<HostJava::NativeBinding> bindings;
      for (std::uint32_t k = 0; k < s.r[3]; ++k) {
        std::uint32_t words[3] = {};
        if (!memory_.Read(s.r[2] + 12 * k, words, sizeof words)) {
          *err = "JNINativeMethod " + std::to_string(k) + " at " + Hex(s.r[2] + 12 * k) + " is unreadable";
          return false;
        }
        HostJava::NativeBinding nb;
        if (!ReadCString(words[0], &nb.name, err) || !ReadCString(words[1], &nb.sig, err)) return false;
        nb.host_fn = binder_(words[2], nb.name, nb.sig, err);
        if (nb.host_fn == nullptr) {
          *err = "binding " + nb.name + nb.sig + ": " + *err;
          return false;
        }
        bindings.push_back(std::move(nb));
      }
      s.r[0] = static_cast<std::uint32_t>(java_.RegisterNatives(a, bindings));
      return true;
    }
    case 216:  // UnregisterNatives
      if (!ref(1, &a)) return false;
      s.r[0] = static_cast<std::uint32_t>(java_.UnregisterNatives(a));
      return true;
    case 217:  // MonitorEnter
    case 218:  // MonitorExit
      if (!ref(1, &a)) return false;
      if (a == 0) {
        *err = "null object";
        return false;
      }
      s.r[0] = static_cast<std::uint32_t>(i == 217 ? java_.MonitorEnter(a) : java_.MonitorExit(a));
      return true;
    case 219: {  // GetJavaVM(JavaVM**)
      const GuestAddr v = vm();
      if (!memory_.Write(s.r[1], &v, 4)) {
        *err = "the JavaVM** at " + Hex(s.r[1]) + " is not writable";
        return false;
      }
      s.r[0] = 0;
      return true;
    }
    case 229: {  // NewDirectByteBuffer(void* address, jlong capacity) — capacity in r2:r3
      const std::uint64_t cap = static_cast<std::uint64_t>(s.r[2]) | (static_cast<std::uint64_t>(s.r[3]) << 32);
      if (s.r[1] == 0 || s.r[1] >= memory_.window_bytes()) {
        *err = Hex(s.r[1]) + " is not guest memory";
        return false;
      }
      void* host = static_cast<char*>(const_cast<void*>(memory_.window_base())) + s.r[1];
      s.r[0] = ToGuestLocal(java_.NewDirectByteBuffer(host, static_cast<std::int64_t>(cap)), err);
      return true;
    }
    case 230: {  // GetDirectBufferAddress
      if (!ref(1, &a)) return false;
      void* p = a == 0 ? nullptr : java_.GetDirectBufferAddress(a);
      if (p == nullptr) {
        s.r[0] = 0;
        return true;
      }
      const auto base = reinterpret_cast<std::uintptr_t>(memory_.window_base());
      const auto at = reinterpret_cast<std::uintptr_t>(p);
      if (at >= base && at - base < memory_.window_bytes()) {
        s.r[0] = static_cast<std::uint32_t>(at - base);
        return true;
      }
      // Host memory (ByteBuffer.allocateDirect, a mapped file): the guest works on a shadow in its window, refreshed
      // from the host here; a small one is also merged both ways at every JNI transition (SyncShadows).
      const std::int64_t cap = java_.GetDirectBufferCapacity(a);
      if (cap < 0 || cap > (std::int64_t{1} << 30)) {
        *err = "a direct buffer of capacity " + std::to_string(cap) + " outside the guest window cannot be shadowed";
        return false;
      }
      std::unique_lock<std::mutex> lk(shadows_mu_);
      auto it = shadows_.find(p);
      if (it == shadows_.end() || it->second.capacity != static_cast<std::uint64_t>(cap)) {
        lk.unlock();
        if (!alloc_) {
          *err = "a direct buffer outside the guest window needs guest memory for its shadow, and no allocator is set";
          return false;
        }
        const GuestAddr g = alloc_(static_cast<std::uint32_t>(cap == 0 ? 1 : cap), err);
        if (g == 0) return false;
        lk.lock();
        Shadow& sh = shadows_[p];
        if (!sh.snap.empty()) small_shadows_.fetch_sub(1, std::memory_order_relaxed);  // a reused address
        sh = Shadow{static_cast<std::uint64_t>(cap), g, {}};
        if (cap <= kSmallShadow) {
          sh.snap.assign(static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + cap);
          if (cap != 0) small_shadows_.fetch_add(1, std::memory_order_relaxed);
        }
        memory_.Write(g, p, static_cast<std::size_t>(cap));
        Note("GetDirectBufferAddress: a " + std::to_string(cap) + "-byte direct buffer in host memory is shadowed at " +
             Hex(g) + (cap <= kSmallShadow ? " (merged at every JNI transition)" : " (refreshed at each GetDirectBufferAddress)"));
        s.r[0] = g;
        return true;
      }
      if (it->second.snap.empty()) memory_.Write(it->second.guest, p, static_cast<std::size_t>(cap));  // large: refresh
      s.r[0] = it->second.guest;
      return true;
    }
    case 231:  // GetDirectBufferCapacity
      if (!ref(1, &a)) return false;
      Return64(s, static_cast<std::uint64_t>(a == 0 ? -1 : java_.GetDirectBufferCapacity(a)));
      return true;
    default: break;
  }
  (void)b;
  *err = "entry " + std::to_string(i) + " is not forwarded";
  return false;
}

bool ForwardingJni::VmSlot(std::uint32_t i, exec::MachineState& s, std::string* err) {
  constexpr std::int32_t kOk = 0, kErr = -1, kDetached = -2, kVersion = -3;
  const auto put_env = [&](GuestAddr where, GuestAddr value) {
    if (where != 0 && !memory_.Write(where, &value, 4)) {
      if (err != nullptr) *err = "JavaVM: the JNIEnv** at " + Hex(where) + " is not writable";
      return false;
    }
    return true;
  };
  switch (i) {
    case 3:  // DestroyJavaVM: an app never may
      Note("JavaVM->DestroyJavaVM called; answered JNI_ERR (an app cannot destroy the VM it runs in)");
      s.r[0] = static_cast<std::uint32_t>(kErr);
      return true;
    case 4:    // AttachCurrentThread(vm, JNIEnv**, args)
    case 7: {  // AttachCurrentThreadAsDaemon
      if (!java_.IsAttached() && !java_.AttachCurrentThread(i == 7)) {
        s.r[0] = static_cast<std::uint32_t>(kErr);
        return true;
      }
      if (!put_env(s.r[1], env())) return false;
      s.r[0] = kOk;
      return true;
    }
    case 5: {  // DetachCurrentThread
      const bool was = java_.IsAttached();
      java_.DetachCurrentThread();
      // A detach that happened frees the thread's locals on ART, so a worker may attach/make locals/detach per
      // event without DeleteLocalRef: empty this thread's table (its serials stay, so its old handles are stale).
      std::string e;
      if (was && !java_.IsAttached()) {
        if (LocalTable* t = Locals(&e)) {
          std::size_t live = 0;
          for (const HostRef r : t->refs) live += r != 0;
          t->refs.clear();
          t->frames.clear();
          t->free.assign(1, {});
          t->args_end.clear();
          shared_->live.fetch_sub(live, std::memory_order_relaxed);
        }
      }
      s.r[0] = kOk;
      return true;
    }
    case 6: {  // GetEnv(vm, void**, version)
      const std::uint32_t v = s.r[2];
      if (!java_.IsAttached()) {
        if (!put_env(s.r[1], 0)) return false;
        s.r[0] = static_cast<std::uint32_t>(kDetached);
        return true;
      }
      const bool known = v == 0x00010001u || v == 0x00010002u || v == 0x00010004u || v == 0x00010006u;
      if (!put_env(s.r[1], known ? env() : 0)) return false;
      s.r[0] = static_cast<std::uint32_t>(known ? kOk : kVersion);
      return true;
    }
    default: break;
  }
  if (err != nullptr) *err = "JavaVM entry " + std::to_string(i) + " is reserved";
  return false;
}
}  // namespace rb::abi
