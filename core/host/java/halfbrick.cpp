// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See rb/java/halfbrick.h.

#include "rb/java/halfbrick.h"

namespace rb::java {

void InstallHalfbrick(TableJava& java, const std::string& model, const std::string& manufacturer,
                      const std::string& package, const std::u16string& version, int sdk) {
  java.DefineClass("com/halfbrick/mortar/HBSupport", "java/lang/Object");
  const auto add_string = [&](const char* name, std::u16string value) {
    java.DefineMethod("com/halfbrick/mortar/HBSupport", name, "()Ljava/lang/String;", true,
                      [value = std::move(value)](TableJava& f, int, const auto&) { return f.NewJavaString(value); });
  };
  const auto text = [](const std::string& value) { return std::u16string(value.begin(), value.end()); };
  // Mortar uses empty strings when Android has no identifier or account to return.
  for (const char* name : {"GetDeviceID", "GetAndroidID", "GetGoogleAccount"}) add_string(name, u"");
  add_string("GetAndroidVersion", text(std::to_string(sdk)));
  add_string("GetManufacturer", text(manufacturer));
  add_string("GetModel", text(model));
  add_string("GetPackageName", text(package));
  add_string("GetPackageVersion", version);
  java.DefineClass("com/halfbrick/mortar/SoftKeyboard", "java/lang/Object");
  java.DefineMethod("com/halfbrick/mortar/SoftKeyboard", "Init", "()V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/halfbrick/mortar/SoftKeyboard", "HideKeyboard", "()V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/halfbrick/mortar/SoftKeyboard", "ShowKeyboard", "(I)V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/halfbrick/mortar/SoftKeyboard", "UpdateKeyboard", "()Z", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
  java.DefineMethod("com/halfbrick/mortar/SoftKeyboard", "Prepare",
                    "(Lcom/halfbrick/mortar/MortarGameView;)V", true,
                    [](TableJava&, int, const std::vector<std::uint64_t>&) { return std::uint64_t{0}; });
}

}  // namespace rb::java
