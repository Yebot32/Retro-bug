/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 */
#include "rb/diag/diag.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace rb::diag {
namespace {

constexpr std::size_t kFieldCount = static_cast<std::size_t>(Field::kCount);
static_assert(kFieldCount <= 32,
              "Recorder::required_ is a 32-bit mask of Field; a 33rd field needs a wider mask, and "
              "silently losing requirements from a bitmask overflow is the exact class of bug this "
              "component exists to make impossible");

/// The fields every launch must answer for, on every platform. Deliberately excludes the two probes
/// (`kExecmemProbe`, `kLowWindowProbe`): those are Android-shaped questions, and `core/` may not encode
/// a platform's answer set as a universal requirement — the Android glue calls `Require()` for them,
/// which is also what makes "this build forgot to probe" a report line rather than a silent absence.
constexpr std::array<Field, 7> kStaticRequired = {
    Field::kCodeTier, Field::kAddressModel, Field::kGuestBase, Field::kCodeCacheBackend,
    Field::kHostPageSize, Field::kGuestPageSize, Field::kIsolationEntry,
};

}  // namespace

const char* Name(Field f) {
  switch (f) {
    case Field::kCodeTier: return "code-tier";
    case Field::kAddressModel: return "address-model";
    case Field::kGuestBase: return "guest-base";
    case Field::kCodeCacheBackend: return "code-cache-backend";
    case Field::kExecmemProbe: return "execmem-probe";
    case Field::kLowWindowProbe: return "low-window-probe";
    case Field::kHostPageSize: return "host-page-size";
    case Field::kGuestPageSize: return "guest-page-size";
    case Field::kIsolationEntry: return "isolation-entry";
    case Field::kProfileApplied: return "profile-applied";
    case Field::kRevivalLayer: return "revival-layer";
    case Field::kPatchMatch: return "patch-match";
    case Field::kCount: break;
  }
  // Unreachable for a caller holding a valid Field, and worth a line anyway: the test that walks every
  // enumerator is the thing that keeps this table honest when a field is added.
  return "unknown-field";
}

const char* Name(Outcome o) {
  switch (o) {
    case Outcome::kActive: return "active";
    case Outcome::kInert: return "inert";
    case Outcome::kFailed: return "failed";
  }
  return "unknown-outcome";
}

std::string Recorder::Render() const {
  // Bounded here rather than at `Record`: the report is the artifact that has to stay readable when it is
  // copied out of a UI, while `entries()` is the programmatic view, and a caller inspecting it should see
  // what was actually said rather than a truncation of it.
  const auto cap = [](const std::string& s) {
    if (s.size() <= kMaxFieldBytes) return s;
    std::string out = s.substr(0, kMaxFieldBytes);
    out += "...[truncated, " + std::to_string(s.size()) + " bytes]";
    return out;
  };
  std::string out = "retrobug-diag/1\n";
  out += "in-stage = " + (stage_.empty() ? std::string{"(none)"} : stage_) + "\n";
  if (!banner_.empty()) {
    out += ">>> INITIALIZATION FAILED AT: " + banner_ + " <<<\n";
  }
  out += "failed-at = " + (banner_.empty() ? std::string{"(none)"} : banner_) + "\n";
  out += sink_failures_ == 0
             ? std::string{"written = ok\n"}
             : "written = FAILED " + std::to_string(sink_failures_) + " times: " + sink_error_ + "\n";
  const std::vector<std::string> missing = MissingRequired();
  out += "unrecorded = ";
  if (missing.empty()) {
    out += "(none)\n";
  } else {
    for (std::size_t i = 0; i < missing.size(); ++i) {
      if (i) out += ", ";
      out += missing[i];
    }
    out += "\n";
  }
  out += "records = " + std::to_string(entries_.size()) + "\n";
  for (const auto& e : entries_) {
    out += "\nfield = " + std::string{Name(e.field)} + "\n";
    out += "stage = " + cap(e.stage) + "\n";
    out += "requested = " + cap(e.requested) + "\n";
    out += "active = " + cap(e.active) + "\n";
    out += std::string{"outcome = "} + Name(e.outcome) + "\n";
    out += "evidence = " + cap(e.evidence) + "\n";
  }
  return out;
}

