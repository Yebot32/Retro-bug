// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "apk_identity.h"
#include "rb/java/table_java.h"

namespace rb::host { class Boot; }

namespace rb::platform::ios {

// The Java-driven lifecycle used by older Android UnityPlayer builds.
class UnityBootstrap {
 public:
  struct JavaServices {
    std::function<bool(const std::string&)> load_library;
    std::function<void()> request_shutdown;
    std::function<void(const std::string&)> log;
  };
  static void InstallJava(java::TableJava& java, const std::string& apk, const ApkIdentity& identity,
                          const JavaServices& services = {});
  UnityBootstrap(host::Boot& boot, java::TableJava& java);
  ~UnityBootstrap();
  bool Start(const std::string& apk, std::string* why);
  bool Render(std::string* why);
  bool Pause(std::string* why);
  bool Resume(std::string* why);

 private:
  bool Has(const char* name, const char* signature) const;
  bool Call(const char* name, const char* signature, const std::vector<std::uint64_t>& args,
            std::string* why, std::uint64_t* result = nullptr);
  bool Attach(std::string* why);
  host::Boot& boot_;
  java::TableJava& java_;
  java::TableJava::Ref receiver_ = 0, activity_ = 0, surface_ = 0;
};

}  // namespace rb::platform::ios
