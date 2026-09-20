/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C14's tests. Most of this file is not checking behaviour but checking *impossibility*: the three
 * mistakes the component exists to prevent are mistakes a program must not be able to express, so the
 * assertions are `static_assert`s and `requires`-expressions rather than runtime cases. A runtime test
 * can only show that a call was refused; these show that the call cannot be written, which is the
 * difference between a rule and a convention (docs/20 §C14: "asserted by a schema test, not by
 * convention").
 *
 * The rest is the resolution order, the file subset, and the echo — where the interesting failures are
 * silent ones, so each case asserts the *message* as well as the bool. A refusal nobody can act on is
 * the same hole as one nobody reported.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/profile/profile.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace rb;
namespace pr = rb::profile;

int failures = 0;
int checks = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK_CTX(cond, detail)                 \
  do {                                          \
    ++checks;                                   \
    if (!(cond)) fail(#cond, __LINE__, detail); \
  } while (0)

std::string All(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += "\n  " + x;
  return s;
}

/// The rule `Store::Get`/`Store::Set` are built to enforce, stated as a compile-time predicate. If
/// anyone adds an overload taking a bare app id — the "fall back to global" hole — these flip to false
/// and this file stops compiling, which is the intended failure mode.
template <class... A>
concept CanLookup = requires(const pr::Store& s, A... a) {
  s.Get(a...);
};
template <class... A>
concept CanSet = requires(pr::Store& s, A... a) {
  s.Set(a...);
};

static_assert(CanLookup<pr::NormalKey, pr::Name>, "a NormalKey lookup must exist");
static_assert(CanLookup<pr::IsolatedKey, pr::Name>, "an IsolatedKey lookup must exist");
static_assert(!CanLookup<std::string, pr::Name>,
              "Get(app_id, name) must be unwritable: an un-namespaced key is the ambient-fallback hole");
static_assert(!CanLookup<const char*, pr::Name>, "same, for the string-literal spelling");
static_assert(!CanLookup<pr::AppId, pr::Name>, "and for the alias an app id actually has");
static_assert(!CanSet<std::string, pr::Name, pr::Value, std::nullptr_t>,
              "Set must be as closed as Get; a writable un-namespaced key is worse");

/// The same rule for the row-string half of the implementation, which is private: a caller cannot type a
/// row and address it, so a `Get` that is closed cannot be routed around by an `Effective(row, name)`
/// that is open. That one cannot be asserted from out here — a `requires` expression that reaches a
/// private member is a hard error rather than a substitution failure, which the first build of this file
/// demonstrated by failing. Recorded as a review point instead: if `Effective`, `SetRow`, `LoadTomlRow`,
/// `SaveTomlRow`, `OverridesOfNothingRow` or `EchoRow` ever move to `public:`, this store has an
/// un-namespaced door again.

/// A key is not a struct someone can brace-fill. Each of these has to be constructible only from the
/// capability token that holds it, which is what makes "an isolated instance built a normal key" a
/// compile error rather than a code-review item.
static_assert(!std::is_constructible_v<pr::NormalKey, pr::AppId>,
              "a NormalKey needs a NormalContext, and only the launcher has one");
static_assert(!std::is_constructible_v<pr::IsolatedKey, pr::AppId>,
              "an IsolatedKey needs an IsolatedContext carrying the instance id");
static_assert(!std::is_constructible_v<pr::IsolatedKey, pr::AppId, pr::NormalContext&>,
              "a normal context must not convert into an isolated one, or the domains merge");
static_assert(!std::is_copy_constructible_v<pr::NormalContext>,
              "copyable would mean portable into a T1 instance, where it must not be");
static_assert(!pr::IsProfileKey<std::string>::value && !pr::IsProfileKey<pr::AppId>::value,
              "the trait is closed: an app id is not a key");

std::string Show(const std::optional<pr::Value>& v) {
  return v ? v->ToToml() : std::string{"<unset>"};
}

bool IsWord(const std::optional<pr::Value>& v, const char* want) {
  const std::string* s = v ? v->as_string() : nullptr;
  return s != nullptr && *s == want;
}

void closed_lookups_are_a_compile_time_fact() {
  // Nothing to run: the static_asserts above are the case. This exists so the lane prints that fact and
  // a reader who sees only runtime cases does not go looking for one.
  pr::Store store;
  CHECK_CTX(store.rows() == 0u, "a fresh store has no rows");
  CHECK_CTX(store.unknown().empty(), "a fresh store has nothing refused");
}

void schema_defaults_are_the_answers_that_need_no_probe() {
  pr::Store store;
  for (const auto n : store.names()) {
    const pr::Schema* s = pr::SchemaOf(n);
    CHECK_CTX(s != nullptr && s->name == n,
              std::string{"names() returned a name with no schema row, or the wrong one: "} +
                  pr::NameOf(n));
  }
  CHECK_CTX(store.names().size() == static_cast<std::size_t>(pr::Name::kCount),
            "names() covers " + std::to_string(store.names().size()) + " of " +
                std::to_string(static_cast<int>(pr::Name::kCount)));

  pr::NormalContext ctx;
  const pr::NormalKey key{"com.example.app", ctx};
  const auto expect = [&](pr::Name n, const char* want) {
    pr::Source src = pr::Source::kNone;
    const auto v = store.Get(key, n, &src);
    CHECK_CTX(IsWord(v, want),
              std::string{"the compiled default of "} + pr::NameOf(n) + " is " + Show(v) +
                  ", want \"" + want + "\"");
    CHECK_CTX(src == pr::Source::kSchemaDefault,
              std::string{"an empty store resolves "} + pr::NameOf(n) + " from " +
                  pr::SourceName(src));
  };
  // Each of these three is a decision, not a placeholder. `normal` because T1 is never the default;
  // `interp` because it is the tier that cannot fail to translate; `sw_angle` because it is the
  // reference renderer (AD-15), so a store nobody probed is slow and correct rather than fast and wrong.
  expect(pr::Name::kTier, "normal");
  expect(pr::Name::kCodeTier, "interp");
  expect(pr::Name::kGfxExecutor, "sw_angle");

  pr::Source src = pr::Source::kNone;
  CHECK_CTX(!store.Get(key, pr::Name::kCount, &src), "a name outside the schema resolves to nothing");
  CHECK_CTX(src == pr::Source::kNone,
            "and says none, not default, so a caller can tell the two apart");
}

void a_probe_cannot_choose_the_tier() {
  pr::Store store;
  const auto refused = store.ApplyProbes({
      {pr::Name::kTier, pr::Value::Str("isolated"), "seccomp looks tight, safer in a sandbox"},
      {pr::Name::kCodeTier, pr::Value::Str("jit"), "execmem probe: RW->RX executes"},
      {pr::Name::kCount, pr::Value::Str("anything"), "a probe inventing a name"},
  });
  CHECK_CTX(refused.size() == 2u,
            "one probe accepted, two refused, got " + std::to_string(refused.size()) + ":" + All(refused));
  CHECK_CTX(refused.size() == 2u && refused[0].find("not probe-supplied") != std::string::npos,
            refused.empty() ? "no refusal at all" : refused[0]);
  CHECK_CTX(refused.size() == 2u && refused[0].find("AD-18") != std::string::npos,
            "the refusal should say why, not just no: " + refused[0]);
  CHECK_CTX(refused.size() == 2u && refused[1].find("no setting") != std::string::npos,
            refused.size() == 2u ? refused[1] : "the out-of-range probe was not refused");

  pr::NormalContext ctx;
  const pr::NormalKey key{"com.example.app", ctx};
  pr::Source src = pr::Source::kNone;
  CHECK_CTX(IsWord(store.Get(key, pr::Name::kTier, &src), "normal") &&
                src == pr::Source::kSchemaDefault,
            "a probe reached the tier: " + Show(store.Get(key, pr::Name::kTier)) + " from " +
                pr::SourceName(src));
  CHECK_CTX(IsWord(store.Get(key, pr::Name::kCodeTier, &src), "jit") && src == pr::Source::kProbe,
            "a probe that was allowed did not land: " + Show(store.Get(key, pr::Name::kCodeTier)) +
                " from " + pr::SourceName(src));
  CHECK_CTX(store.probe(pr::Name::kCodeTier) != nullptr &&
                store.probe(pr::Name::kCodeTier)->evidence.find("execmem") != std::string::npos,
            "the probe's evidence has to survive, or C17 cannot report how it was established");
  CHECK_CTX(!store.has_probe(pr::Name::kTier), "a refused probe must not be stored as a probe either");
}

void an_override_wins_over_a_probe_and_says_so() {
  pr::Store store;
  CHECK_CTX(store.ApplyProbes({{pr::Name::kCodeTier, pr::Value::Str("jit"), "probed"}}).empty(),
            "the probe should be accepted");
  pr::NormalContext ctx;
  const pr::NormalKey key{"com.example.app", ctx};
  std::string err;
  CHECK_CTX(store.Set(key, pr::Name::kCodeTier, pr::Value::Str("interp"), &err), err);
  pr::Source src = pr::Source::kNone;
  CHECK_CTX(IsWord(store.Get(key, pr::Name::kCodeTier, &src), "interp"),
            Show(store.Get(key, pr::Name::kCodeTier)));
  CHECK_CTX(src == pr::Source::kOverride, pr::SourceName(src));
  // The other rows of the same app are untouched by all of this, which is the AD-18 half that is about
  // data rather than about types: nothing here has a way to say "interp implies patches".
  CHECK_CTX(IsWord(store.Get(key, pr::Name::kTier, &src), "normal") &&
                src == pr::Source::kSchemaDefault,
            "a code tier chosen by the user moved the isolation tier");
}

void tier_and_patches_are_independent_fields() {
  pr::Store store;
  pr::NormalContext ctx;
  const pr::NormalKey key{"com.example.app", ctx};
  std::string err;

  CHECK_CTX(store.Set(key, pr::Name::kTier, pr::Value::Str("isolated"), &err), err);
  CHECK_CTX(IsWord(store.Get(key, pr::Name::kTier), "isolated"), "tier did not take");
  const auto patch_value = store.Get(key, pr::Name::kPatches);
  const auto* patches = patch_value->as_strings();
  CHECK_CTX(patches != nullptr && patches->empty(),
            "choosing T1 implied patches: " + Show(store.Get(key, pr::Name::kPatches)));

  const std::vector<std::string> two{"rotate180", "ssl-bypass-legacy"};
  CHECK_CTX(store.Set(key, pr::Name::kPatches, pr::Value::Strs(two), &err), err);
  CHECK_CTX(IsWord(store.Get(key, pr::Name::kTier), "isolated"), "adding a patch moved the tier");
  CHECK_CTX(!store.Get(key, pr::Name::kCodeTier)->as_strings(),
            "the code tier took a patch list from somewhere");

  // The two fields cannot be confused by value either: a tier word is never a legal patches value and
  // the reverse holds too. If the kinds ever overlapped, "patched ⇒ isolated" would be expressible as a
  // single assignment — which is the thing AD-18 says must be impossible in the data model.
  CHECK_CTX(pr::SchemaOf(pr::Name::kTier)->kind != pr::SchemaOf(pr::Name::kPatches)->kind,
            "tier and patches share a kind");
  CHECK_CTX(!store.Set(key, pr::Name::kPatches, pr::Value::Str("isolated"), &err),
            "a tier word is also a valid patches value: " + err);
  CHECK_CTX(!store.Set(key, pr::Name::kTier, pr::Value::Strs({"isolated"}), &err),
            "a patch list is also a valid tier");
}

void normal_and_isolated_rows_cannot_see_each_other() {
  pr::Store store;
  pr::NormalContext normal;
  pr::IsolatedContext iso{"inst-7"};
  const pr::NormalKey nk{"com.example.app", normal};
  const pr::IsolatedKey ik{"com.example.app", iso};
  CHECK_CTX(nk.Row() != ik.Row(), nk.Row() + " vs " + ik.Row());

  std::string err;
  CHECK_CTX(store.Set(nk, pr::Name::kCodeTier, pr::Value::Str("jit"), &err), err);
  CHECK_CTX(IsWord(store.Get(ik, pr::Name::kCodeTier), "interp"),
            "the isolated instance inherited the normal row's JIT: " +
                Show(store.Get(ik, pr::Name::kCodeTier)));

  // ...and the converse, which is the direction that leaks in practice: a T1 row must not be able to
  // reach up into the launcher's own config for the same app.
  CHECK_CTX(store.Set(ik, pr::Name::kGfxExecutor, pr::Value::Str("angle_gles1"), &err), err);
  CHECK_CTX(IsWord(store.Get(nk, pr::Name::kGfxExecutor), "sw_angle"),
            "the normal row saw the instance's executor: " +
                Show(store.Get(nk, pr::Name::kGfxExecutor)));
  CHECK_CTX(store.rows() == 2u, std::to_string(store.rows()) + " rows");
}

void two_instances_of_one_app_are_two_rows() {
  pr::Store store;
  const pr::IsolatedContext a{"inst-1"}, b{"inst-2"};
  const pr::IsolatedKey ka{"com.example.app", a}, kb{"com.example.app", b};
  std::string err;
  CHECK_CTX(store.Set(ka, pr::Name::kCodeTier, pr::Value::Str("jit"), &err), err);
  pr::Source src = pr::Source::kNone;
  CHECK_CTX(IsWord(store.Get(kb, pr::Name::kCodeTier, &src), "interp") &&
                src == pr::Source::kSchemaDefault,
            "instance inst-2 inherited inst-1's code tier: " +
                Show(store.Get(kb, pr::Name::kCodeTier)) + " from " + pr::SourceName(src));
  // Only one row exists: reading the other instance resolved to the schema default rather than
  // materialising a copy. There is no copy operation on the store, which is the "a T1 row is created
  // from probes, never copied from a T0 row" line of docs/20 §C14 as a type fact — nothing to call.
  CHECK_CTX(store.rows() == 1u,
            std::to_string(store.rows()) + " rows, want the one that was written");
}

void a_wrong_kind_or_word_is_refused_naming_what_it_wants() {
  pr::Store store;
  pr::NormalContext ctx;
  const pr::NormalKey key{"com.example.app", ctx};
  std::string err;

  CHECK_CTX(!store.Set(key, pr::Name::kPatches, pr::Value::Str("rotate180"), &err),
            "a bare string where an array belongs must not be converted");
  CHECK_CTX(err.find("string array") != std::string::npos, err);

  CHECK_CTX(!store.Set(key, pr::Name::kTier, pr::Value::Str("isolatd"), &err),
            "a typo in a closed value must not become a setting");
  CHECK_CTX(err.find("normal|isolated") != std::string::npos, err);

  CHECK_CTX(!store.Set(key, pr::Name::kCodeTier, pr::Value::Strs({"jit"}), &err), "wrong kind again");
  CHECK_CTX(err.find("got string array") != std::string::npos, err);

  CHECK_CTX(!store.Set(key, pr::Name::kCount, pr::Value::Str("x"), &err), "no such setting");
  CHECK_CTX(err.find("no setting") != std::string::npos, err);

  // A refused value must not have left a row behind: a `Set` that returns false *and* stores half a
  // value is the failure a caller cannot recover from.
  CHECK_CTX(store.rows() == 0u, std::to_string(store.rows()) + " rows after four refusals");
}

void toml_round_trips_through_the_same_store() {
  pr::Store store;
  pr::NormalContext ctx;
  const pr::NormalKey key{"com.example.app", ctx};
  std::string err;
  CHECK_CTX(store.Set(key, pr::Name::kTier, pr::Value::Str("isolated"), &err), err);
  const std::vector<std::string> awkward{"a, comma", "b \"quoted\" c", "back\\slash"};
  CHECK_CTX(store.Set(key, pr::Name::kPatches, pr::Value::Strs(awkward), &err), err);
  CHECK_CTX(store.Set(key, pr::Name::kCodeTier, pr::Value::Str("jit"), &err), err);

  const std::string text = store.SaveToml(key);
  // Asserted before it is reloaded, because a writer that silently drops a key is invisible to a
  // round-trip alone: the reload would just resolve to the default and the test would still pass.
  // Table order is the write order, and the section is the quoted row.
  CHECK_CTX(text ==
                "# retrobug profile, schema 1\n[\"normal/com.example.app\"]\n"
                "tier = \"isolated\"\n"
                "code_tier = \"jit\"\n"
                "patches = [\"a, comma\", \"b \\\"quoted\\\" c\", \"back\\\\slash\"]\n",
            "SaveToml wrote:\n" + text);
  pr::Store again;
  const auto reports = again.LoadToml(text, key);
  CHECK_CTX(reports.empty(),
            std::to_string(reports.size()) + " report(s) from our own output:" + All(reports));
  CHECK_CTX(IsWord(again.Get(key, pr::Name::kTier), "isolated"), Show(again.Get(key, pr::Name::kTier)));
  CHECK_CTX(IsWord(again.Get(key, pr::Name::kCodeTier), "jit"), "code_tier did not survive");
  // `as_strings()` points *into* the value, so the optional has to be held: binding the pointer alone
  // leaves it addressing a temporary that is already gone, which is what this case's first run did — it
  // read as an empty list, and the empty list is also the default, so nothing else could tell.
  const auto pv = again.Get(key, pr::Name::kPatches);
  const auto* l = pv->as_strings();
  CHECK_CTX(l != nullptr && l->size() == 3u, l ? std::to_string(l->size()) + " items" : "no list");
  if (l != nullptr && l->size() == 3u) {
    // The comma-inside-quotes case is why this parses by scanning instead of splitting on ','.
    CHECK_CTX((*l)[0] == "a, comma", "got [" + (*l)[0] + "]");
    CHECK_CTX((*l)[1] == "b \"quoted\" c", "got [" + (*l)[1] + "]");
    CHECK_CTX((*l)[2] == "back\\slash", "got [" + (*l)[2] + "]");
  }
  CHECK_CTX(again.rows() == 1u, std::to_string(again.rows()) + " rows");

  // The same bytes aimed at a different app's row. The section is part of the address, so nothing
  // applies and every line says so — this is the file form of the cross-row leak the key types prevent,
  // and it is why `LoadToml` takes a key rather than a path plus a hope.
  const pr::NormalKey another{"com.different.app", ctx};
  const auto misused = again.LoadToml(text, another);
  CHECK_CTX(misused.size() == 3u,
            std::to_string(misused.size()) + " report(s), want 3:" + All(misused));
  CHECK_CTX(IsWord(again.Get(another, pr::Name::kTier), "normal"),
            "another app's file was applied to this row: " +
                Show(again.Get(another, pr::Name::kTier)));
  CHECK_CTX(again.rows() == 1u, std::to_string(again.rows()) + " rows after a mis-targeted load");
}

void every_refused_line_is_reported_not_dropped() {
  pr::Store store;
  pr::NormalContext ctx;
  const pr::NormalKey key{"com.example.app", ctx};
  const pr::NormalKey other{"com.other.app", ctx};
  const auto reports = store.LoadToml(
      "# a comment line is fine\n"
      "code_tier = \"jit\"\n"                                // before any section
      "[normal/com.other.app]\n"
      "code_tier = \"jit\"\n"                                // the wrong section (Xenia's bug)
      "[normal/com.example.app]\n"
      "code_tier = jit\n"                                    // bare word, not a quoted string
      "code_tier = \"jit\"\n"
      "code_tier = \"interp\"\n"                             // duplicate
      "patched = \"rotate180\"\n"                            // unknown key
      "patches = \"rotate180\"\n"                            // right key, wrong kind
      "tier = \"yes please\"\n"                              // right kind, unlisted word
      "gfx_executor = [\"sw_angle\", \"angle_metal\"]\n"     // a list where a word belongs
      "gfx_executor = \"angle_gles1\"\n",                    // the word form of the same key
      key);
  CHECK_CTX(reports.size() == 8u, std::to_string(reports.size()) + " reports, want 8:" + All(reports));
  CHECK_CTX(store.unknown() == reports, "the reports are not kept for the echo");
  // Two lines were well-formed and in the right section: the duplicate `code_tier` (where the later line
  // wins and says so) and the final `gfx_executor`. The refused list does not count as a previous
  // setting, so the word form after it is not a duplicate.
  pr::Source src = pr::Source::kNone;
  CHECK_CTX(IsWord(store.Get(key, pr::Name::kGfxExecutor, &src), "angle_gles1") &&
                src == pr::Source::kOverride,
            "the one valid line did not apply: " +
                Show(store.Get(key, pr::Name::kGfxExecutor)));
  // ...except the duplicate, where the later line wins and says so.
  CHECK_CTX(IsWord(store.Get(key, pr::Name::kCodeTier, &src), "interp") &&
                src == pr::Source::kOverride,
            "a refused or duplicate line changed what was stored: " +
                Show(store.Get(key, pr::Name::kCodeTier)));
  CHECK_CTX(!store.Get(key, pr::Name::kPatches, &src)->as_string(),
            "a wrong-kind value was stored anyway");
  // A key under another app's section must not reach either row — the section is part of the address,
  // and "it went somewhere in the store" is not an answer.
  CHECK_CTX(IsWord(store.Get(other, pr::Name::kCodeTier, &src), "interp") &&
                src == pr::Source::kSchemaDefault,
            "another section's key leaked into that app's row: " +
                Show(store.Get(other, pr::Name::kCodeTier)));
  CHECK_CTX(store.rows() == 1u, std::to_string(store.rows()) + " rows, want the one that applied");
  const bool mentions_keys =
      reports.size() == 8u && reports[4].find("the schema accepts") != std::string::npos;
  CHECK_CTX(mentions_keys,
            "the unknown-key report should list what the schema does accept: " +
                (reports.size() == 8u ? reports[4] : "-"));
}

void an_override_of_nothing_is_echoed_as_inert() {
  pr::Store store;
  pr::NormalContext ctx;
  const pr::NormalKey key{"com.example.app", ctx};
  std::string err;
  CHECK_CTX(store.Set(key, pr::Name::kCodeTier, pr::Value::Str("jit"), &err), err);

  diag::Recorder rec;
  store.Echo(rec, "profile", key, "loaded from the app's own file");
  CHECK_CTX(rec.entries().size() == 1u, std::to_string(rec.entries().size()) + " entries");
  if (rec.entries().size() == 1u) {
    const auto& e = rec.entries().front();
    CHECK_CTX(e.field == diag::Field::kProfileApplied, "wrong field echoed");
    CHECK_CTX(e.outcome == diag::Outcome::kInert,
              std::string{"an override with nothing probed under it reported "} +
                  diag::Name(e.outcome));
    CHECK_CTX(e.requested.find("code_tier=\"jit\"") != std::string::npos, e.requested);
    CHECK_CTX(e.active.find("code_tier=\"jit\" (override)") != std::string::npos, e.active);
    CHECK_CTX(e.active.find("tier=\"normal\" (default)") != std::string::npos, e.active);
    CHECK_CTX(e.evidence.find("compiled default") != std::string::npos, e.evidence);
    CHECK_CTX(e.evidence.find("schema v1") != std::string::npos, e.evidence);
  }
  // `Echo` has to satisfy the field it asks for. It is not the only field a *launch* must report — the
  // recorder statically requires seven others, which belong to components this test does not run — so
  // the assertion is about `profile-applied` and nothing else.
  const auto missing = rec.MissingRequired();
  CHECK_CTX(std::find(missing.begin(), missing.end(),
                      std::string{diag::Name(diag::Field::kProfileApplied)}) == missing.end(),
            "Echo did not satisfy the field it requires:" + All(missing));

  // Now the same override with a probe beneath it: identical resolution, a different fact about the
  // launch, and the report has to say which one it is.
  const std::vector<pr::ProbeResult> probed{
      {pr::Name::kCodeTier, pr::Value::Str("interp"), "execmem probe: RW->RX refused on this build"}};
  CHECK_CTX(store.ApplyProbes(probed).empty(), "the probe was refused");
  diag::Recorder rec2;
  store.Echo(rec2, "profile", key, "loaded from the app's own file");
  CHECK_CTX(rec2.entries().size() == 1u && rec2.entries().front().outcome == diag::Outcome::kActive,
            "still inert with a probe underneath: " +
                (rec2.entries().empty() ? std::string{"-"} : rec2.entries().front().evidence));
  CHECK_CTX(IsWord(store.Get(key, pr::Name::kCodeTier), "jit"),
            "the probe outranked the override, which is backwards: " +
                Show(store.Get(key, pr::Name::kCodeTier)));
}

void the_schema_table_is_consistent_with_itself() {
  std::set<std::string> keys;
  pr::Store store;
  pr::NormalContext ctx;
  const pr::NormalKey kx{"x", ctx};
  for (std::size_t i = 0; i < static_cast<std::size_t>(pr::Name::kCount); ++i) {
    const auto n = static_cast<pr::Name>(i);
    const pr::Schema* s = pr::SchemaOf(n);
    CHECK_CTX(s != nullptr && s->name == n,
              "name " + std::to_string(i) + " has no row, or the wrong one");
    if (s == nullptr) continue;
    CHECK_CTX(keys.insert(s->toml_key).second,
              std::string{"duplicate toml key "} + s->toml_key);
    CHECK_CTX(pr::SchemaOfKey(s->toml_key) == s, "a key that does not resolve back to its row");
    std::string err;
    // The default has to be valid by the row's own rule. A schema whose default its own validator
    // rejects is a bug no file can trigger, so nothing else would ever catch it.
    CHECK_CTX(store.Set(kx, n, s->default_value, &err),
              std::string{"the compiled default of "} + s->toml_key + " is refused: " + err);
    std::set<std::string> words;
    for (std::size_t j = 0; j < s->allowed.count; ++j) {
      const std::string w{s->allowed.values[j]};
      CHECK_CTX(!w.empty(), "an empty accepted word in " + std::string{s->toml_key});
      CHECK_CTX(words.insert(w).second, "a repeated accepted word in " + std::string{s->toml_key});
    }
  }
  CHECK_CTX(pr::SchemaOf(pr::Name::kCount) == nullptr, "kCount must not have a row");
  CHECK_CTX(pr::SchemaOfKey("tier ") == nullptr, "SchemaOfKey is exact; the parser trims before it");
  CHECK_CTX(std::string{pr::NameOf(pr::Name::kTier)} == pr::SchemaOf(pr::Name::kTier)->toml_key,
            "NameOf and the schema key diverged — one of the two is now a lie in the report");
  CHECK_CTX(std::string{pr::KindName(pr::Kind::kStrings)} == "string array", "kind naming drifted");
  CHECK_CTX(store.rows() == 1u, "the loop above should have created exactly one row");
}

void comments_and_blank_lines_are_not_settings() {
  pr::Store store;
  pr::NormalContext ctx;
  const pr::NormalKey key{"com.example.app", ctx};
  // `["row"]` is the section form `SaveToml` produces; the commented lines are what a person leaves
  // behind when they mean to turn a setting off.
  const auto reports = store.LoadToml(
      "\n# [normal/com.example.app]\n[\"normal/com.example.app\"]\n# code_tier = \"jit\"\n\n", key);
  CHECK_CTX(reports.empty(), std::to_string(reports.size()) + " report(s), want none:" + All(reports));
  CHECK_CTX(IsWord(store.Get(key, pr::Name::kCodeTier), "interp"),
            "a commented-out line was applied: " + Show(store.Get(key, pr::Name::kCodeTier)));
  CHECK_CTX(store.rows() == 0u, std::to_string(store.rows()) + " rows from comments");
}

/// Named rather than a bare call list: this lane's cases each build a store, and a crash with no case
/// name printed says nothing about which one (docs/30, the C04 lane's first run).
#define RUN(fn)                                   \
  do {                                            \
    std::fprintf(stderr, "[profile] %s\n", #fn);  \
    std::fflush(stderr);                          \
    fn();                                         \
  } while (0)

}  // namespace

int main() {
  RUN(closed_lookups_are_a_compile_time_fact);
  RUN(schema_defaults_are_the_answers_that_need_no_probe);
  RUN(a_probe_cannot_choose_the_tier);
  RUN(an_override_wins_over_a_probe_and_says_so);
  RUN(tier_and_patches_are_independent_fields);
  RUN(normal_and_isolated_rows_cannot_see_each_other);
  RUN(two_instances_of_one_app_are_two_rows);
  RUN(a_wrong_kind_or_word_is_refused_naming_what_it_wants);
  RUN(toml_round_trips_through_the_same_store);
  RUN(every_refused_line_is_reported_not_dropped);
  RUN(an_override_of_nothing_is_echoed_as_inert);
  RUN(the_schema_table_is_consistent_with_itself);
  RUN(comments_and_blank_lines_are_not_settings);

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