std::vector<std::string> Recorder::MissingRequired() const {
  std::uint32_t want = required_;
  for (const Field f : kStaticRequired) want |= 1u << static_cast<std::size_t>(f);
  std::vector<std::string> out;
  for (std::size_t i = 0; i < kFieldCount; ++i) {
    if ((want & (1u << i)) == 0) continue;
    const bool seen = std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) {
      // A failed echo is not an echo. `Record` stores a refusal under the field it refused — so without
      // this test a required field could be "satisfied" by its own complaint, which is the exact way a
      // rule like this one goes decorative.
      return static_cast<std::size_t>(e.field) == i && e.outcome != Outcome::kFailed;
    });
    if (!seen) out.push_back(Name(static_cast<Field>(i)));
  }
  return out;
}

void Recorder::EnterStage(const std::string& stage) {
  if (stage.empty()) return;  // nothing to stamp; un-stamping the real one would be its own lie
  stage_ = stage;
  Flush();  // before the work, so a launch that dies in it leaves the name behind
}

bool Recorder::Record(const Entry& entry) {
  Entry e = entry;
  std::string why;
  if (e.stage.empty()) why = "stage";
  else if (e.requested.empty()) why = "requested";
  else if (e.active.empty()) why = "active";
  else if (e.outcome != Outcome::kActive && e.evidence.empty()) why = "evidence";

  if (!why.empty()) {
    // Refusing loudly instead of discarding quietly. The record keeps its field and stage so the report
    // names *which* echo went missing — that is the whole content of the rule this component enforces,
    // and an `if (!valid) return false;` here would make it a comment instead.
    e.stage = e.stage.empty() ? std::string{"(unset)"} : e.stage;
    e.requested = e.requested.empty() ? std::string{"(not stated)"} : e.requested;
    e.active = e.active.empty() ? std::string{"(not stated)"} : e.active;
    e.outcome = Outcome::kFailed;
    e.evidence = "refused: " + why +
                 " is empty, so this field's requested-vs-active echo cannot be reviewed";
    if (banner_.empty()) banner_ = e.stage;
  }

  entries_.push_back(std::move(e));
  Flush();
  return why.empty();
}

bool Recorder::Record(Field f, std::string stage, std::string requested, std::string active,
                      Outcome outcome, std::string evidence) {
  return Record(Entry{f, std::move(stage), std::move(requested), std::move(active), outcome,
                      std::move(evidence)});
}

void Recorder::Fail(Field f, const std::string& stage, const std::string& detail) {
  Entry e{};
  e.field = f;
  e.stage = stage;
  e.requested = "(attempted)";
  e.active = "(failed)";
  e.outcome = Outcome::kFailed;
  e.evidence = detail;
  // The banner is what `>>> INITIALIZATION FAILED AT: <stage> <<<` is for, and a `Fail` whose payload
  // happens to be well-formed would otherwise record a failure nobody sees at the top of the report.
  // First one wins, for the reason in the header: what went wrong first is what the reader needs.
  if (banner_.empty() && !stage.empty()) banner_ = stage;
  Record(e);
}

void Recorder::Require(Field f) { required_ |= 1u << static_cast<std::size_t>(f); }

bool Recorder::Flush() {
  if (!sink_) return true;
  const std::string text = Render();
  if (sink_(text)) return true;
  ++sink_failures_;
  if (sink_error_.empty()) {
    sink_error_ = "the sink returned false; the in-memory report is intact and the next flush will show "
                  "this line";
  }
  return false;
}

bool Recorder::WriteFile(const std::string& path, std::string* err) const {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (err) *err = "cannot open " + path + ": " + std::strerror(errno);
    return false;
  }
  out << Render();
  out.close();
  if (!out) {
    if (err) *err = "writing " + path + ": " + std::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace rb::diag
