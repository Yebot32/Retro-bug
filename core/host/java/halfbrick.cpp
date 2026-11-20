// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
//
// See rb/java/halfbrick.h.

#include "rb/java/halfbrick.h"

namespace rb::java {

void InstallHalfbrick(TableJava& java) {
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
