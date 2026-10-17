/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * libjnibridge.so's app half (M3 Task 6): the natives com.retrobug.jnibridge.BridgeActivity declares, and a
 * JNI_OnLoad that binds BridgeActivity$Registered.nativeAdd by RegisterNatives. Linked with
 * tests/corpus/jni_bridge_fixture.c (the core lanes' fixture) into one armeabi-v7a library. Every result is built
 * from what JNI answered, so a bridged run that differs from the native one changes a printed value.
 */
#include <jni.h>
#include <stdio.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))
#define P(name) Java_com_retrobug_jnibridge_BridgeActivity_##name

static unsigned fnv(const void* p, size_t n) {
  const unsigned char* b = (const unsigned char*)p;
  unsigned h = 0x811c9dc5u;
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x01000193u;
  return h;
}

static jboolean java_equals(JNIEnv* env, jobject a, jobject b) {
  jclass c = (*env)->FindClass(env, "java/lang/Object");
  jmethodID eq = (*env)->GetMethodID(env, c, "equals", "(Ljava/lang/Object;)Z");
  return (*env)->CallBooleanMethod(env, a, eq, b);
}

/* MUTF-8 and UTF-16 views of both strings, each round-tripped back into a new String. */
EXPORT jstring P(strings)(JNIEnv* env, jclass cls, jstring a, jstring b) {
  (void)cls;
  char out[512];
  jsize alen = (*env)->GetStringLength(env, a), autf = (*env)->GetStringUTFLength(env, a);
  const char* au = (*env)->GetStringUTFChars(env, a, 0);
  unsigned ah = fnv(au, (size_t)autf);
  jstring art = (*env)->NewStringUTF(env, au);
  (*env)->ReleaseStringUTFChars(env, a, au);
  char region[64];
  memset(region, 0x55, sizeof region);
  (*env)->GetStringUTFRegion(env, a, 0, alen, region);
  unsigned rh = fnv(region, (size_t)autf + 1);  /* the bytes and the NUL JNI writes after them */
  jsize blen = (*env)->GetStringLength(env, b), butf = (*env)->GetStringUTFLength(env, b);
  const jchar* bc = (*env)->GetStringChars(env, b, 0);
  unsigned bh = fnv(bc, (size_t)blen * 2);
  jstring brt = (*env)->NewString(env, bc, blen);
  (*env)->ReleaseStringChars(env, b, bc);
  const jchar* crit = (*env)->GetStringCritical(env, b, 0);
  unsigned ch = fnv(crit, (size_t)blen * 2);
  (*env)->ReleaseStringCritical(env, b, crit);
  jchar r10[10];
  (*env)->GetStringRegion(env, b, 1000, 10, r10);
  snprintf(out, sizeof out, "a len %d utf %d h %08x rt %d region %08x; b len %d utf %d h %08x crit %d rt %d r10 %08x",
           (int)alen, (int)autf, ah, java_equals(env, a, art), rh, (int)blen, (int)butf, bh, ch == bh,
           java_equals(env, b, brt), fnv(r10, sizeof r10));
  return (*env)->NewStringUTF(env, out);
}

/* Every primitive kind made here, the caller's arrays changed through each release mode, an object array. */
EXPORT jstring P(arrays)(JNIEnv* env, jclass cls, jintArray ints, jdoubleArray doubles, jobjectArray objects) {
  (void)cls;
  char out[512];
  jint* ip = (*env)->GetIntArrayElements(env, ints, 0);
  ip[0] = 10;
  (*env)->ReleaseIntArrayElements(env, ints, ip, JNI_COMMIT); /* written back, still held */
  ip[1] = 20;
  (*env)->ReleaseIntArrayElements(env, ints, ip, 0); /* written back, freed */
  jint* crit = (jint*)(*env)->GetPrimitiveArrayCritical(env, ints, 0);
  crit[2] += 100;
  (*env)->ReleasePrimitiveArrayCritical(env, ints, crit, 0);
  jint tail = 40;
  (*env)->SetIntArrayRegion(env, ints, 3, 1, &tail);
  jdouble* dp = (*env)->GetDoubleArrayElements(env, doubles, 0);
  jdouble d0 = dp[0];
  (*env)->ReleaseDoubleArrayElements(env, doubles, dp, JNI_ABORT); /* nothing changed, nothing written */
  jdouble two = d0 * 8;
  (*env)->SetDoubleArrayRegion(env, doubles, 1, 1, &two);

  jboolean zs[3] = {1, 0, 1};
  jbyte bs[3] = {-1, 2, 127};
  jchar cs[3] = {0x61, 0xe9, 0xd83d};
  jshort ss[3] = {-32768, 0, 32767};
  jlong ls[3] = {-1, (jlong)1 << 62, 5};
  jfloat fs[3] = {0.5f, -1.25f, 3.0f};
  jbooleanArray za = (*env)->NewBooleanArray(env, 3);
  jbyteArray ba = (*env)->NewByteArray(env, 3);
  jcharArray ca = (*env)->NewCharArray(env, 3);
  jshortArray sa = (*env)->NewShortArray(env, 3);
  jlongArray la = (*env)->NewLongArray(env, 3);
  jfloatArray fa = (*env)->NewFloatArray(env, 3);
  (*env)->SetBooleanArrayRegion(env, za, 0, 3, zs);
  (*env)->SetByteArrayRegion(env, ba, 0, 3, bs);
  (*env)->SetCharArrayRegion(env, ca, 0, 3, cs);
  (*env)->SetShortArrayRegion(env, sa, 0, 3, ss);
  (*env)->SetLongArrayRegion(env, la, 0, 3, ls);
  (*env)->SetFloatArrayRegion(env, fa, 0, 3, fs);
  jboolean z2[3];
  jbyte b2[3];
  jchar c2[3];
  jshort s2[3];
  jlong l2[3];
  jfloat f2[3];
  (*env)->GetBooleanArrayRegion(env, za, 0, 3, z2);
  (*env)->GetByteArrayRegion(env, ba, 0, 3, b2);
  (*env)->GetCharArrayRegion(env, ca, 0, 3, c2);
  (*env)->GetShortArrayRegion(env, sa, 0, 3, s2);
  (*env)->GetLongArrayRegion(env, la, 0, 3, l2);
  (*env)->GetFloatArrayRegion(env, fa, 0, 3, f2);
  unsigned h = fnv(z2, sizeof z2) ^ fnv(b2, sizeof b2) * 3 ^ fnv(c2, sizeof c2) * 5 ^ fnv(s2, sizeof s2) * 7 ^
               fnv(l2, sizeof l2) * 11 ^ fnv(f2, sizeof f2) * 13;
  jbyte* bytes = (*env)->GetByteArrayElements(env, ba, 0);
  int bsum = bytes[0] + bytes[1] + bytes[2];
  (*env)->ReleaseByteArrayElements(env, ba, bytes, JNI_ABORT);

  jstring first = (jstring)(*env)->GetObjectArrayElement(env, objects, 0);
  const char* f = (*env)->GetStringUTFChars(env, first, 0);
  char got[16];
  snprintf(got, sizeof got, "%s", f);
  (*env)->ReleaseStringUTFChars(env, first, f);
  (*env)->SetObjectArrayElement(env, objects, 1, (*env)->NewStringUTF(env, "from native"));
  jclass sc = (*env)->FindClass(env, "java/lang/String");
  jobjectArray made = (*env)->NewObjectArray(env, 4, sc, (*env)->NewStringUTF(env, "fill"));
  jobject m3 = (*env)->GetObjectArrayElement(env, made, 3);
  snprintf(out, sizeof out, "lens %d %d %d made %d prims %08x bytesum %d first %s fill %d",
           (int)(*env)->GetArrayLength(env, ints), (int)(*env)->GetArrayLength(env, la),
           (int)(*env)->GetArrayLength(env, objects), (int)(*env)->GetArrayLength(env, made), h, bsum, got,
           (int)(*env)->GetStringUTFLength(env, (jstring)m3));
  return (*env)->NewStringUTF(env, out);
}

/* native -> Java (BridgeActivity.up) -> native ..., `depth` levels. */
EXPORT jint P(callbacks)(JNIEnv* env, jclass cls, jint depth) {
  if (depth == 0) return 1;
  jmethodID up = (*env)->GetStaticMethodID(env, cls, "up", "(I)I");
  return (*env)->CallStaticIntMethod(env, cls, up, depth) + 1000 * depth;
}

/* A Java exception through native code: caught here by message, replaced with one thrown from native code. */
EXPORT jstring P(exceptions)(JNIEnv* env, jclass cls) {
  jmethodID boom = (*env)->GetStaticMethodID(env, cls, "boom", "()V");
  (*env)->CallStaticVoidMethod(env, cls, boom);
  if (!(*env)->ExceptionCheck(env)) return (*env)->NewStringUTF(env, "boom() did not throw");
  jthrowable t = (*env)->ExceptionOccurred(env);
  (*env)->ExceptionClear(env);
  jclass tc = (*env)->GetObjectClass(env, t);
  jmethodID gm = (*env)->GetMethodID(env, tc, "getMessage", "()Ljava/lang/String;");
  jstring msg = (jstring)(*env)->CallObjectMethod(env, t, gm);
  const char* m = (*env)->GetStringUTFChars(env, msg, 0);
  char out[128];
  jclass ise = (*env)->FindClass(env, "java/lang/IllegalStateException");
  snprintf(out, sizeof out, "from native, caught %s (IllegalStateException %d)", m, (int)(*env)->IsInstanceOf(env, t, ise));
  (*env)->ReleaseStringUTFChars(env, msg, m);
  jclass iae = (*env)->FindClass(env, "java/lang/IllegalArgumentException");
  (*env)->ThrowNew(env, iae, out);
  return 0;
}

static jobject g_global, g_weak;

EXPORT void P(makeGlobals)(JNIEnv* env, jclass cls, jobject o) {
  (void)cls;
  g_global = (*env)->NewGlobalRef(env, o);
  g_weak = (*env)->NewWeakGlobalRef(env, o);
}

/* After GCs with only the global holding the referent: both still name it. Then the global goes. */
EXPORT jstring P(checkGlobals)(JNIEnv* env, jclass cls) {
  (void)cls;
  char out[160];
  jobject strong = (*env)->NewLocalRef(env, g_weak);
  jclass sb = (*env)->FindClass(env, "java/lang/StringBuilder");
  jmethodID ts = (*env)->GetMethodID(env, sb, "toString", "()Ljava/lang/String;");
  jstring s = (jstring)(*env)->CallObjectMethod(env, g_global, ts);
  const char* c = (*env)->GetStringUTFChars(env, s, 0);
  snprintf(out, sizeof out, "global %s weak alive %d same %d types %d %d", c, !(*env)->IsSameObject(env, g_weak, 0),
           (int)(*env)->IsSameObject(env, strong, g_global), (int)(*env)->GetObjectRefType(env, g_global),
           (int)(*env)->GetObjectRefType(env, g_weak));
  (*env)->ReleaseStringUTFChars(env, s, c);
  (*env)->DeleteLocalRef(env, strong);
  (*env)->DeleteGlobalRef(env, g_global);
  g_global = 0;
  return (*env)->NewStringUTF(env, out);
}

EXPORT jstring P(weakCleared)(JNIEnv* env, jclass cls) {
  (void)cls;
  if (!(*env)->IsSameObject(env, g_weak, 0)) return (*env)->NewStringUTF(env, "no");
  (*env)->DeleteWeakGlobalRef(env, g_weak);
  return (*env)->NewStringUTF(env, "yes");
}

EXPORT jint P(add)(JNIEnv* env, jclass cls, jint a, jint b) {
  (void)env;
  (void)cls;
  return a + b;
}

/* Instance and static fields of BridgeActivity$Holder: read every kind, write each back changed. */
EXPORT jstring P(fields)(JNIEnv* env, jclass cls, jobject h) {
  (void)cls;
  char out[200];
  jclass hc = (*env)->GetObjectClass(env, h);
  jfieldID fi = (*env)->GetFieldID(env, hc, "i", "I"), fj = (*env)->GetFieldID(env, hc, "j", "J");
  jfieldID fd = (*env)->GetFieldID(env, hc, "d", "D"), fs = (*env)->GetFieldID(env, hc, "s", "Ljava/lang/String;");
  jfieldID sc = (*env)->GetStaticFieldID(env, hc, "counter", "I");
  jfieldID sl = (*env)->GetStaticFieldID(env, hc, "label", "Ljava/lang/String;");
  jint i = (*env)->GetIntField(env, h, fi);
  jlong j = (*env)->GetLongField(env, h, fj);
  jdouble d = (*env)->GetDoubleField(env, h, fd);
  jstring s = (jstring)(*env)->GetObjectField(env, h, fs);
  jint counter = (*env)->GetStaticIntField(env, hc, sc);
  const char* sv = (*env)->GetStringUTFChars(env, s, 0);
  snprintf(out, sizeof out, "read i=%d j=%lld d=%d/8 s=%s counter=%d", (int)i, (long long)j, (int)(d * 8), sv,
           (int)counter);
  (*env)->ReleaseStringUTFChars(env, s, sv);
  (*env)->SetIntField(env, h, fi, i * 6);
  (*env)->SetLongField(env, h, fj, j + 1);
  (*env)->SetDoubleField(env, h, fd, d * 3);
  (*env)->SetObjectField(env, h, fs, (*env)->NewStringUTF(env, "set by native"));
  (*env)->SetStaticIntField(env, hc, sc, counter + 39);
  (*env)->SetStaticObjectField(env, hc, sl, (*env)->NewStringUTF(env, "static set"));
  return (*env)->NewStringUTF(env, out);
}

/* Two natives named `over`: only the long names say which is which. */
EXPORT jstring Java_com_retrobug_jnibridge_BridgeActivity_over__I(JNIEnv* env, jclass cls, jint x) {
  (void)cls;
  char out[32];
  snprintf(out, sizeof out, "int %d", (int)x + 1);
  return (*env)->NewStringUTF(env, out);
}
EXPORT jstring Java_com_retrobug_jnibridge_BridgeActivity_over__Ljava_lang_String_2(JNIEnv* env, jclass cls,
                                                                                      jstring s) {
  (void)cls;
  return (*env)->NewStringUTF(env, (*env)->GetStringUTFLength(env, s) == 1 ? "string of 1" : "string?");
}

/* BridgeActivity$Surface, as libgumball's gumballJNIBegin/gumballJNIEnd do it: `this` saved in a static (and
 * pinned by a global) on every entry, cleared only when the outermost native ends. */
#define S(name) Java_com_retrobug_jnibridge_BridgeActivity_00024Surface_##name
static jobject g_this, g_pin;
static int g_depth;
static char g_loaded[32];

static void surface_begin(JNIEnv* env, jobject self) {
  g_this = self;
  g_pin = (*env)->NewGlobalRef(env, self);
  g_depth++;
}

static void surface_end(JNIEnv* env) {
  if (--g_depth != 0) return;
  (*env)->DeleteGlobalRef(env, g_pin);
  g_this = 0;
}

/* Calls back load(name) (which calls inner), then uses the saved `this` — by now inner's, after inner returned.
 * ART reads it from inner's returned stack frame: right after the callback it still holds the object, but once a
 * deeper call (label()) has run over that stack, what it reads is leftovers (IsSameObject said 0), so the row stops
 * at the uses Gumball makes. */
EXPORT jstring S(outer)(JNIEnv* env, jobject self, jstring name) {
  char out[160];
  surface_begin(env, self);
  jclass c = (*env)->GetObjectClass(env, self);
  jmethodID load = (*env)->GetMethodID(env, c, "load", "(Ljava/lang/String;)V");
  (*env)->CallVoidMethod(env, self, load, name);
  jclass c2 = (*env)->GetObjectClass(env, g_this);
  jmethodID label = (*env)->GetMethodID(env, c2, "label", "()Ljava/lang/String;");
  jstring l = (jstring)(*env)->CallObjectMethod(env, g_this, label);
  const char* lc = (*env)->GetStringUTFChars(env, l, 0);
  snprintf(out, sizeof out, "loaded %s, then %s (depth %d)", g_loaded, lc, g_depth);
  (*env)->ReleaseStringUTFChars(env, l, lc);
  surface_end(env);
  return (*env)->NewStringUTF(env, out);
}

EXPORT void S(inner)(JNIEnv* env, jobject self, jstring name) {
  surface_begin(env, self);
  const char* n = (*env)->GetStringUTFChars(env, name, 0);
  snprintf(g_loaded, sizeof g_loaded, "%s", n);
  (*env)->ReleaseStringUTFChars(env, name, n);
  surface_end(env);
}

static jint native_add(JNIEnv* env, jclass cls, jint a, jint b) {
  (void)env;
  (void)cls;
  return a * 31 + b;
}

EXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)reserved;
  JNIEnv* env = 0;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return -1;
  jclass c = (*env)->FindClass(env, "com/retrobug/jnibridge/BridgeActivity$Registered");
  if (c == 0) return -2;
  JNINativeMethod m = {"nativeAdd", "(II)I", (void*)native_add};
  if ((*env)->RegisterNatives(env, c, &m, 1) != 0) return -3;
  return JNI_VERSION_1_6;
}
