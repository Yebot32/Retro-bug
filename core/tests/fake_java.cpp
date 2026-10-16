// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "fake_java.h"

#include <cstdio>
#include <cstring>

namespace rb::test {

namespace {
constexpr abi::HostId kFieldIdBase = 0x10000;

std::size_t ElemSize(char k) {
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
}  // namespace

// ------------------------------------------------------------------------------------------- test-side helpers

int FakeJava::DefineClass(const std::string& name, const std::string& super_name) {
  std::lock_guard<std::mutex> lk(mu_);
  objects_.push_back(Object{});
  const int self = static_cast<int>(objects_.size()) - 1;
  Class c;
  c.name = name;
  c.self = self;
  for (const auto& [id, k] : classes_) {
    if (k.name == super_name) c.super = id;
  }
  classes_[self] = c;
  return self;
}

void FakeJava::DefineMethod(const std::string& cls, const std::string& name, const std::string& sig, bool is_static,
                            Body body) {
  std::lock_guard<std::mutex> lk(mu_);
  for (Method& m : methods_) {  // redefining replaces the body (a row may swap in its own)
    if (m.body && classes_.count(m.cls) != 0 && classes_[m.cls].name == cls && m.name == name && m.sig == sig &&
        m.is_static == is_static) {
      m.body = std::move(body);
      return;
    }
  }
  for (const auto& [id, k] : classes_) {
    if (k.name == cls) methods_.push_back(Method{id, name, sig, is_static, std::move(body)});
  }
}

void FakeJava::DefineField(const std::string& cls, const std::string& name, const std::string& sig, bool is_static) {
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& [id, k] : classes_) {
    if (k.name == cls) fields_.push_back(Field{id, name, sig, is_static});
  }
}

FakeJava::Ref FakeJava::NewInstance(const std::string& cls) {
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& [id, k] : classes_) {
    if (k.name == cls) return MakeRefLocked(NewObjectLocked(id));
  }
  return 0;
}

FakeJava::Ref FakeJava::NewJavaString(const std::u16string& s) { return NewString(s); }

int FakeJava::ObjectOf(Ref r) const {
  std::lock_guard<std::mutex> lk(mu_);
  return Obj(r);
}

std::u16string FakeJava::StringOf(int obj) const {
  std::lock_guard<std::mutex> lk(mu_);
  return obj > 0 && obj < static_cast<int>(objects_.size()) ? objects_[obj].str : std::u16string{};
}

std::uint64_t FakeJava::FieldOf(int obj, const std::string& name) const {
  std::lock_guard<std::mutex> lk(mu_);
  if (obj <= 0 || obj >= static_cast<int>(objects_.size())) return 0;
  const auto it = objects_[obj].fields.find(name);
  return it == objects_[obj].fields.end() ? 0 : it->second;
}

std::uint64_t FakeJava::StaticOf(const std::string& cls, const std::string& name) const {
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& [id, k] : classes_) {
    if (k.name != cls) continue;
    const auto it = k.statics.find(name);
    return it == k.statics.end() ? 0 : it->second;
  }
  return 0;
}

FakeJava::Ref FakeJava::MakeRef(int obj) {
  std::lock_guard<std::mutex> lk(mu_);
  return MakeRefLocked(obj);
}

void FakeJava::PutField(int obj, const std::string& name, std::uint64_t v) {
  std::lock_guard<std::mutex> lk(mu_);
  if (obj > 0 && obj < static_cast<int>(objects_.size())) objects_[obj].fields[name] = v;
}

void FakeJava::SetPending(const std::string& cls, const std::string& message) {
  std::lock_guard<std::mutex> lk(mu_);
  ThrowLocked(cls, message);
}

std::string FakeJava::PendingClass() const {
  std::lock_guard<std::mutex> lk(mu_);
  if (pending_ == 0) return {};
  const Class* k = ClassOfObj(objects_[pending_].cls);
  return k ? k->name : std::string{};
}

