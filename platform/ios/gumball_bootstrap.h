// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "rb/abi/abi.h"
#include "rb/java/table_java.h"
#include "rb/loader/apk_assets.h"

namespace rb::host { class Boot; }

namespace rb::platform::ios {

class GumballBootstrap {
 public:
  static constexpr char kInitExport[] = "Java_com_twentysixdigital_gumballjava_JNIGLSurface_jniInit";

  GumballBootstrap(host::Boot& boot, java::TableJava& java, std::string lib, std::string data_dir);
  bool Start(const std::string& apk, int width, int height, std::string* error);
  bool Render(std::string* error);
  void Resume();
  void Touch(int kind, int id, float x, float y);

 private:
  struct Texture { std::u16string name; bool raw; };
  bool Call(const char* method, const char* shorty, const abi::Args32& args, std::string* error);
  bool DeliverTexture(const Texture& texture, std::string* error);
  void InstallJava();

  host::Boot& boot_;
  java::TableJava& java_;
  std::string lib_, data_dir_;
  std::unique_ptr<loader::ApkArchive> apk_;
  java::TableJava::Ref surface_ = 0;
  std::deque<Texture> textures_;
  std::chrono::steady_clock::time_point last_frame_;
  bool started_ = false;
  int width_ = 0, finger_ = -1;
};

}  // namespace rb::platform::ios
