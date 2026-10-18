/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * libmain.so as a Unity NativeActivity build ships it (Derby Quest, FC All-Stars, LEGO Mighty Micros, Torture the
 * Murderer): the entry point the platform dlopen()s and dlsym()s, with JNI_OnLoad beside it and no Java_* at all.
 * ApkScan reads its .dynsym and never runs it; a library built for a Java activity (Angry Birds Epic, MadTruck,
 * Cars 3) exports JNI_OnLoad only, and the host test covers that case with libjnibridge_fixture.so.
 * Built armeabi-v7a Thumb softfp like the other fixtures (tools/run_java_tests.sh).
 */

__attribute__((visibility("default"))) void ANativeActivity_onCreate(void* activity, void* saved_state,
                                                                   unsigned saved_state_size) {
  (void)activity;
  (void)saved_state;
  (void)saved_state_size;
}

__attribute__((visibility("default"))) int JNI_OnLoad(void* vm, void* reserved) {
  (void)vm;
  (void)reserved;
  return 0x00010006; /* JNI_VERSION_1_6 */
}