std::size_t FakeJava::live_refs() const {
  std::lock_guard<std::mutex> lk(mu_);
  return refs_.size();
}

std::size_t FakeJava::calls(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = calls_.find(name);
  return it == calls_.end() ? 0 : it->second;
}

std::vector<abi::HostJava::NativeBinding> FakeJava::natives(const std::string& cls) const {
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& [id, k] : classes_) {
    if (k.name == cls) return k.natives;
  }
  return {};
}

int FakeJava::monitor_depth(int obj) const {
  std::lock_guard<std::mutex> lk(mu_);
  return obj > 0 && obj < static_cast<int>(objects_.size()) ? objects_[obj].monitor : 0;
}

void FakeJava::set_attached(bool attached) {
  std::lock_guard<std::mutex> lk(mu_);
  detached_[std::this_thread::get_id()] = !attached;
}

// ------------------------------------------------------------------------------------------------- internals

FakeJava::Ref FakeJava::MakeRefLocked(int obj) {
  if (obj <= 0) return 0;
  const Ref r = next_ref_;
  next_ref_ += 0x10;
  refs_[r] = obj;
  auto& frames = frames_[std::this_thread::get_id()];
  if (!frames.empty()) frames.back().push_back(r);
  return r;
}

int FakeJava::Obj(Ref r) const {
  const auto it = refs_.find(r);
  return it == refs_.end() ? 0 : it->second;
}

const FakeJava::Class* FakeJava::ClassOfObj(int cls_obj) const {
  const auto it = classes_.find(cls_obj);
  return it == classes_.end() ? nullptr : &it->second;
}

int FakeJava::ClassNamed(const std::string& name) {
  for (const auto& [id, k] : classes_) {
    if (k.name == name) return id;
  }
  objects_.push_back(Object{});
  const int self = static_cast<int>(objects_.size()) - 1;
  Class c;
  c.name = name;
  c.self = self;
  classes_[self] = c;
  return self;
}

int FakeJava::NewObjectLocked(int cls) {
  Object o;
  o.cls = cls;
  objects_.push_back(o);
  return static_cast<int>(objects_.size()) - 1;
}

int FakeJava::FindMethodLocked(int cls_obj, const std::string& name, const std::string& sig, bool is_static) const {
  for (int c = cls_obj; c != 0;) {
    for (std::size_t i = 1; i < methods_.size(); ++i) {
      const Method& m = methods_[i];
      if (m.cls == c && m.name == name && m.sig == sig && m.is_static == is_static) return static_cast<int>(i);
    }
    const Class* k = ClassOfObj(c);
    c = k ? k->super : 0;
  }
  return 0;
}

void FakeJava::ThrowLocked(const std::string& cls, const std::string& message) {
  const int t = NewObjectLocked(ClassNamed(cls));
  objects_[t].str = std::u16string(message.begin(), message.end());
  pending_ = t;
}

// ------------------------------------------------------------------------------------------------- HostJava

FakeJava::Ref FakeJava::FindClass(const std::string& jni_name) {
  std::lock_guard<std::mutex> lk(mu_);
  ++calls_["FindClass(" + jni_name + ")"];
  for (const auto& [id, k] : classes_) {
    if (k.name == jni_name) return MakeRefLocked(id);
  }
  ThrowLocked("java/lang/NoClassDefFoundError", jni_name);
  return 0;
}

FakeJava::Ref FakeJava::GetSuperclass(Ref cls) {
  std::lock_guard<std::mutex> lk(mu_);
  const Class* k = ClassOfObj(Obj(cls));
  return k && k->super ? MakeRefLocked(k->super) : 0;
}

bool FakeJava::IsAssignableFrom(Ref from, Ref to) {
  std::lock_guard<std::mutex> lk(mu_);
  const int target = Obj(to);
  for (int c = Obj(from); c != 0;) {
    if (c == target) return true;
    const Class* k = ClassOfObj(c);
    c = k ? k->super : 0;
  }
  return false;
}

