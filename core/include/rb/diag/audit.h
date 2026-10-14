/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * The self-audit's recorder: named invariant checks, run after a launch or a game run and rendered as
 * `audit:` lines on the report. `rb::diag::Recorder` (diag.h) is the other half of the diagnostics
 * story and its question is "was what was requested what is active?"; this class asks "does the state
 * the run left behind still satisfy what the engine claims about it?" — and it inherits the report's
 * own two rules: nothing is skipped without saying so, and a check that cannot fail is not a check.
 * Every FAIL carries the detail that names the disagreement, because the subsystem exists so that a
 * future bug arrives as a sentence instead of a debugging session. What it cannot catch (plausible
 * wrong answers, unexecuted paths, host UB, hangs) is on the record in docs/30 §Step 42 — an audit is
 * not a proof.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rb::diag {

/// Accumulates checks for one run. The launcher owns one per flow, hands it to whatever could check
/// (the memory manager, the JNI arena, the loader's module table, the launcher's own layout constants),
/// then renders it into the report; tests exercise each contributor directly.
class Auditor {
 public:
  /// One named invariant. `ok == false` is a FAIL whose `detail` must say what disagreed — "FAIL" with
  /// nothing after it is the thing this class exists to prevent, so an empty detail is replaced by a
  /// line saying none was recorded, and a nameless check is refused into a FAIL of its own rather than
  /// stored blank (a nameless check is unreviewable, the same rule `Recorder::Record` applies to a
  /// malformed echo).
  void Check(const std::string& name, bool ok, const std::string& detail = {});

  /// A check that could not run this time, with the reason — never silently absent. The reason is the
  /// point: "SKIP tier/agreement: Inspect runs one tier by the side-effect rule" is an intended
  /// omission a reader can review; a missing line would say nothing.
  void Skip(const std::string& name, const std::string& why);

  /// A counted observation that is not pass/fail: modules placed, pages mapped, refusals recorded.
  void Info(const std::string& name, const std::string& text);

  [[nodiscard]] unsigned passes() const { return passes_; }
  [[nodiscard]] unsigned failures() const { return failures_; }
  [[nodiscard]] unsigned skips() const { return skips_; }

  /// Every line as `audit: PASS <name>`, `audit: FAIL <name>: <detail>`, `audit: SKIP <name>: <why>`
  /// and `audit: <name>: <text>` for INFO, then `audit: N PASS, M FAIL, K SKIP` and `audit: ok` /
  /// `audit: NOT OK`. Newline-separated; the launcher's `Say` loop consumes it line by line.
  [[nodiscard]] std::string Render() const;

 private:
  struct Entry {
    enum class Kind : std::uint8_t { kPass, kFail, kSkip, kInfo } kind = Kind::kPass;
    std::string name;
    std::string text;
  };
  std::vector<Entry> entries_;
  unsigned passes_ = 0;
  unsigned failures_ = 0;
  unsigned skips_ = 0;
};

}  // namespace rb::diag
