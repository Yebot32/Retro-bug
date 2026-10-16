// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "rb/abi/jni_forward.h"

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
        kEnvSlotBase + i, [this, i](exec::MachineState& s, std::string* e) { return Slot(i, s, e); },
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
}

// ------------------------------------------------------------------------------------------------- handles

ForwardingJni::LocalTable* ForwardingJni::Locals(std::string* err) {
  // Per host thread and per bridge instance (several bridges can live in one process, e.g. a test's).
  static thread_local std::vector<std::pair<std::uint64_t, std::unique_ptr<LocalTable>>> tables;
  for (auto& [serial, t] : tables) {
    if (serial == serial_) return t.get();
  }
  const std::uint32_t thread = next_thread_.fetch_add(1, std::memory_order_relaxed);
  if (thread >= kMaxThreads) {
    if (err != nullptr) *err = "more than " + std::to_string(kMaxThreads) + " host threads used this JNI bridge";
    return nullptr;
  }
  auto t = std::make_unique<LocalTable>();
  t->thread = thread;
  tables.emplace_back(serial_, std::move(t));
  return tables.back().second.get();
}

GuestAddr ForwardingJni::ToGuestLocal(HostRef r, std::string* err) {
  if (r == 0) return 0;
  LocalTable* t = Locals(err);
  if (t == nullptr) return 0;
  const std::size_t n = t->refs.size();
  if (n >= kMaxLocals) {
    if (err != nullptr) {
      *err = "more than " + std::to_string(kMaxLocals) + " local references in this thread's frames (a guest that "
             "never deletes its locals, or a native that never returns)";
    }
    return 0;
  }
  t->refs.push_back(r);
  live_locals_.fetch_add(1, std::memory_order_relaxed);
  return kLocalBase | (t->thread << 18) | (static_cast<std::uint32_t>(n) << 2);
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
    const std::uint32_t thread = (h >> 18) & 0x3ffu;
    const std::uint32_t n = (h >> 2) & 0xffffu;
    if (thread != t->thread) {
      return fail(Hex(h) + " is another thread's local reference (thread " + std::to_string(thread) +
                  "; this is thread " + std::to_string(t->thread) + ")");
    }
    if (n >= t->refs.size() || t->refs[n] == 0) {
      return fail(Hex(h) + " is a stale local reference (deleted, or the frame that made it has returned)");
    }
    return t->refs[n];
  }
  if (h >= kGlobalBase || h >= kWeakBase) {
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
  if (LocalTable* t = Locals(&e)) t->frames.push_back(t->refs.size());
}

HostRef ForwardingJni::PopFrame(GuestAddr result, std::string* err) {
  const HostRef r = FromGuest(result, err);
  std::string e;
  LocalTable* t = Locals(&e);
  if (t == nullptr || t->frames.empty()) return r;
  const std::size_t start = t->frames.back();
  t->frames.pop_back();
  std::size_t live = 0;
  for (std::size_t n = start; n < t->refs.size(); ++n) live += t->refs[n] != 0;
  t->refs.resize(start);
  live_locals_.fetch_sub(live, std::memory_order_relaxed);
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
  return &ids_[(h - kIdBase) / 4u];  // std::vector of records: stable only while ids_ does not grow — copied by callers
}

bool ForwardingJni::ReadCString(GuestAddr at, std::string* out, std::string* err) const {
  out->clear();
  for (std::uint32_t n = 0; n < 4096; ++n) {
    char c = 0;
    if (!memory_.Read(at + n, &c, 1)) {
      if (err != nullptr) *err = "the string at " + Hex(at) + " is unreadable at byte " + std::to_string(n);
      return false;
    }
    if (c == '\0') return true;
    out->push_back(c);
  }
  if (err != nullptr) *err = "the string at " + Hex(at) + " has no NUL in 4096 bytes";
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
      if (h < base || h >= end) return refuse(Hex(h) + (weak ? " is not a weak global reference" : " is not a global reference"));
      HostRef r = 0;
      {
        std::lock_guard<std::mutex> lk(globals_mu_);
        std::vector<HostRef>& table = weak ? weaks_ : globals_;
        const std::size_t n = (h - base) / 4u;
        if (n >= table.size() || table[n] == 0) return refuse(Hex(h) + " is already deleted");
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
      if (!ref(1, &a)) return refuse(why);
      LocalTable* t = Locals(&why);
      if (t == nullptr) return refuse(why);
      t->refs[(h >> 2) & 0xffffu] = 0;
      live_locals_.fetch_sub(1, std::memory_order_relaxed);
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
      const GuestAddr h = s.r[1];
      std::string e;
      const HostRef r = FromGuest(h, &e);
      s.r[0] = (h == 0 || r == 0) ? 0u : h >= kLocalBase ? 1u : h >= kGlobalBase ? 2u : h >= kWeakBase ? 3u : 2u;
      return true;
    }
    default: break;
  }
  if ((i >= 28 && i <= 30) || (i >= 34 && i <= 93) || (i >= 114 && i <= 143)) {
    return CallFamily(i, s, &why) ? true : refuse(why);
  }
  if ((i >= 95 && i <= 112) || (i >= 145 && i <= 162)) return FieldFamily(i, s, &why) ? true : refuse(why);
  return refuse("not forwarded by this bridge yet");
}

}  // namespace rb::abi