FakeJava::Ref FakeJava::GetObjectClass(Ref obj) {
  std::lock_guard<std::mutex> lk(mu_);
  const int o = Obj(obj);
  return o ? MakeRefLocked(objects_[o].cls) : 0;
}

bool FakeJava::IsInstanceOf(Ref obj, Ref cls) {
  std::lock_guard<std::mutex> lk(mu_);
  const int o = Obj(obj);
  if (o == 0) return true;  // JNI: null is an instance of every class
  const int target = Obj(cls);
  for (int c = objects_[o].cls; c != 0;) {
    if (c == target) return true;
    const Class* k = ClassOfObj(c);
    c = k ? k->super : 0;
  }
  return false;
}

bool FakeJava::IsSameObject(Ref a, Ref b) {
  std::lock_guard<std::mutex> lk(mu_);
  return Obj(a) == Obj(b);
}

FakeJava::Ref FakeJava::AllocObject(Ref cls) {
  std::lock_guard<std::mutex> lk(mu_);
  const int c = Obj(cls);
  if (ClassOfObj(c) == nullptr) return 0;
  return MakeRefLocked(NewObjectLocked(c));
}

FakeJava::Ref FakeJava::NewLocalRef(Ref r) {
  std::lock_guard<std::mutex> lk(mu_);
  return MakeRefLocked(Obj(r));
}
void FakeJava::DeleteLocalRef(Ref r) {
  std::lock_guard<std::mutex> lk(mu_);
  refs_.erase(r);
}
FakeJava::Ref FakeJava::NewGlobalRef(Ref r) {
  std::lock_guard<std::mutex> lk(mu_);
  const int o = Obj(r);
  if (o == 0) return 0;
  const Ref g = next_ref_;  // globals belong to no frame
  next_ref_ += 0x10;
  refs_[g] = o;
  return g;
}
void FakeJava::DeleteGlobalRef(Ref r) { DeleteLocalRef(r); }
FakeJava::Ref FakeJava::NewWeakGlobalRef(Ref r) { return NewGlobalRef(r); }
void FakeJava::DeleteWeakGlobalRef(Ref r) { DeleteLocalRef(r); }

FakeJava::Id FakeJava::GetMethodID(Ref cls, const std::string& name, const std::string& sig, bool is_static) {
  std::lock_guard<std::mutex> lk(mu_);
  const int m = FindMethodLocked(Obj(cls), name, sig, is_static);
  if (m == 0) ThrowLocked("java/lang/NoSuchMethodError", name + sig);
  return static_cast<Id>(m);
}

FakeJava::Id FakeJava::GetFieldID(Ref cls, const std::string& name, const std::string& sig, bool is_static) {
  std::lock_guard<std::mutex> lk(mu_);
  for (int c = Obj(cls); c != 0;) {
    for (std::size_t i = 1; i < fields_.size(); ++i) {
      const Field& f = fields_[i];
      if (f.cls == c && f.name == name && f.sig == sig && f.is_static == is_static) return kFieldIdBase + i;
    }
    const Class* k = ClassOfObj(c);
    c = k ? k->super : 0;
  }
  ThrowLocked("java/lang/NoSuchFieldError", name);
  return 0;
}

std::uint64_t FakeJava::Call(char /*ret_kind*/, bool is_static, Ref target, Ref nonvirtual_cls, Id method,
                             const std::vector<std::uint64_t>& args) {
  Body body;
  int self = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (method == 0 || method >= methods_.size()) return 0;
    const Method* m = &methods_[method];
    if (!is_static) {
      self = Obj(target);
      // Virtual dispatch from the receiver's class, unless nonvirtual: then the named class's own lookup.
      const int from = nonvirtual_cls != 0 ? Obj(nonvirtual_cls) : objects_[self].cls;
      const int found = FindMethodLocked(from, m->name, m->sig, false);
      if (found != 0) m = &methods_[found];
    }
    ++calls_[m->name];
    body = m->body;
  }
  return body ? body(*this, self, args) : 0;  // outside the lock: a body may call back into the fake
}

