/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * libjnibridge_fixture.so: 32-bit guest code that drives the forwarding JNI bridge (M3). Each fx_* function takes
 * the guest JNIEnv* first and makes ordinary JNI calls, the way a game's native code does; the core lane
 * rb_jni_forward_tests calls them on both tiers against a fake Java (core/tests/fake_java.h). Built armeabi-v7a
 * Thumb softfp like the pthread fixture (tools/run_engine_tests.sh), so the same code also runs natively.
 */
#include <jni.h>

/* FindClass, GetStaticMethodID, CallStaticIntMethod: rb/Fixture.twice(21) = 42. */
__attribute__((visibility("default"))) int fx_find_and_call(JNIEnv* env) {
  jclass c = (*env)->FindClass(env, "rb/Fixture");
  if (c == 0) return -1;
  jmethodID m = (*env)->GetStaticMethodID(env, c, "twice", "(I)I");
  if (m == 0) return -2;
  return (*env)->CallStaticIntMethod(env, c, m, 21);
}

/* Two lookups of one method give one ID; another method a different one. Result: 1 + 10. */
__attribute__((visibility("default"))) int fx_ids(JNIEnv* env) {
  jclass c = (*env)->FindClass(env, "rb/Fixture");
  jmethodID a = (*env)->GetStaticMethodID(env, c, "twice", "(I)I");
  jmethodID b = (*env)->GetStaticMethodID(env, c, "twice", "(I)I");
  jmethodID d = (*env)->GetStaticMethodID(env, c, "mix", "(IJFDZ)J");
  return (a == b ? 1 : 0) + (a != d && d != 0 ? 10 : 0);
}

/* Instance and static fields: i = 7, d = 2.5, s = obj. Result: 7 + (int)(2.5 * 2) + 100 if s reads back as obj. */
__attribute__((visibility("default"))) int fx_fields(JNIEnv* env, jobject obj) {
  jclass cls = (*env)->GetObjectClass(env, obj);
  jfieldID fi = (*env)->GetFieldID(env, cls, "i", "I");
  jfieldID fd = (*env)->GetFieldID(env, cls, "d", "D");
  jfieldID fs = (*env)->GetStaticFieldID(env, cls, "s", "Ljava/lang/Object;");
  if (fi == 0 || fd == 0 || fs == 0) return -1;
  (*env)->SetIntField(env, obj, fi, 7);
  (*env)->SetDoubleField(env, obj, fd, 2.5);
  (*env)->SetStaticObjectField(env, cls, fs, obj);
  jobject back = (*env)->GetStaticObjectField(env, cls, fs);
  return (*env)->GetIntField(env, obj, fi) + (int)((*env)->GetDoubleField(env, obj, fd) * 2) +
         ((*env)->IsSameObject(env, back, obj) ? 100 : 0);
}

/* who() is 1 on rb/Fixture and 2 on rb/Sub; obj is a Sub. Result: virtual * 10 + nonvirtual(super) = 21. */
__attribute__((visibility("default"))) int fx_virtual_vs_nonvirtual(JNIEnv* env, jobject obj) {
  jclass cls = (*env)->GetObjectClass(env, obj);
  jclass sup = (*env)->GetSuperclass(env, cls);
  jmethodID m = (*env)->GetMethodID(env, sup, "who", "()I");
  if (m == 0) return -1;
  return (*env)->CallIntMethod(env, obj, m) * 10 + (*env)->CallNonvirtualIntMethod(env, obj, sup, m);
}

/* rb/Fixture.mix(IJFDZ)J in all three forms with the same arguments; each returns its own fold. Result: the
 * variadic form's value when all three agree, else -1. */
static jlong mix_v(JNIEnv* env, jclass c, jmethodID m, ...) {
  va_list ap;
  va_start(ap, m);
  jlong r = (*env)->CallStaticLongMethodV(env, c, m, ap);
  va_end(ap);
  return r;
}
__attribute__((visibility("default"))) jlong fx_args_in_all_forms(JNIEnv* env) {
  jclass c = (*env)->FindClass(env, "rb/Fixture");
  jmethodID m = (*env)->GetStaticMethodID(env, c, "mix", "(IJFDZ)J");
  const jlong big = (jlong)3 << 40;
  jlong a = (*env)->CallStaticLongMethod(env, c, m, 5, big, 1.5f, 2.25, JNI_TRUE);
  jlong b = mix_v(env, c, m, 5, big, 1.5f, 2.25, JNI_TRUE);
  jvalue v[5];
  v[0].i = 5;
  v[1].j = big;
  v[2].f = 1.5f;
  v[3].d = 2.25;
  v[4].z = JNI_TRUE;
  jlong d = (*env)->CallStaticLongMethodA(env, c, m, v);
  return (a == b && b == d) ? a : -1;
}

/* Makes n locals, deletes every other one; counts those still local. Result: n / 2. */
static jobject g_locals[4096];
__attribute__((visibility("default"))) int fx_locals(JNIEnv* env, int n) {
  if (n > 4096) return -1;
  jclass c = (*env)->FindClass(env, "rb/Fixture");
  for (int i = 0; i < n; ++i) g_locals[i] = (*env)->AllocObject(env, c);
  for (int i = 0; i < n; i += 2) (*env)->DeleteLocalRef(env, g_locals[i]);
  int live = 0;
  for (int i = 1; i < n; i += 2) live += (*env)->GetObjectRefType(env, g_locals[i]) == JNILocalRefType;
  for (int i = 1; i < n; i += 2) (*env)->DeleteLocalRef(env, g_locals[i]);
  return live;
}

/* NewObject with rb/Fixture.<init>(I)V setting i. Result: 5. */
__attribute__((visibility("default"))) int fx_new_object(JNIEnv* env) {
  jclass c = (*env)->FindClass(env, "rb/Fixture");
  jmethodID ctor = (*env)->GetMethodID(env, c, "<init>", "(I)V");
  jobject o = (*env)->NewObject(env, c, ctor, 5);
  jfieldID f = (*env)->GetFieldID(env, c, "i", "I");
  return (*env)->GetIntField(env, o, f);
}

/* Uses a local after deleting it: the bridge must refuse by name. */
__attribute__((visibility("default"))) int fx_use_deleted(JNIEnv* env) {
  jclass c = (*env)->FindClass(env, "rb/Fixture");
  jobject o = (*env)->AllocObject(env, c);
  (*env)->DeleteLocalRef(env, o);
  return (*env)->GetObjectClass(env, o) != 0;
}

/* A local for the caller to hand to another thread, and the use of one. */
__attribute__((visibility("default"))) jobject fx_make_local(JNIEnv* env) {
  return (*env)->AllocObject(env, (*env)->FindClass(env, "rb/Fixture"));
}
__attribute__((visibility("default"))) int fx_class_of(JNIEnv* env, jobject o) {
  return (*env)->GetObjectClass(env, o) != 0;
}

/* No JNI at all: proves a guest call can run while another thread is inside real Java. */
__attribute__((visibility("default"))) int fx_plain(int x) { return x + 1; }
