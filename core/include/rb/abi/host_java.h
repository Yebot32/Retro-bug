// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// HostJava: the real Java the forwarding JNI bridge (`ForwardingJni`) talks to, one virtual per operation it
// forwards, in host types. On a device it is `ArtJava` (android/jni/art_java.cpp) over the calling thread's real
// `JNIEnv`; in the core lanes it is a fake (core/tests/fake_java.h), so the guest half is testable without ART.
//
// Every call is made WITHOUT the Global Guest Lock (the bridge's slots release it): real Java may block, call back
// into a native (which enters the guest again), or run on another thread meanwhile. Implementations are called on
// the host thread whose guest thread made the JNI call, and act on that thread's own JNIEnv.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rb::abi {

/// A real JNI reference (`jobject` bits); 0 is null. Local references belong to the calling thread's current
/// real local frame, exactly as in JNI.
using HostRef = std::uintptr_t;
/// A real `jmethodID` / `jfieldID` (bits); 0 means not found (with a pending exception, as in JNI).
using HostId = std::uintptr_t;

class HostJava {
 public:
  virtual ~HostJava() = default;

  // Classes and objects.
  virtual HostRef FindClass(const std::string& jni_name) = 0;
  virtual HostRef GetSuperclass(HostRef cls) = 0;
  virtual bool IsAssignableFrom(HostRef from, HostRef to) = 0;
  virtual HostRef GetObjectClass(HostRef obj) = 0;
  virtual bool IsInstanceOf(HostRef obj, HostRef cls) = 0;
  virtual bool IsSameObject(HostRef a, HostRef b) = 0;
  virtual HostRef AllocObject(HostRef cls) = 0;

  // References.
  virtual HostRef NewLocalRef(HostRef r) = 0;
  virtual void DeleteLocalRef(HostRef r) = 0;
  virtual HostRef NewGlobalRef(HostRef r) = 0;
  virtual void DeleteGlobalRef(HostRef r) = 0;
  virtual HostRef NewWeakGlobalRef(HostRef r) = 0;
  virtual void DeleteWeakGlobalRef(HostRef r) = 0;

  // Members. `is_static` selects Get{Static}{Method,Field}ID.
  virtual HostId GetMethodID(HostRef cls, const std::string& name, const std::string& sig, bool is_static) = 0;
  virtual HostId GetFieldID(HostRef cls, const std::string& name, const std::string& sig, bool is_static) = 0;
  /// A method call. `ret_kind` is the method's return kind (V Z B C S I J F D, L for a reference). `target` is
  /// the receiver, or the class for a static call; `nonvirtual_cls` is the class of a CallNonvirtual call (0
  /// otherwise). Each argument is in its own bits (`jni::DecodeArgs`' encoding, references as HostRef). A
  /// reference result is returned as a new local reference's bits.
  virtual std::uint64_t Call(char ret_kind, bool is_static, HostRef target, HostRef nonvirtual_cls, HostId method,
                             const std::vector<std::uint64_t>& args) = 0;
  virtual HostRef NewObject(HostRef cls, HostId ctor, const std::vector<std::uint64_t>& args) = 0;
  /// Field access in the same bit encoding; `target` is the object, or the class for a static field.
  virtual std::uint64_t GetField(char kind, bool is_static, HostRef target, HostId field) = 0;
  virtual void SetField(char kind, bool is_static, HostRef target, HostId field, std::uint64_t value) = 0;

  // Strings, as UTF-16 code units (the bridge does every MUTF-8 conversion itself, so a guest sees the same bytes
  // whichever HostJava answers).
  virtual HostRef NewString(const std::u16string& chars) = 0;
  virtual std::u16string GetStringChars(HostRef s) = 0;

  // Arrays. `kind` is the element kind: Z B C S I J F D, or L for an object array.
  virtual HostRef NewArray(char kind, std::int32_t length, HostRef elem_cls, HostRef init) = 0;
  virtual std::int32_t GetArrayLength(HostRef a) = 0;
  /// The element kind of a primitive array, 'L' for an object array, 0 when `a` is not an array.
  virtual char ArrayKind(HostRef a) = 0;
  /// Element copies in the elements' own little-endian bytes. False, with an ArrayIndexOutOfBoundsException
  /// pending, when [start, start + len) is not inside the array.
  virtual bool GetArrayRegion(char kind, HostRef a, std::int32_t start, std::int32_t len, void* out) = 0;
  virtual bool SetArrayRegion(char kind, HostRef a, std::int32_t start, std::int32_t len, const void* in) = 0;
  virtual HostRef GetObjectArrayElement(HostRef a, std::int32_t index) = 0;
  virtual void SetObjectArrayElement(HostRef a, std::int32_t index, HostRef value) = 0;

  // Exceptions.
  virtual bool ExceptionCheck() = 0;
  virtual std::int32_t Throw(HostRef throwable) = 0;
  virtual std::int32_t ThrowNew(HostRef cls, const std::string& message) = 0;
  virtual HostRef ExceptionOccurred() = 0;
  virtual void ExceptionDescribe() = 0;
  virtual void ExceptionClear() = 0;

  // Local frames and monitors.
  virtual std::int32_t PushLocalFrame(std::int32_t capacity) = 0;
  virtual HostRef PopLocalFrame(HostRef result) = 0;
  virtual std::int32_t MonitorEnter(HostRef obj) = 0;
  virtual std::int32_t MonitorExit(HostRef obj) = 0;

  // Natives: a guest RegisterNatives, each method bound to a host entry point the bridge made for it.
  struct NativeBinding {
    std::string name, sig;
    void* host_fn = nullptr;
  };
  virtual std::int32_t RegisterNatives(HostRef cls, const std::vector<NativeBinding>& methods) = 0;
  virtual std::int32_t UnregisterNatives(HostRef cls) = 0;

  // Reflection.
  virtual HostRef ToReflected(HostRef cls, HostId id, bool is_field, bool is_static) = 0;
  /// The ID of a java.lang.reflect.Method/Constructor/Field, with its name, JNI signature and staticness (the
  /// bridge records them so later calls through the ID can be decoded).
  virtual HostId FromReflected(HostRef member, bool is_field, std::string* name, std::string* sig, bool* is_static) = 0;

  // Direct buffers over host memory.
  virtual HostRef NewDirectByteBuffer(void* address, std::int64_t capacity) = 0;
  virtual void* GetDirectBufferAddress(HostRef buf) = 0;
  virtual std::int64_t GetDirectBufferCapacity(HostRef buf) = 0;

  // The invocation interface, for the calling host thread.
  virtual bool IsAttached() = 0;
  virtual bool AttachCurrentThread(bool daemon) = 0;
  virtual void DetachCurrentThread() = 0;
};

}  // namespace rb::abi
