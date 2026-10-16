// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// ArtJava: the forwarding JNI bridge's HostJava on a device — every operation on the calling thread's own real
// JNIEnv (JavaVM::GetEnv). A host thread ART never attached (a guest pthread) is attached on its first JNI use and
// detached when it exits (a pthread key destructor), because ART aborts on a thread that exits while attached.
#pragma once

#include <jni.h>

#include <mutex>
#include <string>
#include <vector>

#include "rb/abi/host_java.h"

namespace rb::launcher {

class ArtJava : public abi::HostJava {
 public:
  explicit ArtJava(JavaVM* vm);
  ArtJava(const ArtJava&) = delete;
  ArtJava& operator=(const ArtJava&) = delete;

  using Ref = abi::HostRef;
  using Id = abi::HostId;

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
  /// The calling thread's JNIEnv, attaching it first if ART does not know it (logged once per thread).
  JNIEnv* Env();
  /// A class's JNI descriptor ("I", "[B", "Ljava/lang/String;") through java.lang.Class.getName().
  std::string Descriptor(JNIEnv* env, jclass c);

  JavaVM* vm_;
  std::once_flag arrays_once_;
  jclass array_classes_[9] = {};  ///< [Z [B [C [S [I [J [F [D [Ljava/lang/Object; — global refs
};

}  // namespace rb::launcher
