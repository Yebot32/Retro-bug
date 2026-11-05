// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// The iOS app's C surface (S1). Swift owns the UI; this owns one running game: the boot, the EGL layer,
// the render thread and the touch queue. Everything here is called on the main thread except the frame
// loop, which is this file's own thread and the only thread that ever touches GL.
//
// Pointers are opaque. `layer` is a CAEAGLLayer* (bridged by the Swift side); it is retained on the render
// thread once the game starts.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RbIosSession RbIosSession;

/// 1 when this launch can actually run a JIT: a debugger is attached (iOS grants code-page write access to
/// a traced process — how AltStore/SideStore's "Enable JIT" works) and the JIT's code region can be mapped
/// and flipped R|X -> R|W -> R|X. A home-screen launch reads 0 even when the R|X mapping alone succeeds,
/// because the JIT's first store into that page would fault. The session selects its tier automatically:
/// Tier J when this says 1, then the AOT profile, then the interpreter; a Tier J boot that dies leaves a
/// marker so the same build does not retry it.
int rb_ios_jit_available(void);

/// 1 when this session actually booted on Tier J, 0 when it is not on Tier J. The log reports the engine
/// the running game really got.
int rb_ios_session_jit(RbIosSession* s);
/// The engine a session actually booted on: 0 = Tier I, 1 = AOT (offline-translated), 2 = Tier J.
int rb_ios_session_engine(RbIosSession* s);

/// The game's icon, read out of the APK the way the Android launcher asks PackageManager for it: the
/// largest entry that looks like a launcher image (PNG/JPEG/WebP), adaptive-icon layers and nine-patch
/// sheets skipped. Returns a malloc'd copy in *len (0 when there is none) that the caller releases with
/// rb_ios_free_bytes; NULL when the APK cannot be read or carries no such entry.
unsigned char* rb_ios_apk_icon(const char* apk_path, unsigned int* len);
void rb_ios_free_bytes(void* bytes);

/// Creates a session for `lib_name` in `apk_path`. The boot happens on the render thread when rb_ios_start
/// is called (so the UI never blocks on it); failures arrive through rb_ios_last_error and the log file.
/// `data_dir` is the app's writable directory (the game's files/ and cache/ live under it). `log_path`,
/// when not NULL, receives every engine/report line (appended, flushed per line), so a failed boot is
/// diagnosable from the file. `client_version` is the GLES version the game asks for (2 for Cocos2d-x 3.x).
/// NULL with `err` filled only for a bad argument; `err_len` includes the NUL.
RbIosSession* rb_ios_create(const char* apk_path, const char* lib_name, const char* data_dir,
                            const char* log_path, int client_version, char* err, int err_len);

/// Starts the render thread over `layer` (a CAEAGLLayer*) with a drawable of `width`x`height` pixels. On
/// its first run the thread boots the library, then makes the EGL context current, calls the game's
/// nativeInit and draws. 0 with `err` only when the thread could not be started; boot/render failures are
/// reported through rb_ios_last_error/the log.
int rb_ios_start(RbIosSession* s, void* layer, int width, int height, char* err, int err_len);

/// Stops and joins the render thread (the boot and its guest process stay alive; start may be called again).
void rb_ios_stop(RbIosSession* s);

/// Stops, joins and destroys on a detached thread. The caller returns immediately: a guest in the middle
/// of a long nativeRender (a level load) cannot be interrupted, and joining it on the main thread froze
/// the launcher for as long as the load took. The session must not be touched after this call.
void rb_ios_stop_async(RbIosSession* s);

/// The app moved to the background/foreground: the render thread stops drawing and calls the game's
/// nativeOnPause/nativeOnResume on itself (never on the caller's thread).
void rb_ios_pause(RbIosSession* s);
void rb_ios_resume(RbIosSession* s);

/// Stops, then tears everything down. Safe on NULL.
void rb_ios_destroy(RbIosSession* s);

/// Touch, in drawable pixels (origin top-left). Queued for the render thread; id is the finger's id.
void rb_ios_touch_begin(RbIosSession* s, int id, float x, float y);
void rb_ios_touch_move(RbIosSession* s, int id, float x, float y);
void rb_ios_touch_end(RbIosSession* s, int id, float x, float y);
void rb_ios_touch_cancel(RbIosSession* s, int id, float x, float y);

/// Frames rendered since start, and the last error (empty when none). `err_len` includes the NUL.
int rb_ios_frame_count(RbIosSession* s);
void rb_ios_last_error(RbIosSession* s, char* out, int out_len);

/// The most recent log lines, newest last, one per line, for the app's diagnostics view. Returns the
/// number of bytes written (truncated to `out_len` when the tail is longer).
int rb_ios_log_tail(RbIosSession* s, char* out, int out_len);

#ifdef __cplusplus
}
#endif