FakeJava::Ref FakeJava::NewObject(Ref cls, Id ctor, const std::vector<std::uint64_t>& args) {
  const Ref obj = AllocObject(cls);
  if (obj == 0) return 0;
  Body body;
  int self = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (ctor == 0 || ctor >= methods_.size()) return obj;
    body = methods_[ctor].body;
    self = Obj(obj);
  }
  if (body) body(*this, self, args);
  return obj;
}

std::uint64_t FakeJava::GetField(char kind, bool is_static, Ref target, Id field) {
  std::lock_guard<std::mutex> lk(mu_);
  if (field < kFieldIdBase || field - kFieldIdBase >= fields_.size()) return 0;
  const Field& f = fields_[field - kFieldIdBase];
  std::uint64_t v = 0;
  if (is_static) {
    const auto it = classes_[f.cls].statics.find(f.name);
    v = it == classes_[f.cls].statics.end() ? 0 : it->second;
  } else {
    const int o = Obj(target);
    const auto it = objects_[o].fields.find(f.name);
    v = it == objects_[o].fields.end() ? 0 : it->second;
  }
  return kind == 'L' ? MakeRefLocked(static_cast<int>(v)) : v;  // a reference read is a new local
}

void FakeJava::SetField(char kind, bool is_static, Ref target, Id field, std::uint64_t value) {
  std::lock_guard<std::mutex> lk(mu_);
  if (field < kFieldIdBase || field - kFieldIdBase >= fields_.size()) return;
  const Field& f = fields_[field - kFieldIdBase];
  // A stored reference is kept as the object it names (a fresh local is made on every read), as Java does.
  const std::uint64_t stored = kind == 'L' ? static_cast<std::uint64_t>(Obj(value)) : value;
  if (is_static) {
    classes_[f.cls].statics[f.name] = stored;
  } else {
    objects_[Obj(target)].fields[f.name] = stored;
  }
}

FakeJava::Ref FakeJava::NewString(const std::u16string& chars) {
  std::lock_guard<std::mutex> lk(mu_);
  const int o = NewObjectLocked(ClassNamed("java/lang/String"));
  objects_[o].str = chars;
  return MakeRefLocked(o);
}

std::u16string FakeJava::GetStringChars(Ref s) {
  std::lock_guard<std::mutex> lk(mu_);
  return objects_[Obj(s)].str;
}

FakeJava::Ref FakeJava::NewArray(char kind, std::int32_t length, Ref elem_cls, Ref init) {
  std::lock_guard<std::mutex> lk(mu_);
  if (length < 0) {
    ThrowLocked("java/lang/NegativeArraySizeException", std::to_string(length));
    return 0;
  }
  std::string name = std::string{"["} + kind;
  if (kind == 'L') {
    const Class* k = ClassOfObj(Obj(elem_cls));
    name = "[L" + (k ? k->name : std::string{"?"}) + ";";
  }
  const int o = NewObjectLocked(ClassNamed(name));
  objects_[o].array_kind = kind;
  if (kind == 'L') {
    objects_[o].elems.assign(static_cast<std::size_t>(length), Obj(init));
  } else {
    objects_[o].bytes.assign(static_cast<std::size_t>(length) * ElemSize(kind), 0);
  }
  return MakeRefLocked(o);
}

std::int32_t FakeJava::GetArrayLength(Ref a) {
  std::lock_guard<std::mutex> lk(mu_);
  const Object& o = objects_[Obj(a)];
  if (o.array_kind == 'L') return static_cast<std::int32_t>(o.elems.size());
  return o.array_kind ? static_cast<std::int32_t>(o.bytes.size() / ElemSize(o.array_kind)) : 0;
}

