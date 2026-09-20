/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C14 `rb::profile` — one store, one schema, closed lookups (AD-12).
 *
 * The component exists to make three mistakes unrepresentable rather than discouraged, and each of the
 * three has a test that fails if the mechanism is removed:
 *
 *   1. **"fall back to global"**. `Store::Get` accepts `NormalKey` or `IsolatedKey` and nothing else —
 *      there is no overload taking an app id string, and `IsProfileKey` is a closed trait, so a caller
 *      that tries `Get("com.foo", name)` does not compile. The trait is asserted in the test file with
 *      `static_assert`, which is the only way to test a rule whose whole purpose is that a program
 *      cannot be written that violates it.
 *   2. **a typo inventing a setting.** `Name` is an enum, not a string; every name has a schema row
 *      declaring its key, its kind, the closed set of values it accepts where that applies, and whether
 *      a probe may supply it. A file's unknown key — or a key sitting under a section that is not this
 *      app's row — is collected and reported, never silently dropped, because `docs/research/08`
 *      §"silent-ignore interfaces" lists "a wrong TOML section" and "unknown keys" among the interfaces
 *      where "every one cost a debug session".
 *   3. **cross-tier inheritance.** A row's key carries its domain, so a `NormalKey` lookup cannot see an
 *      `IsolatedKey` row for the same app and same name — and the converse, which is the direction that
 *      actually leaks today in systems like this. The two keys are also built from different capability
 *      tokens (`NormalContext` held by the launcher, `IsolatedContext` held by a T1 instance), so an
 *      isolated instance cannot even form the string that addresses normal config. A T1 row is therefore
 *      created from probes, never copied from a T0 row: there is no operation that takes one row's rows.
 *
 * Two further rules from AD-12/AD-18 shape the API rather than sitting in comments. **A profile may only
 * override a probe result**: `Effective()` answers with its `Source` (override, probe, schema default),
 * and an override with no probe beneath it is an override of nothing — reported `inert` by `Echo`, not
 * quietly winning. **Tier and patches are separate fields that may not reference each other**: they are
 * two names with independent kinds and no cross-reference in the schema, `kTier` is explicitly *not*
 * probe-supplied (a probe choosing T1 would be the silent fallback AD-18 forbids), and the test asserts
 * that setting one never moves the other.
 *
 * The file format is the TOML subset this store can produce and parse itself (`["row"]` sections,
 * `key = "string"`, `key = ["a", "b"]`, whole-line comments), written by hand for the same reason the
 * rest of `core/` is: no new dependency, and a parser small enough that "unknown key reported, mistyped
 * value refused, disallowed word refused" is a fact the tests hold rather than a promise about a
 * third-party library's behaviour. `kSchemaVersion` is the version of the table below; it is echoed so a
 * profile from an older build is at least attributable, and migration is deliberately *not* invented
 * here — nothing in this store is applied without appearing in the report, which is what an unmigrated
 * key ends up as: reported, not applied.
 *
 * See docs/20 §C14 and docs/10 §11.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "rb/diag/diag.h"

namespace rb::profile {

/// Version of the schema table below. Reported with every profile echo.
constexpr unsigned kSchemaVersion = 1;

/// The app whose profile this is, as an opaque id. Nothing in `core/` compares it against a literal:
/// brief (g) forbids per-app logic, and the one legitimate per-app behaviour is data the App Revival
/// system carries, not a branch here.
using AppId = std::string;
/// A T1 instance, from C16's `create`. Empty is never valid; the type is separate so a mistake is a
/// compile error rather than a row in the wrong bucket.
using InstanceId = std::string;

/// Every setting the schema knows. Adding a setting means adding an enumerator, a `SchemaOf` row and a
/// test; it cannot happen by a string appearing in a file.
enum class Name : std::uint8_t {
  /// T0 or T1, as the user chose. AD-18's field: it never implies `kPatches` and is never implied by it,
  /// and no probe may supply it — see the schema row.
  kTier = 0,
  /// Which executor the *host* should use for a guest GL call, from the GLES1-artefact and feature
  /// probes (C12). Renamed from the plan sketch's "gfx backend" because it selects an executor, not a
  /// driver.
  kGfxExecutor,
  /// Interpreter or JIT for the CPU (C02's `CodeTier`), which is the one a user is most likely to force
  /// when a tier misbehaves on one device — and the one C17 must then echo as requested-versus-active.
  kCodeTier,
  /// The App Revival patch list. A string array, deliberately, so a single patch cannot be confused with
  /// a tier: the value has no meaning that overlaps `kTier`'s.
  kPatches,
  kCount,
};

/// What a name holds. The parser and `Set` both refuse a value of the wrong kind and say which was
/// expected, because the interesting failure of a typed store is the silent conversion.
enum class Kind : std::uint8_t { kString, kStrings };

[[nodiscard]] const char* NameOf(Name);   ///< the setting's own name, for logs and reports
[[nodiscard]] const char* KindName(Kind);

/// The closed set of words a `kString` name accepts. Empty means "any string", which is only right for
/// data the profile does not interpret — the patch ids, whose vocabulary belongs to C15.
struct Allowed {
  const char* const* values = nullptr;
  std::size_t count = 0;
  [[nodiscard]] bool empty() const { return values == nullptr || count == 0; }
};

/// A typed value. Deliberately not a `std::variant`: the store has to say *why* a value was refused, and
/// that is easier with two accessors and a kind than with a visit.
class Value {
 public:
  Value() = default;
  static Value Str(std::string v) {
    Value x;
    x.kind_ = Kind::kString;
    x.s_ = std::move(v);
    return x;
  }
  static Value Strs(std::vector<std::string> v) {
    Value x;
    x.kind_ = Kind::kStrings;
    x.list_ = std::move(v);
    return x;
  }

