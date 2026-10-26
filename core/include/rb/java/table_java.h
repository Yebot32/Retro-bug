// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// TableJava: the table-driven Java a guest can be given when there is no VM — the core lanes' fake and the
// iOS app's Java replacement. It is an in-memory model of classes with a superclass chain, objects with
// fields, strings (UTF-16), arrays, static and virtual methods implemented by host lambdas, real reference
// semantics (a reference is a counted handle to an object: two references to one object are IsSameObject; a deleted
// one is gone; a local frame pops the references made in it), a pending exception that is a real throwable object,
// monitors, registered natives, reflection objects and direct buffers. Thread-safe (one mutex), because the bridge
// calls it from several host threads at once.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rb/abi/host_java.h"

namespace rb::java {

class TableJava : public abi::HostJava {
 public:
  using Ref = abi::HostRef;
  using Id = abi::HostId;
  /// A method body: the receiver's object id (0 for static), the arguments in the bridge's bit encoding (a
  /// reference as a HostRef the fake made for this call), the result in the same encoding.
  using Body = std::function<std::uint64_t(TableJava&, int self, const std::vector<std::uint64_t>& args)>;

  /// A line for every failed class/method/field lookup. The iOS boot log uses it to name the Java
  /// surface a title expects but the profile does not yet provide.
  void set_trace(std::function<void(const std::string&)> trace);

  /// Defines a class (`super_name` empty for a root) and returns its class object's id.
  int DefineClass(const std::string& name, const std::string& super_name = {});
  /// Defines (or, for an existing one, replaces the body of) a method.
  void DefineMethod(const std::string& cls, const std::string& name, const std::string& sig, bool is_static, Body body);
  void DefineField(const std::string& cls, const std::string& name, const std::string& sig, bool is_static);
  /// A new instance of `cls` (fields zeroed) and a new local reference to it.
  Ref NewInstance(const std::string& cls);
  Ref NewJavaString(const std::u16string& s);
  int ObjectOf(Ref r) const;  ///< the object a live reference names, 0 if none
  std::u16string StringOf(int obj) const;
  std::uint64_t FieldOf(int obj, const std::string& name) const;
  std::uint64_t StaticOf(const std::string& cls, const std::string& name) const;
  Ref MakeRef(int obj);  ///< a new local reference (for method bodies returning objects)
  void PutField(int obj, const std::string& name, std::uint64_t v);  ///< for method bodies (constructors)
  /// Throws a new instance of `cls` (defined on demand) with `message`.
  void SetPending(const std::string& cls, const std::string& message);
  std::string PendingClass() const;  ///< the pending throwable's class name, empty if none
  std::size_t live_refs() const;
  std::size_t calls(const std::string& name) const;  ///< how often a method or FindClass(name) was reached
  std::vector<NativeBinding> natives(const std::string& cls) const;
  int monitor_depth(int obj) const;
  /// A direct buffer over host memory that is not guest memory (for the bridge's refusal).
  Ref NewHostDirectBuffer(void* address, std::int64_t capacity) { return NewDirectByteBuffer(address, capacity); }
  void set_attached(bool attached);  ///< for the calling thread (default: attached)

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
  Ref NewString(const std::u16string& chars) override;
  std::u16string GetStringChars(Ref s) override;
  Ref NewArray(char kind, std::int32_t length, Ref elem_cls, Ref init) override;
  std::int32_t GetArrayLength(Ref a) override;
  char ArrayKind(Ref a) override;
  bool GetArrayRegion(char kind, Ref a, std::int32_t start, std::int32_t len, void* out) override;
  bool SetArrayRegion(char kind, Ref a, std::int32_t start, std::int32_t len, const void* in) override;
  Ref GetObjectArrayElement(Ref a, std::int32_t index) override;
  void SetObjectArrayElement(Ref a, std::int32_t index, Ref value) override;
  bool ExceptionCheck() override;
  std::int32_t Throw(Ref throwable) override;
  std::int32_t ThrowNew(Ref cls, const std::string& message) override;
  Ref ExceptionOccurred() override;
  void ExceptionDescribe() override;
  void ExceptionClear() override;
  std::int32_t PushLocalFrame(std::int32_t capacity) override;
  Ref PopLocalFrame(Ref result) override;
  std::int32_t MonitorEnter(Ref obj) override;
  std::int32_t MonitorExit(Ref obj) override;
  std::int32_t RegisterNatives(Ref cls, const std::vector<NativeBinding>& methods) override;
  std::int32_t UnregisterNatives(Ref cls) override;
  Ref ToReflected(Ref cls, Id id, bool is_field, bool is_static) override;
  Id FromReflected(Ref member, bool is_field, std::string* name, std::string* sig, bool* is_static) override;
  Ref NewDirectByteBuffer(void* address, std::int64_t capacity) override;
  void* GetDirectBufferAddress(Ref buf) override;
  std::int64_t GetDirectBufferCapacity(Ref buf) override;
  bool IsAttached() override;
  bool AttachCurrentThread(bool daemon) override;
  void DetachCurrentThread() override;

 private:
  struct Object {
    int cls = 0;  ///< class object id; a class object's own `cls` is 0
    std::map<std::string, std::uint64_t> fields;
    std::u16string str;  ///< java/lang/String
    char array_kind = 0;  ///< 0: not an array
    std::vector<std::uint8_t> bytes;  ///< primitive array elements
    std::vector<int> elems;           ///< object array elements (object ids)
    void* direct = nullptr;
    std::int64_t capacity = -1;
    Id member = 0;  ///< reflection object
    bool member_is_field = false;
    int monitor = 0;
  };
  struct Class {
    std::string name;
    int super = 0;  ///< class object id
    int self = 0;
    std::map<std::string, std::uint64_t> statics;
    std::vector<NativeBinding> natives;
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
  int ClassNamed(const std::string& name);  ///< defines it on demand; mu_ held
  int NewObjectLocked(int cls);
  int FindMethodLocked(int cls_obj, const std::string& name, const std::string& sig, bool is_static) const;
  void ThrowLocked(const std::string& cls, const std::string& message);

  mutable std::mutex mu_;
  std::vector<Object> objects_{Object{}};  ///< id 0 is "no object"
  std::map<int, Class> classes_;           ///< by class object id
  std::vector<Method> methods_{Method{}};  ///< HostId = index (0 unused)
  std::vector<Field> fields_{Field{}};     ///< HostId = 0x10000 + index
  std::map<Ref, int> refs_;
  Ref next_ref_ = 0x1000;
  std::map<std::string, std::size_t> calls_;
  std::function<void(const std::string&)> trace_;
  int pending_ = 0;  ///< the pending throwable's object id
  std::map<std::thread::id, std::vector<std::vector<Ref>>> frames_;  ///< per thread: refs made in each frame
  std::map<std::thread::id, bool> detached_;
};

}  // namespace rb::java
