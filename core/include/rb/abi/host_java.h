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

  // Exceptions (the rest arrive with Task 2).
  virtual bool ExceptionCheck() = 0;
};

}  // namespace rb::abi
