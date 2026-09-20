/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C17 `rb::diag` — the launch recorder, whose entire purpose is that nothing gets applied,
 * substituted, or skipped without saying so in the report.
 *
 * The shape comes from Xenia's Vulkan diagnostics, audited in `docs/research/08` §"prior work": an
 * ordered set of candidate stages, a report rewritten **in full after every append** so that a hang or
 * a crash mid-initialisation still leaves the last stage visible, and `GpuDiagnosticStage()` producing
 * `>>> INITIALIZATION FAILED AT: <stage> <<<`. What that audit cost is on record too (`docs/research/08`
 * §"silent-ignore interfaces"): a wrong TOML section, restricted entitlements that were silently inert,
 * a patch DB whose filename rules nobody could see, a driver fallback nobody was told about, and an
 * evidence file deleted by the very reset that produced it — "every one cost a debug session". The rule
 * those sessions bought (`docs/10` §12.3) is the one this type enforces: **nothing is applied without an
 * "active vs requested" echo**, and a declared thing that did not apply is reported as inert rather than
 * quietly skipped.
 *
 * So the design question was not "how do we log?" but "how do we make the omission impossible?". Three
 * answers, each with a test that would fail if it were removed:
 *
 *   - `Record` cannot discard. A malformed entry (no stage, no requested side, no active side) is stored
 *     as a *failure* naming the field it could not echo, because dropping a bad record is exactly the
 *     silent-ignore behaviour this component exists to prevent, and a caller that ignores the return
 *     value still gets a legible report.
 *   - Required fields are enumerable. `MissingRequired()` lists what a report *ought* to contain — the
 *     M1 exit criterion is "`rb::diag` shows active tier + address model + code-cache backend", which is
 *     a sentence until this makes it an assertion.
 *   - Entering a stage is itself an append. A process that dies in the middle leaves a file naming the
 *     stage it died in, which is the only reason the rewrite-in-full cost is worth paying.
 *
 * `docs/20` §C17 asks that every other component's "must be reviewed" line be a field here; `Field` is
 * that list for the components that exist so far, and the naming table is checked against the enum by a
 * test rather than by care.
 *
 * See docs/20 §C17 and docs/10 §12.3.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rb::diag {

/// What a launch has to report on whatever it ended up doing. Every one of these is a
/// requested-vs-active pair: the thing asked for (a profile, a build-time choice, a platform default)
/// and the thing that is actually in effect, established by a probe or a read rather than by hope.
enum class Field : std::uint8_t {
  /// C12/C02: "JIT" requested, "interpreter" active — or the reverse, which is the case AD-17's
  /// persistent badge exists to make impossible to miss.
  kCodeTier = 0,
  /// AD-6: flat/identity versus biased `GUEST_BASE`, chosen at build time and *confirmed by probe*.
  kAddressModel,
  /// The numeric bias, because "biased" without a number cannot be reviewed. Zero is a value, not an
  /// absence — which is why the field is required rather than omitted on the identity model.
  kGuestBase,
  /// C04: `AnonRwx` / `MemfdDualAlias` / `Interpreted`. "Selection is probed, never assumed, and the
  /// probe result is recorded in the diagnostics report" (`docs/10` §8.3).
  kCodeCacheBackend,
  /// U-13, this launch: can this process execute memory it mapped, in this SELinux domain, on this OEM
  /// build? Re-measured rather than cached, because the cached answer is what makes a fleet claim wrong.
  kExecmemProbe,
  /// U-2, this launch: is a contiguous low window obtainable? On the two devices measured in `30`
  /// §Step 8 the answer is no, and the *reason* (occupancy, not permission) is the useful half.
  kLowWindowProbe,
  /// C03/AD-7: guest 4 KiB against a host that may be 16 KiB.
  kHostPageSize,
  kGuestPageSize,
  /// C16: T0 or T1, and for a T1 entry the fact that a deliberate gesture asked for it. `Enter` failing
  /// visibly and `SelfTest` gating the word "isolated" both land as echoes here.
  kIsolationEntry,
  /// C14/C15: which profile and which overrides are in effect — an override may only ever replace a
  /// *probe result*, so the probe side has to be visible next to it.
  kProfileApplied,
  /// App Revival: which layer answered an intent, and in the order consulted. Tier 3 is only ever
  /// justified by this record existing, so it is the one field with no fallback meaning.
  kRevivalLayer,
  /// A declared patch that did not match: `inert`, with the reason. The `docs/10` §12.3 wording names
  /// this case specifically, and it is the one an "unknown key" warning would still miss.
  kPatchMatch,
  kCount,
};

/// `kActive` is the boring case. `kInert` is the one this component exists for: declared, not applied,
/// and *said out loud* — a patch that did not match, an override a probe outranked, a tier a platform
/// refused.
enum class Outcome : std::uint8_t { kActive, kInert, kFailed };

[[nodiscard]] const char* Name(Field f);
[[nodiscard]] const char* Name(Outcome o);

/// One echo. `evidence` is how the `active` side was established — "probed with MAP_FIXED_NOREPLACE",
/// "read from getconf", "profile key x" — because an active value with no evidence is a claim, and the
/// plan's whole verification discipline is about telling those apart.
struct Entry {
  Field field = Field::kCodeTier;
  std::string stage;
  std::string requested;
  std::string active;
  Outcome outcome = Outcome::kActive;
  std::string evidence;
};

/// Longest any single string in a record occupies in the report. Values are *not* dropped at this limit,
/// only truncated with the real length in the marker: a 4 KiB profile key that matters is still
/// identifiable, and a runaway string cannot make a copyable-from-the-UI report unreadable.
constexpr std::size_t kMaxFieldBytes = 512;

/// Writes the whole rendered report, in one call, replacing whatever was there. Returning false is
/// reported (see `Recorder::sink_error()`) rather than retried or swallowed.
using Sink = std::function<bool(const std::string&)>;

class Recorder {
 public:
  Recorder() = default;

  void set_sink(Sink sink) { sink_ = std::move(sink); }

  /// Stamps the stage in progress and rewrites the report. Called before the work, not after: a launch
  /// that dies here leaves a file that says so.
  void EnterStage(const std::string& stage);

  /// Echoes one field. Always stores something — see the header on why a refused record would be the
  /// bug rather than the feature. The bool says whether what it stored was the entry as given.
  bool Record(const Entry& entry);
  bool Record(Field f, std::string stage, std::string requested, std::string active,
              Outcome outcome = Outcome::kActive, std::string evidence = {});

  /// A failure at `stage`, which is what the `>>> INITIALIZATION FAILED AT: ... <<<` line names. The
  /// *first* failure is the banner: what went wrong first is what a reader needs, and a later, vaguer
  /// failure must not overwrite it.
  void Fail(Field f, const std::string& stage, const std::string& detail);

  /// A field the launch must end up reporting even though no build-time rule demanded it — a profile
  /// that declares patches makes `kPatchMatch` required, for instance.
  void Require(Field f);

  [[nodiscard]] bool failed() const { return !banner_.empty(); }
  [[nodiscard]] const std::string& failed_at() const { return banner_; }
  [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }
  [[nodiscard]] const std::string& in_stage() const { return stage_; }
  /// Fields required (statically or by `Require`) and never echoed. Empty is the goal, and M1's exit
  /// criterion is expressible as exactly that. An entry recorded with `Outcome::kFailed` — including a
  /// refusal `Record` produced on the caller's behalf — does **not** satisfy a requirement: the field is
  /// still listed until something on both sides of the echo is actually stated.
  [[nodiscard]] std::vector<std::string> MissingRequired() const;

  /// Anything that went wrong writing the report, which has nowhere else to live: the failure of the
  /// mechanism for reporting failures still has to be reported.
  [[nodiscard]] const std::string& sink_error() const { return sink_error_; }

  [[nodiscard]] std::string Render() const;

  /// Rewrites the report to the sink. Called after every append; explicit calls are harmless and
  /// idempotent, which is what makes the determinism test meaningful.
  bool Flush();

  /// The file form of the same thing, for a caller with no sink: `path`, truncated, whole report.
  bool WriteFile(const std::string& path, std::string* err) const;

 private:
  std::vector<Entry> entries_;
  std::string stage_;
  std::string banner_;
  std::uint32_t required_ = 0;  ///< bit set of Field, so `Require` is free
  std::string sink_error_;
  unsigned sink_failures_ = 0;
  Sink sink_;
};

}  // namespace rb::diag
