// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "art_java.h"

#include <android/log.h>
#include <pthread.h>
#include <unistd.h>

#include <cstring>

namespace rb::launcher {
namespace {

constexpr const char* kTag = "RBRUNTIME";

pthread_key_t g_attached_key;
std::once_flag g_key_once;

void DetachAtExit(void* vm) {
  // A thread this bridge attached is exiting: ART aborts if it is still attached.
  static_cast<JavaVM*>(vm)->DetachCurrentThread();
}

void MarkAttached(JavaVM* vm) {
  std::call_once(g_key_once, [] { pthread_key_create(&g_attached_key, DetachAtExit); });
  pthread_setspecific(g_attached_key, vm);
}

jobject J(abi::HostRef r) { return reinterpret_cast<jobject>(r); }
jclass C(abi::HostRef r) { return reinterpret_cast<jclass>(r); }
abi::HostRef R(jobject o) { return reinterpret_cast<abi::HostRef>(o); }

std::vector<jvalue> Jvalues(const std::vector<std::uint64_t>& args) {
  // Each argument is in its own little-endian bits, so the union's j member holds every kind's value.
  std::vector<jvalue> v(args.size());
  for (std::size_t i = 0; i < args.size(); ++i) v[i].j = static_cast<jlong>(args[i]);
  return v;
}

template <typename T>
std::uint64_t Bits(T v) {
  std::uint64_t b = 0;
  std::memcpy(&b, &v, sizeof v);
  return b;
}

template <typename T>
T From(std::uint64_t b) {
  T v{};
  std::memcpy(&v, &b, sizeof v);
  return v;
}

}  // namespace

ArtJava::ArtJava(JavaVM* vm) : vm_{vm} {}

JNIEnv* ArtJava::Env() {
  JNIEnv* env = nullptr;
  if (vm_ == nullptr) return nullptr;
  if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
  // A guest pthread making JNI calls without AttachCurrentThread: it would crash on a device; here it is
  // attached (and detached at its exit), and said once per thread.
  if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
  MarkAttached(vm_);
  __android_log_print(ANDROID_LOG_INFO, kTag, "jni: attached host thread %d to the VM (it called JNI unattached)",
                      static_cast<int>(gettid()));
  return env;
}

#define ENV_OR(value)              \
  JNIEnv* env = Env();             \
  if (env == nullptr) return value

ArtJava::Ref ArtJava::FindClass(const std::string& jni_name) {
  ENV_OR(0);
  return R(env->FindClass(jni_name.c_str()));
}
ArtJava::Ref ArtJava::GetSuperclass(Ref cls) {
  ENV_OR(0);
  return R(env->GetSuperclass(C(cls)));
}
bool ArtJava::IsAssignableFrom(Ref from, Ref to) {
  ENV_OR(false);
  return env->IsAssignableFrom(C(from), C(to)) != JNI_FALSE;
}
ArtJava::Ref ArtJava::GetObjectClass(Ref obj) {
  ENV_OR(0);
  return R(env->GetObjectClass(J(obj)));
}
bool ArtJava::IsInstanceOf(Ref obj, Ref cls) {
  ENV_OR(false);
  return env->IsInstanceOf(J(obj), C(cls)) != JNI_FALSE;
}
bool ArtJava::IsSameObject(Ref a, Ref b) {
  ENV_OR(false);
  return env->IsSameObject(J(a), J(b)) != JNI_FALSE;
}
ArtJava::Ref ArtJava::AllocObject(Ref cls) {
  ENV_OR(0);
  return R(env->AllocObject(C(cls)));
}
ArtJava::Ref ArtJava::NewLocalRef(Ref r) {
  ENV_OR(0);
  return R(env->NewLocalRef(J(r)));
}
void ArtJava::DeleteLocalRef(Ref r) {
  ENV_OR();
  env->DeleteLocalRef(J(r));
}
ArtJava::Ref ArtJava::NewGlobalRef(Ref r) {
  ENV_OR(0);
  return R(env->NewGlobalRef(J(r)));
}
void ArtJava::DeleteGlobalRef(Ref r) {
  ENV_OR();
  env->DeleteGlobalRef(J(r));
}
ArtJava::Ref ArtJava::NewWeakGlobalRef(Ref r) {
  ENV_OR(0);
  return R(env->NewWeakGlobalRef(J(r)));
}
void ArtJava::DeleteWeakGlobalRef(Ref r) {
  ENV_OR();
  env->DeleteWeakGlobalRef(reinterpret_cast<jweak>(r));
}

ArtJava::Id ArtJava::GetMethodID(Ref cls, const std::string& name, const std::string& sig, bool is_static) {
  ENV_OR(0);
  return reinterpret_cast<Id>(is_static ? env->GetStaticMethodID(C(cls), name.c_str(), sig.c_str())
                                        : env->GetMethodID(C(cls), name.c_str(), sig.c_str()));
}
ArtJava::Id ArtJava::GetFieldID(Ref cls, const std::string& name, const std::string& sig, bool is_static) {
  ENV_OR(0);
  return reinterpret_cast<Id>(is_static ? env->GetStaticFieldID(C(cls), name.c_str(), sig.c_str())
                                        : env->GetFieldID(C(cls), name.c_str(), sig.c_str()));
}

std::uint64_t ArtJava::Call(char ret_kind, bool is_static, Ref target, Ref nonvirtual_cls, Id method,
                            const std::vector<std::uint64_t>& args) {
  ENV_OR(0);
  const std::vector<jvalue> v = Jvalues(args);
  const jvalue* a = v.empty() ? nullptr : v.data();
  const auto m = reinterpret_cast<jmethodID>(method);
  const jobject o = J(target);
  const jclass c = C(target);
  const jclass nv = C(nonvirtual_cls);
#define RB_CALL(T, Type)                                                                     \
  (is_static ? env->CallStatic##Type##MethodA(c, m, a)                                       \
             : nv != nullptr ? env->CallNonvirtual##Type##MethodA(o, nv, m, a) : env->Call##Type##MethodA(o, m, a))
  switch (ret_kind) {
    case 'V':
      if (is_static) {
        env->CallStaticVoidMethodA(c, m, a);
      } else if (nv != nullptr) {
        env->CallNonvirtualVoidMethodA(o, nv, m, a);
      } else {
        env->CallVoidMethodA(o, m, a);
      }
      return 0;
    case 'L': return R(RB_CALL(jobject, Object));
    case 'Z': return static_cast<std::uint64_t>(RB_CALL(jboolean, Boolean));
    case 'B': return static_cast<std::uint64_t>(static_cast<std::int64_t>(RB_CALL(jbyte, Byte)));
    case 'C': return static_cast<std::uint64_t>(RB_CALL(jchar, Char));
    case 'S': return static_cast<std::uint64_t>(static_cast<std::int64_t>(RB_CALL(jshort, Short)));
    case 'I': return static_cast<std::uint32_t>(RB_CALL(jint, Int));
    case 'J': return static_cast<std::uint64_t>(RB_CALL(jlong, Long));
    case 'F': return Bits(RB_CALL(jfloat, Float));
    case 'D': return Bits(RB_CALL(jdouble, Double));
    default: return 0;
  }
#undef RB_CALL
}

ArtJava::Ref ArtJava::NewObject(Ref cls, Id ctor, const std::vector<std::uint64_t>& args) {
  ENV_OR(0);
  const std::vector<jvalue> v = Jvalues(args);
  return R(env->NewObjectA(C(cls), reinterpret_cast<jmethodID>(ctor), v.empty() ? nullptr : v.data()));
}

std::uint64_t ArtJava::GetField(char kind, bool is_static, Ref target, Id field) {
  ENV_OR(0);
  const auto f = reinterpret_cast<jfieldID>(field);
#define RB_GET(Type) (is_static ? env->GetStatic##Type##Field(C(target), f) : env->Get##Type##Field(J(target), f))
  switch (kind) {
    case 'L': return R(RB_GET(Object));
    case 'Z': return static_cast<std::uint64_t>(RB_GET(Boolean));
    case 'B': return static_cast<std::uint64_t>(static_cast<std::int64_t>(RB_GET(Byte)));
    case 'C': return static_cast<std::uint64_t>(RB_GET(Char));
    case 'S': return static_cast<std::uint64_t>(static_cast<std::int64_t>(RB_GET(Short)));
    case 'I': return static_cast<std::uint32_t>(RB_GET(Int));
    case 'J': return static_cast<std::uint64_t>(RB_GET(Long));
    case 'F': return Bits(RB_GET(Float));
    case 'D': return Bits(RB_GET(Double));
    default: return 0;
  }
#undef RB_GET
}

void ArtJava::SetField(char kind, bool is_static, Ref target, Id field, std::uint64_t value) {
  ENV_OR();
  const auto f = reinterpret_cast<jfieldID>(field);
#define RB_SET(Type, v)                         \
  (is_static ? env->SetStatic##Type##Field(C(target), f, v) : env->Set##Type##Field(J(target), f, v))
  switch (kind) {
    case 'L': RB_SET(Object, J(value)); break;
    case 'Z': RB_SET(Boolean, static_cast<jboolean>(value)); break;
    case 'B': RB_SET(Byte, static_cast<jbyte>(value)); break;
    case 'C': RB_SET(Char, static_cast<jchar>(value)); break;
    case 'S': RB_SET(Short, static_cast<jshort>(value)); break;
    case 'I': RB_SET(Int, static_cast<jint>(value)); break;
    case 'J': RB_SET(Long, static_cast<jlong>(value)); break;
    case 'F': RB_SET(Float, From<jfloat>(value)); break;
    case 'D': RB_SET(Double, From<jdouble>(value)); break;
    default: break;
  }
#undef RB_SET
}

ArtJava::Ref ArtJava::NewString(const std::u16string& chars) {
  ENV_OR(0);
  return R(env->NewString(reinterpret_cast<const jchar*>(chars.data()), static_cast<jsize>(chars.size())));
}

std::u16string ArtJava::GetStringChars(Ref s) {
  ENV_OR({});
  const jsize n = env->GetStringLength(static_cast<jstring>(J(s)));
  std::u16string out(static_cast<std::size_t>(n), u'\0');
  if (n > 0) env->GetStringRegion(static_cast<jstring>(J(s)), 0, n, reinterpret_cast<jchar*>(out.data()));
  return out;
}

ArtJava::Ref ArtJava::NewArray(char kind, std::int32_t length, Ref elem_cls, Ref init) {
  ENV_OR(0);
  switch (kind) {
    case 'Z': return R(env->NewBooleanArray(length));
    case 'B': return R(env->NewByteArray(length));
    case 'C': return R(env->NewCharArray(length));
    case 'S': return R(env->NewShortArray(length));
    case 'I': return R(env->NewIntArray(length));
    case 'J': return R(env->NewLongArray(length));
    case 'F': return R(env->NewFloatArray(length));
    case 'D': return R(env->NewDoubleArray(length));
    case 'L': return R(env->NewObjectArray(length, C(elem_cls), J(init)));
    default: return 0;
  }
}

std::int32_t ArtJava::GetArrayLength(Ref a) {
  ENV_OR(0);
  return env->GetArrayLength(static_cast<jarray>(J(a)));
}

char ArtJava::ArrayKind(Ref a) {
  ENV_OR(0);
  std::call_once(arrays_once_, [&] {
    static const char* const kNames[9] = {"[Z", "[B", "[C", "[S", "[I", "[J", "[F", "[D", "[Ljava/lang/Object;"};
    for (int i = 0; i < 9; ++i) {
      jclass c = env->FindClass(kNames[i]);
      array_classes_[i] = c != nullptr ? static_cast<jclass>(env->NewGlobalRef(c)) : nullptr;
      env->DeleteLocalRef(c);
    }
  });
  static const char kKinds[9] = {'Z', 'B', 'C', 'S', 'I', 'J', 'F', 'D', 'L'};
  for (int i = 0; i < 9; ++i) {
    if (array_classes_[i] != nullptr && env->IsInstanceOf(J(a), array_classes_[i])) return kKinds[i];
  }
  return 0;
}

bool ArtJava::GetArrayRegion(char kind, Ref a, std::int32_t start, std::int32_t len, void* out) {
  ENV_OR(false);
  switch (kind) {
    case 'Z': env->GetBooleanArrayRegion(static_cast<jbooleanArray>(J(a)), start, len, static_cast<jboolean*>(out)); break;
    case 'B': env->GetByteArrayRegion(static_cast<jbyteArray>(J(a)), start, len, static_cast<jbyte*>(out)); break;
    case 'C': env->GetCharArrayRegion(static_cast<jcharArray>(J(a)), start, len, static_cast<jchar*>(out)); break;
    case 'S': env->GetShortArrayRegion(static_cast<jshortArray>(J(a)), start, len, static_cast<jshort*>(out)); break;
    case 'I': env->GetIntArrayRegion(static_cast<jintArray>(J(a)), start, len, static_cast<jint*>(out)); break;
    case 'J': env->GetLongArrayRegion(static_cast<jlongArray>(J(a)), start, len, static_cast<jlong*>(out)); break;
    case 'F': env->GetFloatArrayRegion(static_cast<jfloatArray>(J(a)), start, len, static_cast<jfloat*>(out)); break;
    case 'D': env->GetDoubleArrayRegion(static_cast<jdoubleArray>(J(a)), start, len, static_cast<jdouble*>(out)); break;
    default: return false;
  }
  return env->ExceptionCheck() == JNI_FALSE;
}

bool ArtJava::SetArrayRegion(char kind, Ref a, std::int32_t start, std::int32_t len, const void* in) {
  ENV_OR(false);
  switch (kind) {
    case 'Z': env->SetBooleanArrayRegion(static_cast<jbooleanArray>(J(a)), start, len, static_cast<const jboolean*>(in)); break;
    case 'B': env->SetByteArrayRegion(static_cast<jbyteArray>(J(a)), start, len, static_cast<const jbyte*>(in)); break;
    case 'C': env->SetCharArrayRegion(static_cast<jcharArray>(J(a)), start, len, static_cast<const jchar*>(in)); break;
    case 'S': env->SetShortArrayRegion(static_cast<jshortArray>(J(a)), start, len, static_cast<const jshort*>(in)); break;
    case 'I': env->SetIntArrayRegion(static_cast<jintArray>(J(a)), start, len, static_cast<const jint*>(in)); break;
    case 'J': env->SetLongArrayRegion(static_cast<jlongArray>(J(a)), start, len, static_cast<const jlong*>(in)); break;
    case 'F': env->SetFloatArrayRegion(static_cast<jfloatArray>(J(a)), start, len, static_cast<const jfloat*>(in)); break;
    case 'D': env->SetDoubleArrayRegion(static_cast<jdoubleArray>(J(a)), start, len, static_cast<const jdouble*>(in)); break;
    default: return false;
  }
  return env->ExceptionCheck() == JNI_FALSE;
}

ArtJava::Ref ArtJava::GetObjectArrayElement(Ref a, std::int32_t index) {
  ENV_OR(0);
  return R(env->GetObjectArrayElement(static_cast<jobjectArray>(J(a)), index));
}
void ArtJava::SetObjectArrayElement(Ref a, std::int32_t index, Ref value) {
  ENV_OR();
  env->SetObjectArrayElement(static_cast<jobjectArray>(J(a)), index, J(value));
}

bool ArtJava::ExceptionCheck() {
  ENV_OR(false);
  return env->ExceptionCheck() != JNI_FALSE;
}
std::int32_t ArtJava::Throw(Ref throwable) {
  ENV_OR(-1);
  return env->Throw(static_cast<jthrowable>(J(throwable)));
}
std::int32_t ArtJava::ThrowNew(Ref cls, const std::string& message) {
  ENV_OR(-1);
  return env->ThrowNew(C(cls), message.c_str());
}
ArtJava::Ref ArtJava::ExceptionOccurred() {
  ENV_OR(0);
  return R(env->ExceptionOccurred());
}
void ArtJava::ExceptionDescribe() {
  ENV_OR();
  env->ExceptionDescribe();
}
void ArtJava::ExceptionClear() {
  ENV_OR();
  env->ExceptionClear();
}

std::int32_t ArtJava::PushLocalFrame(std::int32_t capacity) {
  ENV_OR(-1);
  return env->PushLocalFrame(capacity);
}
ArtJava::Ref ArtJava::PopLocalFrame(Ref result) {
  ENV_OR(0);
  return R(env->PopLocalFrame(J(result)));
}
std::int32_t ArtJava::MonitorEnter(Ref obj) {
  ENV_OR(-1);
  return env->MonitorEnter(J(obj));
}
std::int32_t ArtJava::MonitorExit(Ref obj) {
  ENV_OR(-1);
  return env->MonitorExit(J(obj));
}

std::int32_t ArtJava::RegisterNatives(Ref cls, const std::vector<NativeBinding>& methods) {
  ENV_OR(-1);
  std::vector<JNINativeMethod> m;
  m.reserve(methods.size());
  for (const NativeBinding& b : methods) m.push_back({b.name.c_str(), b.sig.c_str(), b.host_fn});
  return env->RegisterNatives(C(cls), m.data(), static_cast<jint>(m.size()));
}
std::int32_t ArtJava::UnregisterNatives(Ref cls) {
  ENV_OR(-1);
  return env->UnregisterNatives(C(cls));
}

ArtJava::Ref ArtJava::ToReflected(Ref cls, Id id, bool is_field, bool is_static) {
  ENV_OR(0);
  return is_field ? R(env->ToReflectedField(C(cls), reinterpret_cast<jfieldID>(id), is_static ? JNI_TRUE : JNI_FALSE))
                  : R(env->ToReflectedMethod(C(cls), reinterpret_cast<jmethodID>(id), is_static ? JNI_TRUE : JNI_FALSE));
}

std::string ArtJava::Descriptor(JNIEnv* env, jclass c) {
  jclass k = env->GetObjectClass(c);  // java.lang.Class
  jmethodID get_name = env->GetMethodID(k, "getName", "()Ljava/lang/String;");
  jmethodID is_primitive = env->GetMethodID(k, "isPrimitive", "()Z");
  env->DeleteLocalRef(k);
  if (get_name == nullptr || is_primitive == nullptr) return {};
  auto name = static_cast<jstring>(env->CallObjectMethod(c, get_name));
  const char* chars = name != nullptr ? env->GetStringUTFChars(name, nullptr) : nullptr;
  std::string n = chars != nullptr ? chars : "";
  if (chars != nullptr) env->ReleaseStringUTFChars(name, chars);
  env->DeleteLocalRef(name);
  for (char& ch : n) {
    if (ch == '.') ch = '/';
  }
  if (env->CallBooleanMethod(c, is_primitive) != JNI_FALSE) {
    static const char* const kPrim[][2] = {{"void", "V"},  {"boolean", "Z"}, {"byte", "B"},  {"char", "C"},  {"short", "S"},
                                           {"int", "I"},   {"long", "J"},    {"float", "F"}, {"double", "D"}};
    for (const auto& p : kPrim) {
      if (n == p[0]) return p[1];
    }
    return {};
  }
  return n[0] == '[' ? n : "L" + n + ";";  // getName() is already a descriptor for arrays
}

ArtJava::Id ArtJava::FromReflected(Ref member, bool is_field, std::string* name, std::string* sig, bool* is_static) {
  ENV_OR(0);
  const jobject m = J(member);
  jclass mc = env->GetObjectClass(m);
  const auto string_of = [&](jmethodID id) {
    auto s = static_cast<jstring>(env->CallObjectMethod(m, id));
    const char* c = s != nullptr ? env->GetStringUTFChars(s, nullptr) : nullptr;
    std::string out = c != nullptr ? c : "";
    if (c != nullptr) env->ReleaseStringUTFChars(s, c);
    env->DeleteLocalRef(s);
    return out;
  };
  jmethodID get_modifiers = env->GetMethodID(mc, "getModifiers", "()I");
  *is_static = get_modifiers != nullptr && (env->CallIntMethod(m, get_modifiers) & 0x8) != 0;
  Id id = 0;
  if (is_field) {
    *name = string_of(env->GetMethodID(mc, "getName", "()Ljava/lang/String;"));
    auto type = static_cast<jclass>(env->CallObjectMethod(m, env->GetMethodID(mc, "getType", "()Ljava/lang/Class;")));
    *sig = Descriptor(env, type);
    env->DeleteLocalRef(type);
    id = reinterpret_cast<Id>(env->FromReflectedField(m));
  } else {
    jclass ctor_class = env->FindClass("java/lang/reflect/Constructor");
    const bool ctor = ctor_class != nullptr && env->IsInstanceOf(m, ctor_class);
    env->DeleteLocalRef(ctor_class);
    *name = ctor ? "<init>" : string_of(env->GetMethodID(mc, "getName", "()Ljava/lang/String;"));
    auto params = static_cast<jobjectArray>(
        env->CallObjectMethod(m, env->GetMethodID(mc, "getParameterTypes", "()[Ljava/lang/Class;")));
    std::string s = "(";
    const jsize n = params != nullptr ? env->GetArrayLength(params) : 0;
    for (jsize i = 0; i < n; ++i) {
      auto p = static_cast<jclass>(env->GetObjectArrayElement(params, i));
      s += Descriptor(env, p);
      env->DeleteLocalRef(p);
    }
    env->DeleteLocalRef(params);
    s += ")";
    if (ctor) {
      s += "V";
    } else {
      auto ret = static_cast<jclass>(env->CallObjectMethod(m, env->GetMethodID(mc, "getReturnType", "()Ljava/lang/Class;")));
      s += Descriptor(env, ret);
      env->DeleteLocalRef(ret);
    }
    *sig = s;
    id = reinterpret_cast<Id>(env->FromReflectedMethod(m));
  }
  env->DeleteLocalRef(mc);
  return id;
}

ArtJava::Ref ArtJava::NewDirectByteBuffer(void* address, std::int64_t capacity) {
  ENV_OR(0);
  return R(env->NewDirectByteBuffer(address, capacity));
}
void* ArtJava::GetDirectBufferAddress(Ref buf) {
  ENV_OR(nullptr);
  return env->GetDirectBufferAddress(J(buf));
}
std::int64_t ArtJava::GetDirectBufferCapacity(Ref buf) {
  ENV_OR(-1);
  return env->GetDirectBufferCapacity(J(buf));
}

bool ArtJava::IsAttached() {
  JNIEnv* env = nullptr;
  return vm_ != nullptr && vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK;
}

bool ArtJava::AttachCurrentThread(bool daemon) {
  if (IsAttached()) return true;
  JNIEnv* env = nullptr;
  const jint rc = daemon ? vm_->AttachCurrentThreadAsDaemon(&env, nullptr) : vm_->AttachCurrentThread(&env, nullptr);
  if (rc != JNI_OK) return false;
  MarkAttached(vm_);
  return true;
}

void ArtJava::DetachCurrentThread() {
  // Only a thread this bridge attached may be detached: detaching one of ART's own (the UI thread, a GLThread)
  // from inside a native would break its Java caller.
  std::call_once(g_key_once, [] { pthread_key_create(&g_attached_key, DetachAtExit); });
  if (pthread_getspecific(g_attached_key) == nullptr) return;
  pthread_setspecific(g_attached_key, nullptr);
  vm_->DetachCurrentThread();
}

#undef ENV_OR

}  // namespace rb::launcher
