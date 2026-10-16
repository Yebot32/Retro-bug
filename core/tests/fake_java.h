// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// FakeJava: an in-memory Java for the forwarding JNI bridge's core lanes — classes with a superclass chain, objects
// with fields, static and virtual methods implemented by host lambdas, real reference semantics (a reference is a
// counted handle to an object: two references to one object are IsSameObject; a deleted one is gone) and a
// pending exception. Thread-safe (one mutex), because the bridge calls it from several host threads at once.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "rb/abi/host_java.h"

namespace rb::test {

class FakeJava : public abi::HostJava {
 public:
  using Ref = abi::HostRef;
  using Id = abi::HostId;
  /// A method body: the receiver's object id (0 for static), the arguments in the bridge's bit encoding (a
  /// reference as a HostRef the fake made for this call), the result in the same encoding.
  using Body = std::function<std::uint64_t(FakeJava&, int self, const std::vector<std::uint64_t>& args)>;

  /// Defines a class (`super_name` empty for a root) and returns its class object's id.
  int DefineClass(const std::string& name, const std::string& super_name = {});
  void DefineMethod(const std::string& cls, const std::string& name, const std::string& sig, bool is_static, Body body);
  void DefineField(const std::string& cls, const std::string& name, const std::string& sig, bool is_static);
  /// A new instance of `cls` (fields zeroed) and a new local reference to it.
  Ref NewInstance(const std::string& cls);
  int ObjectOf(Ref r) const;  ///< the object a live reference names, 0 if none
  std::uint64_t FieldOf(int obj, const std::string& name) const;
  std::uint64_t StaticOf(const std::string& cls, const std::string& name) const;
  Ref MakeRef(int obj);  ///< a new local reference (for method bodies returning objects)
  void PutField(int obj, const std::string& name, std::uint64_t v);  ///< for method bodies (constructors)
  void SetPending(const std::string& cls, const std::string& message);
  std::size_t live_refs() const;
  std::size_t calls(const std::string& name) const;  ///< how often a method or FindClass(name) was reached

  // HostJava
  Ref FindClass(const std::string& jni_name) override;
  Ref GetSuperclass(Ref cls) override;
  bool IsAssignableFrom(Ref from, Ref to) override;
  Ref GetObjectClass(Ref obj) override;
  bool IsInstanceOf(Ref obj, Ref cls) override;
  bool IsSameObject(Ref a, Ref b) override;
  Ref AllocObject(Ref cls) override;
  Ref NewLocalRef(Ref r) override;
  void DeleteLocalRef(Ref r) override;
  Ref NewGlobalRef(Ref r) override;
  void DeleteGlobalRef(Ref r) override;
  Ref NewWeakGlobalRef(Ref r) override;
  void DeleteWeakGlobalRef(Ref r) override;
  Id GetMethodID(Ref cls, const std::string& name, const std::string& sig, bool is_static) override;
  Id GetFieldID(Ref cls, const std::string& name, const std::string& sig, bool is_static) override;
  std::uint64_t Call(char ret_kind, bool is_static, Ref target, Ref nonvirtual_cls, Id method,
                     const std::vector<std::uint64_t>& args) override;
  Ref NewObject(Ref cls, Id ctor, const std::vector<std::uint64_t>& args) override;
  std::uint64_t GetField(char kind, bool is_static, Ref target, Id field) override;
  void SetField(char kind, bool is_static, Ref target, Id field, std::uint64_t value) override;
  bool ExceptionCheck() override;

 private:
  struct Object {
    int cls = 0;  ///< class object id; a class object's own `cls` is 0
    std::map<std::string, std::uint64_t> fields;
  };
  struct Class {
    std::string name;
    int super = 0;  ///< class object id
    int self = 0;
    std::map<std::string, std::uint64_t> statics;
  };
  struct Method {
    int cls = 0;
    std::string name, sig;
    bool is_static = false;
    Body body;
  };
  struct Field {
    int cls = 0;
    std::string name, sig;
    bool is_static = false;
  };

  Ref MakeRefLocked(int obj);
  int Obj(Ref r) const;  ///< mu_ held
  const Class* ClassOfObj(int cls_obj) const;
  int FindMethodLocked(int cls_obj, const std::string& name, const std::string& sig, bool is_static) const;

  mutable std::mutex mu_;
  std::vector<Object> objects_{Object{}};  ///< id 0 is "no object"
  std::map<int, Class> classes_;           ///< by class object id
  std::vector<Method> methods_{Method{}};  ///< HostId = index (0 unused)
  std::vector<Field> fields_{Field{}};     ///< HostId = 0x10000 + index
  std::map<Ref, int> refs_;
  Ref next_ref_ = 0x1000;
  std::map<std::string, std::size_t> calls_;
  bool pending_ = false;
  std::string pending_cls_, pending_msg_;
};

}  // namespace rb::test
