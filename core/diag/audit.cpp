/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * See rb/diag/audit.h for what this is and the rules it keeps. Deliberately dependency-free, like the
 * rest of `rb_diag`: it is included by the memory manager, the JNI environment and the launcher, so it
 * must not pull any of them in.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/diag/audit.h"

#include <utility>

namespace rb::diag {

void Auditor::Check(const std::string& name, bool ok, const std::string& detail) {
  Entry e;
  e.kind = ok ? Entry::Kind::kPass : Entry::Kind::kFail;
  e.name = name;
  e.text = ok ? std::string{} : detail;
  if (name.empty()) {
    e.kind = Entry::Kind::kFail;
    e.name = "<unnamed check>";
    e.text = "a check with no name was added to the audit";
  }
  if (e.kind == Entry::Kind::kFail && e.text.empty()) {
    e.text = "failed with no detail recorded";
  }
  if (e.kind == Entry::Kind::kPass) {
    ++passes_;
  } else if (e.kind == Entry::Kind::kFail) {
    ++failures_;
  }
  entries_.push_back(std::move(e));
}

void Auditor::Skip(const std::string& name, const std::string& why) {
  ++skips_;
  entries_.push_back(Entry{Entry::Kind::kSkip, name, why.empty() ? "no reason recorded" : why});
}

void Auditor::Info(const std::string& name, const std::string& text) {
  entries_.push_back(Entry{Entry::Kind::kInfo, name, text});
}

std::string Auditor::Render() const {
  std::string out;
  for (const Entry& e : entries_) {
    switch (e.kind) {
      case Entry::Kind::kPass:
        out += "audit: PASS " + e.name + "\n";
        break;
      case Entry::Kind::kFail:
        out += "audit: FAIL " + e.name + ": " + e.text + "\n";
        break;
      case Entry::Kind::kSkip:
        out += "audit: SKIP " + e.name + ": " + e.text + "\n";
        break;
      case Entry::Kind::kInfo:
        out += "audit: " + e.name + ": " + e.text + "\n";
        break;
    }
  }
  out += "audit: " + std::to_string(passes_) + " PASS, " + std::to_string(failures_) + " FAIL, " +
         std::to_string(skips_) + " SKIP\n";
  out += failures_ == 0 ? "audit: ok\n" : "audit: NOT OK\n";
  return out;
}

}  // namespace rb::diag
