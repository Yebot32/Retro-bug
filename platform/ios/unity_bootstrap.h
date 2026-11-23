// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "apk_identity.h"
#include "rb/java/table_java.h"

namespace rb::host { class Boot; }

namespace rb::platform::ios {

struct UnityJavaTasks;

// The Java-driven lifecycle used by older Android UnityPlayer builds.
class UnityBootstrap {
 public:
  struct DisplayInfo {
    int width = 0, height = 0;
    int rotation = 0, density_dpi = 160;
    float refresh_rate = 60;
  };
  struct JavaServices {
    std::function<bool(const std::string&)> load_library;
    std::function<void()> request_shutdown;
    std::function<void(const std::string&)> log;
    std::function<DisplayInfo()> display;
    std::string data_dir;
    std::string native_library_dir;
  };
  static std::shared_ptr<UnityJavaTasks> InstallJava(java::TableJava& java, const std::string& apk,
      const ApkIdentity& identity, const JavaServices& services = {});
  static bool RunJavaTasks(java::TableJava& java, const std::shared_ptr<UnityJavaTasks>& tasks,
                           std::string* why);
  UnityBootstrap(host::Boot& boot, java::TableJava& java, std::shared_ptr<UnityJavaTasks> tasks = {});
  ~UnityBootstrap();
  bool Start(const std::string& apk, std::string* why);
  bool Render(std::string* why);
  bool Pause(std::string* why);
  bool Resume(std::string* why);
  void Touch(int kind, int id, float x, float y);

 private:
  bool Has(const char* name, const char* signature) const;
  bool Call(const char* name, const char* signature, const std::vector<std::uint64_t>& args,
            std::string* why, std::uint64_t* result = nullptr);
  bool Attach(std::string* why);
  struct Pointer { int input_id; unsigned android_id; float x, y; };
  host::Boot& boot_;
  java::TableJava& java_;
  std::shared_ptr<UnityJavaTasks> tasks_;
  java::TableJava::Ref receiver_ = 0, activity_ = 0, surface_ = 0;
  java::TableJava::Ref touch_event_ = 0;
  std::vector<Pointer> pointers_;
  std::uint64_t down_time_ = 0;
  bool touch_available_ = false;
};

}  // namespace rb::platform::ios