char FakeJava::ArrayKind(Ref a) {
  std::lock_guard<std::mutex> lk(mu_);
  return objects_[Obj(a)].array_kind;
}

bool FakeJava::GetArrayRegion(char kind, Ref a, std::int32_t start, std::int32_t len, void* out) {
  std::lock_guard<std::mutex> lk(mu_);
  Object& o = objects_[Obj(a)];
  const std::size_t es = ElemSize(kind);
  const std::size_t n = o.bytes.size() / es;
  if (o.array_kind != kind || start < 0 || len < 0 || static_cast<std::size_t>(start) + static_cast<std::size_t>(len) > n) {
    ThrowLocked("java/lang/ArrayIndexOutOfBoundsException", std::to_string(start) + "+" + std::to_string(len));
    return false;
  }
  std::memcpy(out, o.bytes.data() + start * es, len * es);
  return true;
}

bool FakeJava::SetArrayRegion(char kind, Ref a, std::int32_t start, std::int32_t len, const void* in) {
  std::lock_guard<std::mutex> lk(mu_);
  Object& o = objects_[Obj(a)];
  const std::size_t es = ElemSize(kind);
  const std::size_t n = o.bytes.size() / es;
  if (o.array_kind != kind || start < 0 || len < 0 || static_cast<std::size_t>(start) + static_cast<std::size_t>(len) > n) {
    ThrowLocked("java/lang/ArrayIndexOutOfBoundsException", std::to_string(start) + "+" + std::to_string(len));
    return false;
  }
  std::memcpy(o.bytes.data() + start * es, in, len * es);
  return true;
}

FakeJava::Ref FakeJava::GetObjectArrayElement(Ref a, std::int32_t index) {
  std::lock_guard<std::mutex> lk(mu_);
  const Object& o = objects_[Obj(a)];
  if (index < 0 || static_cast<std::size_t>(index) >= o.elems.size()) {
    ThrowLocked("java/lang/ArrayIndexOutOfBoundsException", std::to_string(index));
    return 0;
  }
  return MakeRefLocked(o.elems[index]);
}

void FakeJava::SetObjectArrayElement(Ref a, std::int32_t index, Ref value) {
  std::lock_guard<std::mutex> lk(mu_);
  Object& o = objects_[Obj(a)];
  if (index < 0 || static_cast<std::size_t>(index) >= o.elems.size()) {
    ThrowLocked("java/lang/ArrayIndexOutOfBoundsException", std::to_string(index));
    return;
  }
  o.elems[index] = Obj(value);
}

bool FakeJava::ExceptionCheck() {
  std::lock_guard<std::mutex> lk(mu_);
  return pending_ != 0;
}

std::int32_t FakeJava::Throw(Ref throwable) {
  std::lock_guard<std::mutex> lk(mu_);
  pending_ = Obj(throwable);
  return pending_ != 0 ? 0 : -1;
}

std::int32_t FakeJava::ThrowNew(Ref cls, const std::string& message) {
  std::lock_guard<std::mutex> lk(mu_);
  const int c = Obj(cls);
  if (ClassOfObj(c) == nullptr) return -1;
  const int t = NewObjectLocked(c);
  objects_[t].str = std::u16string(message.begin(), message.end());
  pending_ = t;
  return 0;
}

FakeJava::Ref FakeJava::ExceptionOccurred() {
  std::lock_guard<std::mutex> lk(mu_);
  return pending_ != 0 ? MakeRefLocked(pending_) : 0;
}

void FakeJava::ExceptionDescribe() {
  const std::string cls = PendingClass();
  std::fprintf(stderr, "#   FakeJava: pending %s\n", cls.c_str());
}

void FakeJava::ExceptionClear() {
  std::lock_guard<std::mutex> lk(mu_);
  pending_ = 0;
}

std::int32_t FakeJava::PushLocalFrame(std::int32_t /*capacity*/) {
  std::lock_guard<std::mutex> lk(mu_);
  frames_[std::this_thread::get_id()].emplace_back();
  return 0;
}