  [[nodiscard]] Kind kind() const { return kind_; }
  /// `nullptr` for the wrong kind, which is the same refusal discipline as `Store::Set`. Both borrow:
  /// they point into *this* value, so a caller has to hold the value — or, from `Store::Get`, the
  /// optional around it — rather than the pointer.
  [[nodiscard]] const std::string* as_string() const {
    return kind_ == Kind::kString ? &s_ : nullptr;
  }
  [[nodiscard]] const std::vector<std::string>* as_strings() const {
    return kind_ == Kind::kStrings ? &list_ : nullptr;
  }

  /// Rendered the way the file format wants it, so a value and a line are the same thing in two places.
  [[nodiscard]] std::string ToToml() const;

 private:
  Kind kind_ = Kind::kString;
  std::string s_;
  std::vector<std::string> list_;
};

struct Schema {
  Name name;
  const char* toml_key;   ///< exactly the spelling in the file, so a typo there is an unknown key
  Kind kind;
  /// May a probe supply this? `false` is the load-bearing answer for `kTier`: a probe that could choose
  /// T1 would be the silent fallback AD-18 forbids, so `ApplyProbes` refuses it out loud.
  bool probe_supplied;
  Allowed allowed;
  Value default_value;
};

/// The one schema. `nullptr` from `SchemaOf` means "not in the schema", which is the same answer an
/// unknown key in a file gets.
[[nodiscard]] const Schema* SchemaOf(Name);
/// The schema row for a TOML key, or `nullptr`. This is what makes an unknown key detectable at all.
[[nodiscard]] const Schema* SchemaOfKey(const std::string& toml_key);

/// Where the answer for a name came from. Reported, not assumed: "the default won" and "an override won"
/// are different facts about a launch, and AD-12's rule that a profile may only *override* a probe result
/// is only checkable if the resolution keeps the distinction.
enum class Source : std::uint8_t { kOverride, kProbe, kSchemaDefault, kNone };
[[nodiscard]] const char* SourceName(Source);

/// The two key types, and the closed set they form. `IsProfileKey` is what `Store::Get` and
/// `Store::Set` are templated on, so the only way to be a key is to be one of these two.
template <class T>
struct IsProfileKey : std::false_type {};

/// Held by the launcher for the normal path. Non-copyable and created once, which is what makes
/// "an isolated instance cannot build a `NormalKey`" a type fact rather than a convention.
class NormalContext {
 public:
  NormalContext() = default;
  NormalContext(const NormalContext&) = delete;
  NormalContext& operator=(const NormalContext&) = delete;
};

/// Held by a T1 instance, from C16's `enter`. Carries the instance id so a key built from it cannot be
/// made to address another instance's row by accident.
class IsolatedContext {
 public:
  explicit IsolatedContext(InstanceId id) : id_(std::move(id)) {}
  [[nodiscard]] const InstanceId& instance() const { return id_; }

 private:
  InstanceId id_;
};

class NormalKey {
 public:
  NormalKey(AppId app, const NormalContext&) : app_(std::move(app)) {}
  [[nodiscard]] const AppId& app() const { return app_; }
  /// The store's row id. The domain is *inside* the row, which is what makes the two buckets
  /// unaddressable from each other without a type conversion nobody can accidentally omit.
  [[nodiscard]] std::string Row() const { return "normal/" + app_; }

 private:
  AppId app_;
};

class IsolatedKey {
 public:
  IsolatedKey(AppId app, const IsolatedContext& ctx)
      : app_(std::move(app)), instance_(ctx.instance()) {}
  [[nodiscard]] const AppId& app() const { return app_; }
  [[nodiscard]] const InstanceId& instance() const { return instance_; }
  [[nodiscard]] std::string Row() const { return "isolated/" + instance_ + "/" + app_; }

