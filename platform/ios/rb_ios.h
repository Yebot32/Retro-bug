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

/// 1 when this process can map executable memory, i.e. a JIT can run: probed once (an R|X mapping of the
/// shape the code cache needs) and cached. 0 is the indicator's red state, and every session then runs on
/// Tier I. The probe is deliberately the same permission the code cache exercises, not a guess from the OS
/// version: a sideloaded build with a JIT entitlement passes it, a stock one does not.
int rb_ios_jit_available(void);

/// Whether the next session may use Tier J when `rb_ios_jit_available` says it can. Default 1: use it when
/// available. The app persists the user's choice; a session never forces JIT — no executable memory means
/// Tier I, and a Tier J boot that fails retries on Tier I with the reason in the log.
void rb_ios_set_jit_enabled(int enabled);

/// 1 when this session actually booted on Tier J, 0 when it is on Tier I (or has not booted yet). The app
/// shows the tier the running game really got, not just what was requested.
int rb_ios_session_jit(RbIosSession* s);

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
