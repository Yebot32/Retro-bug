// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// Blocky Roads' Java profile: the `com/utils/*` classes its `libgame.so` names through JNI. The APK's own
// Java is not executed, so each class here is the surface the native side reaches — found by resolving every
// class-name reference in the library's code to the method/signature strings loaded beside it:
//
//   GameControllerInput   the Cocos2d-x controller bridge (CCControllerDispatcher's constructor)
//   FileHelper            saves: read/write/erase under the app's files directory
//   CCMotion              the accelerometer; absent here, so it answers unavailable
//   HttpGet               an asynchronous download with a native callback
//   DeviceAndSystem       OS actions (open URL, vibrate, clipboard) and network presence
//   InappPurchase         Google Play billing; not enabled on this host
//   ShareInterface        share sheet / mail composer
//   GPlayService          Play Games achievements, leaderboards and cloud saves
//   GPLeaderboardEntry    the entry shape the native leaderboard reader walks
//   DropboxInterface      Dropbox sync, inactive without a linked account
//   DropboxFileItem       the entry shape the native Dropbox reader walks
//   UnityAds, Admob       ad providers; nothing cached, nothing shown
//
// A class the native finds but whose method is absent is worse than a no-op: JniHelper caches the null
// method ID and the game keeps calling through it. That is why every method the library names is a row here.
#pragma once

#include "rb/java/android_base.h"
#include "rb/java/table_java.h"

namespace rb::java {

/// Registers Blocky Roads' `com/utils/*` classes on `java`. FileHelper is a real store under
/// `env.files_dir`; every store/provider without a backend answers its "not available" value.
void InstallBlockyroads(TableJava& java, const AndroidEnv& env);

}  // namespace rb::java
