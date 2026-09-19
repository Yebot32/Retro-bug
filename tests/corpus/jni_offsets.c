/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Measures the JNI function-table indices out of the platform's own `jni.h`.
 *
 * Why a program has to do this: core/ may not include Android headers (core/CMakeLists.txt states the
 * rule, and it is what keeps the engine buildable for an iOS host), so `rb::abi` cannot say
 * `offsetof(JNINativeInterface, NewStringUTF)` the way C09 will be able to. The numbers still must not
 * be typed in from a specification — the table's order is an ABI, and one entry inserted by a
 * `#if` in a header shifts every index after it, which produces a guest calling the wrong JNI function
 * *successfully*. So this prints them from the same header the fixture was compiled against, and the
 * output is committed as `tests/corpus/jni-offsets.txt`.
 *
 * Two independent measurements are then cross-checked, which is the point:
 *   - here: index = offsetof / sizeof(void*), so the number is pointer-width independent;
 *   - in the fixture's own Thumb-2 code: `ldr.w r2, [r2, #0x29c]` for NewStringUTF, i.e. 0x29c/4 = 167.
 * If those ever disagree, one of them is wrong, and the disagreement is the finding.
 *
 * Built for arm64-v8a and run on the same device as the oracle: any compiler with the NDK's header
 * would do, and the device is what is already attached.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <jni.h>
#include <stddef.h>
#include <stdio.h>

/* In C the name is a struct tag; only the C++ side of jni.h gets the bare typedef. */
#define P(field) printf("%-32s index=%3u offset32=%4u\n", #field,                                  \
                        (unsigned)(offsetof(struct JNINativeInterface, field) / sizeof(void*)),     \
                        (unsigned)(offsetof(struct JNINativeInterface, field) / sizeof(void*) * 4u))

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("# sizeof(void*)=%u; offset32 is what a 32-bit guest's ldr immediate must contain\n",
         (unsigned)sizeof(void*));
  P(GetVersion);
  P(DefineClass);
  P(FindClass);
  P(GetObjectRefType);
  P(NewWeakGlobalRef);
  P(GetStringUTFLength);
  P(NewGlobalRef);
  P(DeleteGlobalRef);
  P(DeleteLocalRef);
  P(IsSameObject);
  P(NewLocalRef);
  P(AllocObject);
  P(NewObjectV);
  P(GetObjectClass);
  P(IsInstanceOf);
  P(GetMethodID);
  P(CallIntMethod);
  P(CallStaticIntMethod);
  P(GetFieldID);
  P(GetIntField);
  P(SetIntField);
  P(GetStringLength);
  P(GetStringChars);
  P(GetStringUTFChars);
  P(ReleaseStringUTFChars);
  P(GetArrayLength);
  P(NewByteArray);
  P(GetByteArrayElements);
  P(ReleaseByteArrayElements);
  P(ExceptionOccurred);
  P(ExceptionDescribe);
  P(ExceptionClear);
  P(FatalError);
  P(NewStringUTF);
  P(GetJavaVM);
  P(GetDirectBufferAddress);
  return 0;
}