 private:
  AppId app_;
  InstanceId instance_;
};

template <>
struct IsProfileKey<NormalKey> : std::true_type {};
template <>
struct IsProfileKey<IsolatedKey> : std::true_type {};

/// The closed set: the only types any lookup or override of this store accepts. Constraining the
/// template — rather than `static_assert`ing inside its body — is what makes `Get(app_id, name)` fail to
/// *name* a function at the call site. A body-level assert only fires once something instantiates the
/// template, and an uninstantiated one is a convention again.
template <class K>
concept ProfileKey = IsProfileKey<K>::value;

/// A probe answer, fed in by the launcher after the real measurements (U-2's reservation probe, U-13's
/// execmem probe, the GPU feature probe). This type exists so the store's "defaults come from probes"
/// rule has a single entrance rather than a global someone reads.
struct ProbeResult {
  Name name;
  Value value;
  std::string evidence;  ///< how it was established, so the report can carry it
};

/// One store, one schema. Not thread-safe: an instance's store belongs to the thread that launches it,
/// and a shared mutable profile map is exactly the cross-instance leak the key types are built to
/// prevent.
///
/// Every public entry point takes a `ProfileKey`. That is deliberate and is the reason the row-string
/// half of the implementation is private: a `Get` that is closed but an `Effective(row, name)` that is
/// open is a closed lookup with the door off its hinges, and a caller can type any row it likes.
class Store {
 public:
  Store() = default;

  /// An override for one row. Refused, with the expected kind or the accepted words named, if the value
  /// is not what the schema says.
  template <ProfileKey K>
  [[nodiscard]] bool Set(const K& key, Name name, Value value, std::string* err) {
    return SetRow(key.Row(), name, std::move(value), err);
  }

  /// Resolve one name: override, then probe, then schema default. `source` says which, and it is the
  /// reason this is the entry point rather than a bare value — AD-12's "a profile may only override a
  /// probe result" is only checkable if the answer keeps where it came from.
  template <ProfileKey K>
  [[nodiscard]] std::optional<Value> Get(const K& key, Name name, Source* source = nullptr) const {
    return Effective(key.Row(), name, source);
  }

  /// Feed the probe answers in — the plan sketch's `DeriveDefaults`, with the probe list as the argument
  /// instead of an `ImportedApp`, because C05/C06 do not exist yet and the launcher is what holds the
  /// real measurement results. A probe may only supply names the schema marks probe-supplied; anything
  /// else is refused and reported, because a probe that can set an arbitrary name is the
  /// ambient-fallback hole with a different label on it.
  std::vector<std::string> ApplyProbes(std::vector<ProbeResult> results);

  /// The file side, for one row. `LoadToml` reports every key it did not recognise, could not type,
  /// found under a section that is not this app's row, or could not apply — those strings are the
  /// component's whole answer to "silently ignored", and the launcher is expected to put them in the C17
  /// report rather than in a log level nobody reads.
  template <ProfileKey K>
  std::vector<std::string> LoadToml(const std::string& text, const K& key) {
    return LoadTomlRow(text, key.Row());
  }
  template <ProfileKey K>
  [[nodiscard]] std::string SaveToml(const K& key) const {
    return SaveTomlRow(key.Row());
  }

  /// Names this row overrides with nothing probed beneath them: an override of nothing. AD-12's rule
  /// that a profile may only override a *probe result*, as a query rather than a prohibition — the
  /// override still resolves (refusing to apply a user's explicit choice would be its own kind of
  /// silent), but `Echo` reports these as inert and C15 must not treat them as evidence of anything.
  template <ProfileKey K>
  [[nodiscard]] std::vector<std::string> OverridesOfNothing(const K& key) const {
    return OverridesOfNothingRow(key.Row());
  }

  /// C17: `profile-applied` — requested (the row's own overrides) versus active (what actually resolved,
  /// with its source) — so "the profile was loaded" cannot be reported as success while nothing in it
  /// was applied.
  template <ProfileKey K>
  void Echo(diag::Recorder& rec, const std::string& stage, const K& key,
            const std::string& evidence) const {
    EchoRow(rec, stage, key.Row(), evidence);
  }

  /// Every name in the schema, in table order. The launcher uses this to ask the same question of all of
  /// them rather than a hand-written list that drifts.
  [[nodiscard]] std::vector<Name> names() const;

  [[nodiscard]] std::size_t rows() const { return overrides_.size(); }
  /// Lines refused across every file this store loaded, oldest first — store-wide on purpose: a line
  /// nobody could place is still a line that was not applied, and dropping it from the report because it
  /// could not be attributed would be the silent-ignore behaviour again.
  [[nodiscard]] std::vector<std::string> unknown() const { return unknown_; }
  [[nodiscard]] bool has_probe(Name) const;
  /// `nullptr` if no probe supplied it. The evidence string is how C17 says *how* a default was
  /// established, so it is exposed rather than folded into the value.
  [[nodiscard]] const ProbeResult* probe(Name) const;

 private:
  bool SetRow(const std::string& row, Name name, Value value, std::string* err);
  std::optional<Value> Effective(const std::string& row, Name name, Source* source) const;
  std::vector<std::string> LoadTomlRow(const std::string& text, const std::string& row);
  std::string SaveTomlRow(const std::string& row) const;
  std::vector<std::string> OverridesOfNothingRow(const std::string& row) const;
  void EchoRow(diag::Recorder&, const std::string& stage, const std::string& row,
               const std::string& evidence) const;

  std::map<std::string, std::map<std::size_t, Value>> overrides_;
  std::map<Name, ProbeResult> probes_;
  std::vector<std::string> unknown_;
};

}  // namespace rb::profile
