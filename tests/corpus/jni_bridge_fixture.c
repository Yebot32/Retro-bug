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

/* ---- Task 2 ---------------------------------------------------------------------------------------------- */

static unsigned fnv(const unsigned char* p, int n) {
  unsigned h = 2166136261u;
  for (int i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}
static int cstrlen(const char* s) {
  int n = 0;
  while (s[n] != 0) ++n;
  return n;
}

/* Everything a string offers, into out[0..6]: UTF length, UTF-16 length, hash of the UTF bytes, isCopy,
 * GetStringRegion(1, 2) packed, hash of GetStringUTFRegion(0, 2), GetStringChars[0]. */
__attribute__((visibility("default"))) int fx_string_info(JNIEnv* env, jstring s, int* out) {
  jboolean is_copy = 0;
  const char* u = (*env)->GetStringUTFChars(env, s, &is_copy);
  if (u == 0) return -1;
  out[0] = (*env)->GetStringUTFLength(env, s);
  out[1] = (*env)->GetStringLength(env, s);
  out[2] = (int)fnv((const unsigned char*)u, out[0]);
  out[3] = is_copy;
  (*env)->ReleaseStringUTFChars(env, s, u);
  jchar region[2] = {0xffff, 0xffff};
  (*env)->GetStringRegion(env, s, 1, 2, region);
  out[4] = region[0] | (region[1] << 16);
  char utf_region[16] = {0};
  (*env)->GetStringUTFRegion(env, s, 0, 2, utf_region);
  out[5] = (int)fnv((const unsigned char*)utf_region, cstrlen(utf_region));
  const jchar* chars = (*env)->GetStringChars(env, s, 0);
  out[6] = chars[0];
  (*env)->ReleaseStringChars(env, s, chars);
  return 0;
}

__attribute__((visibility("default"))) jstring fx_new_string_utf(JNIEnv* env, const char* bytes) {
  return (*env)->NewStringUTF(env, bytes);
}
__attribute__((visibility("default"))) jstring fx_new_string(JNIEnv* env, const jchar* chars, int n) {
  return (*env)->NewString(env, chars, n);
}

/* Arrays of every kind and every release mode. Result: a bitmask, 0x1ff when all hold. */
__attribute__((visibility("default"))) int fx_arrays(JNIEnv* env, jobject obj) {
  int ok = 0;
  jintArray ia = (*env)->NewIntArray(env, 4);
  const jint v[4] = {1, 2, 3, 4};
  (*env)->SetIntArrayRegion(env, ia, 0, 4, v);
  jint* e = (*env)->GetIntArrayElements(env, ia, 0);
  e[0] = 10;
  (*env)->ReleaseIntArrayElements(env, ia, e, JNI_ABORT); /* not copied back */
  e = (*env)->GetIntArrayElements(env, ia, 0);
  e[1] = 20;
  (*env)->ReleaseIntArrayElements(env, ia, e, JNI_COMMIT); /* copied back, buffer kept */
  e[2] = 30;
  (*env)->ReleaseIntArrayElements(env, ia, e, 0); /* copied back, freed */
  jint r[4] = {0, 0, 0, 0};
  (*env)->GetIntArrayRegion(env, ia, 0, 4, r);
  if (r[0] == 1 && r[1] == 20 && r[2] == 30 && r[3] == 4) ok |= 1;
  if ((*env)->GetArrayLength(env, ia) == 4) ok |= 2;
  jlongArray la = (*env)->NewLongArray(env, 2);
  const jlong lv[2] = {(jlong)1 << 40, -7};
  (*env)->SetLongArrayRegion(env, la, 0, 2, lv);
  jlong lr[2] = {0, 0};
  (*env)->GetLongArrayRegion(env, la, 0, 2, lr);
  if (lr[0] == ((jlong)1 << 40) && lr[1] == -7) ok |= 4;
  jdoubleArray da = (*env)->NewDoubleArray(env, 1);
  double* de = (*env)->GetDoubleArrayElements(env, da, 0);
  de[0] = 0.125;
  (*env)->ReleaseDoubleArrayElements(env, da, de, 0);
  double dr = 0;
  (*env)->GetDoubleArrayRegion(env, da, 0, 1, &dr);
  if (dr == 0.125) ok |= 8;
  jbyteArray ba = (*env)->NewByteArray(env, 3);
  const jbyte bv[3] = {-1, 2, 3};
  (*env)->SetByteArrayRegion(env, ba, 0, 3, bv);
  jbyte* crit = (jbyte*)(*env)->GetPrimitiveArrayCritical(env, ba, 0);
  crit[2] = 9;
  (*env)->ReleasePrimitiveArrayCritical(env, ba, crit, 0);
  jbyte br[3] = {0, 0, 0};
  (*env)->GetByteArrayRegion(env, ba, 0, 3, br);
  if (br[0] == -1 && br[1] == 2 && br[2] == 9) ok |= 16;
  jclass c = (*env)->GetObjectClass(env, obj);
  jobjectArray oa = (*env)->NewObjectArray(env, 2, c, 0);
  (*env)->SetObjectArrayElement(env, oa, 1, obj);
  jobject back = (*env)->GetObjectArrayElement(env, oa, 1);
  if ((*env)->IsSameObject(env, back, obj) && (*env)->GetObjectArrayElement(env, oa, 0) == 0) ok |= 32;
  jcharArray ca = (*env)->NewCharArray(env, 2);
  const jchar cv[2] = {0x263a, 'x'};
  (*env)->SetCharArrayRegion(env, ca, 0, 2, cv);
  jchar cr[2] = {0, 0};
  (*env)->GetCharArrayRegion(env, ca, 0, 2, cr);
  if (cr[0] == 0x263a && cr[1] == 'x') ok |= 64;
  jfloatArray fa = (*env)->NewFloatArray(env, 1);
  const jfloat fv = 1.75f;
  (*env)->SetFloatArrayRegion(env, fa, 0, 1, &fv);
  jfloat fr = 0;
  (*env)->GetFloatArrayRegion(env, fa, 0, 1, &fr);
  if (fr == 1.75f) ok |= 128;
  jint oob[1];
  (*env)->GetIntArrayRegion(env, ia, 3, 2, oob); /* out of bounds: an exception, not a crash */
  if ((*env)->ExceptionCheck(env)) {
    ok |= 256;
    (*env)->ExceptionClear(env);
  }
  return ok;
}

/* Exceptions. Result: 31 when every step behaved as on a device. */
__attribute__((visibility("default"))) int fx_exceptions(JNIEnv* env) {
  int r = 0;
  jclass c = (*env)->FindClass(env, "java/lang/IllegalStateException");
  if ((*env)->ThrowNew(env, c, "boom") == 0 && (*env)->ExceptionCheck(env)) r |= 1;
  jthrowable t = (*env)->ExceptionOccurred(env);
  if (t != 0 && (*env)->IsInstanceOf(env, t, c)) r |= 2;
  jclass f = (*env)->FindClass(env, "rb/Fixture"); /* while pending: forwarded, and named in a note */
  if (f != 0) r |= 4;
  (*env)->ExceptionClear(env);
  if (!(*env)->ExceptionCheck(env)) r |= 8;
  if ((*env)->Throw(env, t) == 0 && (*env)->ExceptionCheck(env)) r |= 16;
  (*env)->ExceptionClear(env);
  return r;
}

/* PushLocalFrame / PopLocalFrame: the result survives as a new local in the outer frame naming the same object.
 * Result: 3. */
__attribute__((visibility("default"))) int fx_frames(JNIEnv* env) {
  jclass c = (*env)->FindClass(env, "rb/Fixture");
  if ((*env)->PushLocalFrame(env, 16) != 0) return -1;
  jobject inner = (*env)->AllocObject(env, c);
  jobject global = (*env)->NewGlobalRef(env, inner);
  jobject kept = (*env)->PopLocalFrame(env, inner);
  const int r = ((*env)->GetObjectRefType(env, kept) == JNILocalRefType ? 1 : 0) +
                ((*env)->IsSameObject(env, kept, global) ? 2 : 0);
  (*env)->DeleteGlobalRef(env, global);
  return r;
}

/* Uses a local after the frame that made it popped: the bridge must refuse it by name (on a device, undefined). */
__attribute__((visibility("default"))) int fx_use_after_pop(JNIEnv* env) {
  jclass c = (*env)->FindClass(env, "rb/Fixture");
  (*env)->PushLocalFrame(env, 4);
  jobject inner = (*env)->AllocObject(env, c);
  (*env)->PopLocalFrame(env, 0);
  (*env)->AllocObject(env, c); /* takes the popped slot */
  return (*env)->GetObjectClass(env, inner) != 0;
}

__attribute__((visibility("default"))) int fx_monitor(JNIEnv* env, jobject o) {
  return (*env)->MonitorEnter(env, o) + (*env)->MonitorEnter(env, o) + (*env)->MonitorExit(env, o) +
         (*env)->MonitorExit(env, o);
}

__attribute__((visibility("default"))) jint fx_native_add(JNIEnv* env, jclass cls, jint a, jint b) {
  (void)env;
  (void)cls;
  return a + b;
}
__attribute__((visibility("default"))) int fx_register(JNIEnv* env) {
  jclass c = (*env)->FindClass(env, "rb/Fixture");
  const JNINativeMethod m[1] = {{"nativeAdd", "(II)I", (void*)fx_native_add}};
  return (*env)->RegisterNatives(env, c, m, 1);
}

/* The JavaVM: GetJavaVM, GetEnv for 1.6 and for an unknown version, AttachCurrentThread. Result: 7. */
__attribute__((visibility("default"))) int fx_vm(JNIEnv* env) {
  JavaVM* vm = 0;
  if ((*env)->GetJavaVM(env, &vm) != 0 || vm == 0) return -1;
  int r = 0;
  JNIEnv* e2 = 0;
  if ((*vm)->GetEnv(vm, (void**)&e2, JNI_VERSION_1_6) == JNI_OK && e2 == env) r |= 1;
  if ((*vm)->GetEnv(vm, (void**)&e2, 0x00010008) == JNI_EVERSION) r |= 2;
  JNIEnv* e3 = 0;
  if ((*vm)->AttachCurrentThread(vm, &e3, 0) == JNI_OK && e3 == env) r |= 4;
  return r;
}

/* A direct buffer over guest memory. Result: 3. */
__attribute__((visibility("default"))) int fx_direct(JNIEnv* env, void* p) {
  jobject b = (*env)->NewDirectByteBuffer(env, p, 64);
  return ((*env)->GetDirectBufferAddress(env, b) == p ? 1 : 0) + ((*env)->GetDirectBufferCapacity(env, b) == 64 ? 2 : 0);
}
__attribute__((visibility("default"))) void* fx_direct_address(JNIEnv* env, jobject b) {
  return (*env)->GetDirectBufferAddress(env, b);
}

/* ToReflectedMethod then FromReflectedMethod gives a usable ID. Result: twice(4) = 8. */
__attribute__((visibility("default"))) int fx_reflect(JNIEnv* env) {
  jclass c = (*env)->FindClass(env, "rb/Fixture");
  jmethodID m = (*env)->GetStaticMethodID(env, c, "twice", "(I)I");
  jobject reflected = (*env)->ToReflectedMethod(env, c, m, JNI_TRUE);
  jmethodID back = (*env)->FromReflectedMethod(env, reflected);
  return (*env)->CallStaticIntMethod(env, c, back, 4);
}

/* ---- Task 3: natives Java calls (through the inbound dispatcher) ------------------------------------------ */

/* Twenty arguments of every width, enough to spill both AAPCS64 register files onto the stack: 8 integers
 * (x2..x7, then two stacked) and 10 floating-point values (d0..d7, then two stacked). The fold weighs each so a
 * misplaced argument changes the result. Float-to-int conversions are 32-bit (one VCVT, no runtime helper). */
__attribute__((visibility("default"))) jlong Java_rb_Fixture_mix(JNIEnv* env, jclass cls, jint a, jlong b, jfloat c,
                                                                 jdouble d, jint e, jlong f, jfloat g, jdouble h,
                                                                 jint i, jint j, jint k, jint l, jfloat m, jfloat n,
                                                                 jdouble o, jdouble p, jfloat q, jdouble r) {
  (void)env;
  (void)cls;
  return a + b * 3 + (jint)(c * 8) + (jint)(d * 16) + e * 5 + f * 7 + (jint)(g * 32) + (jint)(h * 64) +
         i * 11 + j * 13 + k * 17 + l * 19 + (jint)(m * 128) + (jint)(n * 256) + (jint)(o * 512) +
         (jint)(p * 1024) + (jint)(q * 2048) + (jint)(r * 4096);
}

__attribute__((visibility("default"))) jobject Java_rb_Fixture_echoObject(JNIEnv* env, jclass cls, jobject o) {
  (void)cls;
  jobject extra = (*env)->NewLocalRef(env, o); /* a local of its own, which the frame must take back */
  (void)extra;
  return o;
}

__attribute__((visibility("default"))) jdouble Java_rb_Fixture_half(JNIEnv* env, jclass cls, jfloat x) {
  (void)env;
  (void)cls;
  return x / 2.0;
}

__attribute__((visibility("default"))) jfloat Java_rb_Fixture_third(JNIEnv* env, jclass cls, jdouble x) {
  (void)env;
  (void)cls;
  return (jfloat)(x / 3.0);
}

__attribute__((visibility("default"))) jint Java_rb_Fixture_fails(JNIEnv* env, jclass cls) {
  (void)env;
  (void)cls;
  return *(volatile jint*)0;
}
