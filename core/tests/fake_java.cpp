// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "fake_java.h"

namespace rb::test {

namespace {
constexpr abi::HostId kFieldIdBase = 0x10000;
}

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
    if (k.name == cls) {
      objects_.push_back(Object{id, {}});
      return MakeRefLocked(static_cast<int>(objects_.size()) - 1);
    }
  }
  return 0;
}

int FakeJava::ObjectOf(Ref r) const {
  std::lock_guard<std::mutex> lk(mu_);
  return Obj(r);
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

FakeJava::Ref FakeJava::MakeRefLocked(int obj) {
  if (obj <= 0) return 0;
  const Ref r = next_ref_;
  next_ref_ += 0x10;
  refs_[r] = obj;
  return r;
}

void FakeJava::SetPending(const std::string& cls, const std::string& message) {
  std::lock_guard<std::mutex> lk(mu_);
  pending_ = true;
  pending_cls_ = cls;
  pending_msg_ = message;
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

int FakeJava::Obj(Ref r) const {
  const auto it = refs_.find(r);
  return it == refs_.end() ? 0 : it->second;
}

const FakeJava::Class* FakeJava::ClassOfObj(int cls_obj) const {
  const auto it = classes_.find(cls_obj);
  return it == classes_.end() ? nullptr : &it->second;
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

FakeJava::Ref FakeJava::FindClass(const std::string& jni_name) {
  std::lock_guard<std::mutex> lk(mu_);
  ++calls_["FindClass(" + jni_name + ")"];
  for (const auto& [id, k] : classes_) {
    if (k.name == jni_name) return MakeRefLocked(id);
  }
  pending_ = true;
  pending_cls_ = "java/lang/NoClassDefFoundError";
  pending_msg_ = jni_name;
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
  std::unique_lock<std::mutex> lk(mu_);
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
  objects_.push_back(Object{c, {}});
  return MakeRefLocked(static_cast<int>(objects_.size()) - 1);
}

FakeJava::Ref FakeJava::NewLocalRef(Ref r) {
  std::lock_guard<std::mutex> lk(mu_);
  return MakeRefLocked(Obj(r));
}
void FakeJava::DeleteLocalRef(Ref r) {
  std::lock_guard<std::mutex> lk(mu_);
  refs_.erase(r);
}
FakeJava::Ref FakeJava::NewGlobalRef(Ref r) { return NewLocalRef(r); }
void FakeJava::DeleteGlobalRef(Ref r) { DeleteLocalRef(r); }
FakeJava::Ref FakeJava::NewWeakGlobalRef(Ref r) { return NewLocalRef(r); }
void FakeJava::DeleteWeakGlobalRef(Ref r) { DeleteLocalRef(r); }

FakeJava::Id FakeJava::GetMethodID(Ref cls, const std::string& name, const std::string& sig, bool is_static) {
  std::lock_guard<std::mutex> lk(mu_);
  const int m = FindMethodLocked(Obj(cls), name, sig, is_static);
  if (m == 0) {
    pending_ = true;
    pending_cls_ = "java/lang/NoSuchMethodError";
    pending_msg_ = name + sig;
  }
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
  pending_ = true;
  pending_cls_ = "java/lang/NoSuchFieldError";
  pending_msg_ = name;
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

bool FakeJava::ExceptionCheck() {
  std::lock_guard<std::mutex> lk(mu_);
  return pending_;
}

}  // namespace rb::test

namespace rb::test {
void FakeJava::PutField(int obj, const std::string& name, std::uint64_t v) {
  std::lock_guard<std::mutex> lk(mu_);
  if (obj > 0 && obj < static_cast<int>(objects_.size())) objects_[obj].fields[name] = v;
}
}  // namespace rb::test
