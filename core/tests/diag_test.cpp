/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C17 `rb::diag` tests. The subject matter is a rule rather than an algorithm, so most of these cases
 * are of one shape: do the thing that would make a launch's report lie, then read the report and check
 * that it says so. That is also why there is no fixture `.so` and no device dependency here — if the
 * recorder's guarantees only held on hardware, they would not be guarantees about the recorder.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/diag/diag.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace rb::diag {
namespace {

int failures = 0;
int checks = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK_CTX(cond, detail)                     \
  do {                                              \
    ++checks;                                       \
    if (!(cond)) fail(#cond, __LINE__, detail);      \
  } while (0)
#define CHECK(cond) CHECK_CTX(cond, {})

bool Has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

/// The `unrecorded = ...` line on its own, because asserting on the whole report with `Has` cannot tell
/// a gap from a record — a field that *was* echoed still appears, as a record. That distinction is what
/// this list is about, so the test reads the line rather than the file.
std::string UnrecordedLine(const Recorder& r) {
  const std::string text = r.Render();
  const std::size_t at = text.find("unrecorded = ");
  if (at == std::string::npos) return {};
  const std::size_t end = text.find('\n', at);
  return text.substr(at, end - at);
}

/// Counts `line` occurrences so a report can be checked for structure rather than for substring soup.
int CountLines(const std::string& text, const std::string& prefix) {
  int n = 0;
  for (std::size_t at = text.find(prefix); at != std::string::npos; at = text.find(prefix, at + 1)) {
    if (at == 0 || text[at - 1] == '\n') ++n;
  }
  return n;
}

/// Every field recorded well, which is the "green launch" the other cases subtract from.
Recorder Complete() {
  Recorder r;
  r.EnterStage("engine-start");
  r.Record(Field::kCodeTier, "engine-start", "JIT (Tier J)", "JIT (Tier J)", Outcome::kActive,
           "probed: mmap RWX executed a 4-byte stub");
  r.Record(Field::kAddressModel, "engine-start", "flat/identity low 4 GiB", "biased GUEST_BASE",
           Outcome::kActive, "probed: MAP_FIXED_NOREPLACE 4 GiB at 0x10000 -> EEXIST");
  r.Record(Field::kGuestBase, "engine-start", "0x0", "0x7f00000000", Outcome::kActive,
           "chosen by the reservation that succeeded");
  r.Record(Field::kCodeCacheBackend, "engine-start", "AnonRwx", "AnonRwx", Outcome::kActive,
           "probed: execmem granted in u:r:untrusted_app");
  r.Record(Field::kHostPageSize, "engine-start", "4 KiB", "4 KiB", Outcome::kActive, "getconf");
  r.Record(Field::kGuestPageSize, "engine-start", "4 KiB", "4 KiB", Outcome::kActive,
           "RB_GUEST_PAGE_SHIFT=12 at build time");
  r.Record(Field::kIsolationEntry, "launch", "T0 in-process", "T0 in-process", Outcome::kActive,
           "single-tap path; no isolated request was made");
  return r;
}

// ------------------------------------------------------- the echo itself

/// An empty report is still a report, and it says what is missing rather than looking like nothing
/// happened.
void empty_report_names_what_is_missing() {
  Recorder r;
  const std::string text = r.Render();
  CHECK_CTX(Has(text, "retrobug-diag/1"), text);
  CHECK_CTX(Has(text, "in-stage = (none)"), text);
  CHECK_CTX(Has(text, "failed-at = (none)"), text);
  CHECK_CTX(Has(text, "unrecorded = code-tier, address-model, guest-base, code-cache-backend, "
                     "host-page-size, guest-page-size, isolation-entry"),
            text);
  CHECK_CTX(Has(text, "records = 0"), text);
  CHECK_CTX(r.MissingRequired().size() == 7u, std::to_string(r.MissingRequired().size()));
}

void a_recorded_field_leaves_the_missing_list() {
  Recorder r;
  r.Record(Field::kGuestPageSize, "engine-start", "4 KiB", "4 KiB");
  CHECK(!Has(UnrecordedLine(r), "guest-page-size"));
  CHECK_CTX(Has(UnrecordedLine(r), "unrecorded = code-tier"), UnrecordedLine(r));
  CHECK_CTX(Has(r.Render(), "field = guest-page-size"), "the record itself vanished");
}

/// The property the whole component is standing on: one echo per field, both sides, no exceptions.
void both_sides_are_always_present() {
  const Recorder r = Complete();
  const std::string text = r.Render();
  CHECK_CTX(CountLines(text, "requested = ") == 7, "requested lines: " + std::to_string(CountLines(text, "requested = ")));
  CHECK_CTX(CountLines(text, "active = ") == 7, "active lines: " + std::to_string(CountLines(text, "active = ")));
  CHECK_CTX(CountLines(text, "evidence = ") == 7, text);
  CHECK_CTX(Has(text, "unrecorded = (none)"), text);
  // "JIT requested, interpreter active" is the AD-17 badge case; a report that could say only one of
  // the two would let the substitution this exists to catch slip through.
  CHECK_CTX(Has(text, "requested = JIT (Tier J)"), text);
  CHECK_CTX(Has(text, "active = JIT (Tier J)"), text);
}

/// A half-echo is refused — and refusing means *becoming a visible failure*, not being dropped. This is
/// the case that would be an `if (!valid) return false;` if the rule were decoration.
void a_half_echo_cannot_be_dropped() {
  Recorder r;
  CHECK(!r.Record(Field::kCodeTier, "engine-start", "JIT (Tier J)", ""));
  const std::string text = r.Render();
  CHECK_CTX(Has(text, "records = 1"), text);
  CHECK_CTX(Has(text, "outcome = failed"), text);
  CHECK_CTX(Has(text, "field = code-tier"), text);
  CHECK_CTX(Has(text, "refused: active is empty"), text);
  CHECK_CTX(Has(text, ">>> INITIALIZATION FAILED AT: engine-start <<<"), text);
  // And it still counts as un-echoed for the required-field check: a refusal is not an answer.
  CHECK_CTX(Has(text, "unrecorded = code-tier"), text);
}

void inert_requires_a_reason() {
  Recorder r;
  // Declared, did not apply, no reason given: exactly the shape of the patch-DB bug in the audit this
  // pattern came from, where the file's own rules silently selected nothing. Note the `active` side is
  // stated ("not applied") and the *evidence* is what's missing — an inert outcome is still an echo, and
  // the refusal has to be about the reason rather than about the missing value.
  CHECK(!r.Record(Field::kPatchMatch, "revival", "patch libGL wrapper", "not applied",
                  Outcome::kInert, ""));
  CHECK_CTX(Has(r.Render(), "outcome = failed"), r.Render());
  CHECK_CTX(Has(r.Render(), "refused: evidence is empty"), r.Render());

  Recorder ok;
  CHECK(ok.Record(Field::kPatchMatch, "revival", "patch libGL wrapper", "not applied",
                  Outcome::kInert, "declared target symbol absent in this APK's libGLESv2.so"));
  CHECK_CTX(Has(ok.Render(), "outcome = inert"), ok.Render());
  CHECK(!ok.failed());
}

// ------------------------------------------------------- failure and stage bookkeeping

/// The first failure is the one a reader needs, so a later, vaguer one must not overwrite it — the
/// same rule `JitExecutor::SetPending` arrived at for the same reason.
void first_failure_wins_the_banner() {
  Recorder r;
  r.Fail(Field::kAddressModel, "memory-window", "no contiguous 4 GiB reservation below 4 GiB");
  r.Fail(Field::kCodeCacheBackend, "code-cache", "memfd create failed");
  const std::string text = r.Render();
  CHECK_CTX(Has(text, ">>> INITIALIZATION FAILED AT: memory-window <<<"), text);
  CHECK(!Has(text, "FAILED AT: code-cache"));
  ++checks;
  if (r.failed_at() != "memory-window") fail("failed_at()", __LINE__, r.failed_at());
  CHECK_CTX(CountLines(text, "outcome = failed") == 2, text);
}

/// Rewritten in full *before* the work, so a launch that dies mid-stage leaves a file naming it. This
/// is the only reason the rewrite cost is worth paying, and it is invisible until a hang happens —
/// which is when a missing case here would be discovered.
void entering_a_stage_is_itself_an_append() {
  Recorder r;
  int flushes = 0;
  std::string last;
  r.set_sink([&](const std::string& text) {
    ++flushes;
    last = text;
    return true;
  });
  r.EnterStage("loader");
  CHECK_CTX(Has(last, "in-stage = loader"), last);
  CHECK_CTX(Has(last, "records = 0"), last);
  r.Record(Field::kGuestBase, "loader", "0x0", "0x0");
  CHECK_CTX(flushes == 2, std::to_string(flushes));
  CHECK_CTX(Has(last, "in-stage = loader"), last);
  CHECK_CTX(Has(last, "records = 1"), last);
  // An empty stage name is not a stage: entering it must un-stamp nothing, or the report would name
  // "(none)" at the exact moment a reader needs to know where the launch was.
  r.EnterStage("");
  CHECK_CTX(flushes == 2, "an empty EnterStage wrote the report: " + std::to_string(flushes));
  CHECK_CTX(r.in_stage() == "loader", r.in_stage());
}

void the_report_is_rewritten_after_every_append() {
  Recorder r;
  int flushes = 0;
  r.set_sink([&](const std::string&) {
    ++flushes;
    return true;
  });
  for (int i = 0; i < 5; ++i) r.Record(Field::kHostPageSize, "s", "4 KiB", "4 KiB");
  CHECK_CTX(flushes == 5, std::to_string(flushes));
}

/// Where the mechanism for reporting failures itself fails, that has to be reported too. It cannot go
/// in the payload that just failed to be written, so it appears one flush later — which is what this
/// pins down rather than papers over.
void a_failing_sink_reports_itself() {
  Recorder r;
  int calls = 0;
  r.set_sink([&](const std::string&) {
    ++calls;
    return calls >= 2;  // the first write fails, the rest work
  });
  r.EnterStage("engine-start");
  CHECK_CTX(calls == 1, std::to_string(calls));
  r.Record(Field::kGuestBase, "engine-start", "0x0", "0x0");
  CHECK_CTX(Has(r.Render(), "written = FAILED 1 times"), r.Render());
  CHECK_CTX(Has(r.sink_error(), "the sink returned false"), r.sink_error());
  CHECK(!r.failed());  // a broken writer is not a failed launch, and must not look like one
  r.Record(Field::kCodeTier, "engine-start", "JIT", "JIT");
  CHECK_CTX(r.entries().size() == 2u, std::to_string(r.entries().size()));
}

// ------------------------------------------------------- format and determinism

void values_are_truncated_not_lost() {
  Recorder r;
  const std::string huge(4000, 'x');
  r.Record(Field::kProfileApplied, "profile", "app.toml", huge);
  const std::string text = r.Render();
  CHECK_CTX(Has(text, "...[truncated, 4000 bytes]"), "no truncation marker in a 4000-byte value");
  CHECK(!Has(text, std::string(4000, 'x')));
  CHECK_CTX(text.size() < 5000u, std::to_string(text.size()));
  // The stored entry keeps the real value; only the rendering is bounded.
  CHECK_CTX(r.entries().back().active.size() == 4000u,
            std::to_string(r.entries().back().active.size()));
}

/// Two launches that did the same thing render the same bytes, so the UI can diff them and so a test can
/// assert on structure. The recorder therefore has no clock and no id: a timestamp in here would make
/// every later assertion about "the same report" meaningless and would buy nothing the caller cannot
/// stamp itself.
void identical_launches_render_identically() {
  const std::string a = Complete().Render();
  const std::string b = Complete().Render();
  CHECK_CTX(a == b, "two complete launches rendered differently");
  Recorder r = Complete();
  const std::string before = r.Render();
  CHECK_CTX(r.Flush() && before == r.Render(), "a second flush changed the report");
  CHECK_CTX(Has(a, "records = 7"), a);
}

void every_field_has_a_unique_name() {
  std::vector<std::string> names;
  for (std::size_t i = 0; i < static_cast<std::size_t>(Field::kCount); ++i) {
    const std::string n = Name(static_cast<Field>(i));
    CHECK_CTX(!n.empty(), "field " + std::to_string(i) + " has no name");
    CHECK_CTX(n != "unknown-field", "field " + std::to_string(i) + " is missing from the name table");
    CHECK(std::find(names.begin(), names.end(), n) == names.end());
    names.push_back(n);
  }
  // Every enumerator, and no duplicates: a field added to the enum without a name would otherwise render
  // as `unknown-field` and be invisible in the one artifact whose job is to be read.
  CHECK_CTX(names.size() == static_cast<std::size_t>(Field::kCount), std::to_string(names.size()));
  CHECK_CTX(std::string{Name(Outcome::kInert)} == "inert", Name(Outcome::kInert));
}

/// A field no build-time rule demands becomes demanded the moment something declares it, which is how
/// "a profile that declares patches must report whether they matched" is enforced without the core
/// knowing what a profile is.
void required_fields_can_be_demanded_at_runtime() {
  Recorder r;
  CHECK_CTX(UnrecordedLine(r) != r.Render(), "no unrecorded line at all");
  CHECK(!Has(UnrecordedLine(r), "patch-match"));
  r.Require(Field::kPatchMatch);
  r.Require(Field::kExecmemProbe);
  // In enum order, which is what makes the line comparable between launches instead of merely non-empty.
  CHECK_CTX(Has(UnrecordedLine(r), "code-cache-backend, execmem-probe, host-page-size"),
            UnrecordedLine(r));
  CHECK_CTX(Has(UnrecordedLine(r), "isolation-entry, patch-match"), UnrecordedLine(r));
  r.Record(Field::kPatchMatch, "revival", "two declared patches", "both applied", Outcome::kActive,
           "matched 2/2 target symbols");
  CHECK(!Has(UnrecordedLine(r), "patch-match"));
  CHECK_CTX(Has(r.Render(), "field = patch-match"), "the record itself vanished");
  CHECK_CTX(r.MissingRequired().size() == 8u, std::to_string(r.MissingRequired().size()));
}

void write_file_round_trips_on_the_device() {
  Recorder r = Complete();
  r.Require(Field::kPatchMatch);
  r.Fail(Field::kRevivalLayer, "revival", "no layer answered the intent");
  const std::string path = "/data/local/tmp/rb_diag_report.txt";
  std::string err;
  ++checks;
  if (!r.WriteFile(path, &err)) fail("WriteFile", __LINE__, err);
  std::ifstream in(path, std::ios::binary);
  const std::string read_back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK_CTX(read_back == r.Render(), "the file is not the report");
  CHECK_CTX(Has(read_back, ">>> INITIALIZATION FAILED AT: revival <<<"), read_back);
  // `patch-match` is the only unrecorded requirement: nothing demanded `revival-layer`, so the failed
  // attempt shows as a record rather than as a gap. That distinction is the point — `Require()` is how a
  // caller turns a question into one that a launch has to answer, and `a_half_echo_cannot_be_dropped`
  // covers the case where a demanded field's echo *is* the failure.
  CHECK_CTX(Has(read_back, "unrecorded = patch-match"), read_back);
  std::remove(path.c_str());
}

/// The failure path for a write has to be an error and not a shorter file: a launch that could not
/// record anything must be able to say so.
void write_file_fails_visibly() {
  Recorder r;
  std::string err;
  CHECK(!r.WriteFile("/nonexistent-dir-rb/diag.txt", &err));
  CHECK_CTX(!err.empty(), "a refused write reported no reason");
  CHECK(!r.WriteFile("/", &err));
  CHECK_CTX(!err.empty(), err);
}

}  // namespace
}  // namespace rb::diag

int main() {
  using namespace rb::diag;
  namespace t = rb::diag;
  t::empty_report_names_what_is_missing();
  t::a_recorded_field_leaves_the_missing_list();
  t::both_sides_are_always_present();
  t::a_half_echo_cannot_be_dropped();
  t::inert_requires_a_reason();
  t::first_failure_wins_the_banner();
  t::entering_a_stage_is_itself_an_append();
  t::the_report_is_rewritten_after_every_append();
  t::a_failing_sink_reports_itself();
  t::values_are_truncated_not_lost();
  t::identical_launches_render_identically();
  t::every_field_has_a_unique_name();
  t::required_fields_can_be_demanded_at_runtime();
  t::write_file_round_trips_on_the_device();
  t::write_file_fails_visibly();

  std::printf("%d checks, %d failures\n", rb::diag::checks, rb::diag::failures);
  return rb::diag::failures == 0 ? 0 : 1;
}