FakeJava::Ref FakeJava::PopLocalFrame(Ref result) {
  std::lock_guard<std::mutex> lk(mu_);
  auto& frames = frames_[std::this_thread::get_id()];
  const int kept = Obj(result);
  if (!frames.empty()) {
    for (const Ref r : frames.back()) refs_.erase(r);
    frames.pop_back();
  }
  return MakeRefLocked(kept);
}

std::int32_t FakeJava::MonitorEnter(Ref obj) {
  std::lock_guard<std::mutex> lk(mu_);
  ++objects_[Obj(obj)].monitor;
  return 0;
}

std::int32_t FakeJava::MonitorExit(Ref obj) {
  std::lock_guard<std::mutex> lk(mu_);
  Object& o = objects_[Obj(obj)];
  if (o.monitor <= 0) {
    ThrowLocked("java/lang/IllegalMonitorStateException", "");
    return -1;
  }
  --o.monitor;
  return 0;
}

std::int32_t FakeJava::RegisterNatives(Ref cls, const std::vector<NativeBinding>& methods) {
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = classes_.find(Obj(cls));
  if (it == classes_.end()) return -1;
  for (const NativeBinding& b : methods) it->second.natives.push_back(b);
  return 0;
}

std::int32_t FakeJava::UnregisterNatives(Ref cls) {
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = classes_.find(Obj(cls));
  if (it == classes_.end()) return -1;
  it->second.natives.clear();
  return 0;
}

FakeJava::Ref FakeJava::ToReflected(Ref /*cls*/, Id id, bool is_field, bool /*is_static*/) {
  std::lock_guard<std::mutex> lk(mu_);
  const int o = NewObjectLocked(ClassNamed(is_field ? "java/lang/reflect/Field" : "java/lang/reflect/Method"));
  objects_[o].member = id;
  objects_[o].member_is_field = is_field;
  return MakeRefLocked(o);
}

FakeJava::Id FakeJava::FromReflected(Ref member, bool is_field, std::string* name, std::string* sig, bool* is_static) {
  std::lock_guard<std::mutex> lk(mu_);
  const Object& o = objects_[Obj(member)];
  if (o.member == 0 || o.member_is_field != is_field) return 0;
  if (is_field) {
    const Field& f = fields_[o.member - kFieldIdBase];
    *name = f.name;
    *sig = f.sig;
    *is_static = f.is_static;
  } else {
    const Method& m = methods_[o.member];
    *name = m.name;
    *sig = m.sig;
    *is_static = m.is_static;
  }
  return o.member;
}

FakeJava::Ref FakeJava::NewDirectByteBuffer(void* address, std::int64_t capacity) {
  std::lock_guard<std::mutex> lk(mu_);
  const int o = NewObjectLocked(ClassNamed("java/nio/DirectByteBuffer"));
  objects_[o].direct = address;
  objects_[o].capacity = capacity;
  return MakeRefLocked(o);
}

void* FakeJava::GetDirectBufferAddress(Ref buf) {
  std::lock_guard<std::mutex> lk(mu_);
  return objects_[Obj(buf)].direct;
}

std::int64_t FakeJava::GetDirectBufferCapacity(Ref buf) {
  std::lock_guard<std::mutex> lk(mu_);
  return objects_[Obj(buf)].capacity;
}

bool FakeJava::IsAttached() {
  std::lock_guard<std::mutex> lk(mu_);
  return !detached_[std::this_thread::get_id()];
}

bool FakeJava::AttachCurrentThread(bool /*daemon*/) {
  std::lock_guard<std::mutex> lk(mu_);
  detached_[std::this_thread::get_id()] = false;
  return true;
}

void FakeJava::DetachCurrentThread() {
  std::lock_guard<std::mutex> lk(mu_);
  detached_[std::this_thread::get_id()] = true;
}

}  // namespace rb::test
