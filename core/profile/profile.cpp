/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * C14's schema, resolution order and file subset. The rules are stated on the types in
 * `rb/profile/profile.h`; what is here is the table, the parser, and the places where a refusal has to
 * be phrased so a reader can act on it ("expects a string array", "accepts one of normal|isolated"),
 * because a store that says only "invalid" reproduces the debug session this component was written to
 * avoid.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rb/profile/profile.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace rb::profile {
namespace {

/// The words each closed name accepts. These are the spellings C12's `Executor` and C02's `CodeTier`
/// use, so a profile line maps onto an enumerator without a translation table someone has to keep in
/// sync — and a word that matches none is refused rather than defaulted.
constexpr const char* kTierValues[] = {"normal", "isolated"};
constexpr const char* kGfxValues[] = {"host_native_gles3", "host_native_gles1", "angle_gles1",
                                      "sw_angle", "angle_metal"};
constexpr const char* kCodeTierValues[] = {"interp", "jit"};

/// The one schema (AD-12). Table order is the order `SaveToml` writes and `Echo` lists, so a report is
/// diffable across runs.
///
/// Three rows carry the weight of the design:
///   - `tier` defaults to `normal` and is *not* probe-supplied. A probe that could pick `isolated`
///     would be the silent fallback AD-18 forbids, so `ApplyProbes` refuses it even though the probe
///     side of the launcher is where every other default comes from.
///   - `code_tier` defaults to `interp`: with neither probe nor override, the tier that cannot fail to
///     translate an instruction is the correct fallback, and a JIT chosen by absence is a bug.
///   - `patches` has an empty `Allowed`: its vocabulary belongs to C15's data, and a schema that
///     enumerated patch ids here would be per-app logic in the engine (brief (g)).
const Schema kSchema[] = {
    {Name::kTier, "tier", Kind::kString, false, Allowed{kTierValues, std::size(kTierValues)},
     Value::Str("normal")},
    {Name::kGfxExecutor, "gfx_executor", Kind::kString, true,
     Allowed{kGfxValues, std::size(kGfxValues)}, Value::Str("sw_angle")},
    {Name::kCodeTier, "code_tier", Kind::kString, true,
     Allowed{kCodeTierValues, std::size(kCodeTierValues)}, Value::Str("interp")},
    {Name::kPatches, "patches", Kind::kStrings, false, Allowed{}, Value::Strs({})},
};

std::string Trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

void EscapeInto(std::string& out, const std::string& s) {
  out.push_back('"');
  for (const char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
}

/// The inverse of `EscapeInto`. Anything after a backslash is taken literally, so `\n` in a file stays
/// two characters rather than becoming something the store has to invent a meaning for.
std::string Unescape(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      out.push_back(s[++i]);
      continue;
    }
    out.push_back(s[i]);
  }
  return out;
}

/// The span of a quoted literal starting at `from`, which has to be scanned rather than split: a comma
/// inside `"a, b"` is data, and a parser that gets that wrong reports a good line as unknown. Returns
/// false on a non-quote or an unterminated literal so the caller can say which line is bad.
bool ReadQuoted(const std::string& s, std::size_t& i, std::string* out) {
  if (i >= s.size() || s[i] != '"') return false;
  ++i;
  std::string raw;
  while (i < s.size()) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      raw.push_back(s[i]);
      raw.push_back(s[i + 1]);
      i += 2;
      continue;
    }
    if (s[i] == '"') {
      ++i;
      *out = Unescape(raw);
      return true;
    }
    raw.push_back(s[i++]);
  }
  return false;
}

std::string Join(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}

std::vector<std::string> SchemaKeys() {
  std::vector<std::string> v;
  for (const auto& s : kSchema) v.push_back(s.toml_key);
  return v;
}

/// "a string", or "one of normal|isolated". Every refusal ends up quoting this, which is what makes the
/// two ways a value can be wrong — wrong kind and wrong word — read the same way in a report.
std::string Expected(const Schema& s) {
  if (s.allowed.empty()) return KindName(s.kind);
  std::vector<std::string> words;
  for (std::size_t i = 0; i < s.allowed.count; ++i) words.push_back(s.allowed.values[i]);
  return std::string{"a "} + KindName(s.kind) + " that is one of " + Join(words, "|");
}

/// The single validity rule, used by an override and by a probe alike. `Store::SetRow` and
/// `Store::ApplyProbes` both go through it so the two cannot drift apart — a probe accepted on a
/// path the user's own file would reject is the hole AD-12 is about.
bool Validate(const Schema& s, const Value& v, std::string* err) {
  if (v.kind() != s.kind) {
    if (err) {
      *err = std::string{s.toml_key} + " expects " + KindName(s.kind) + ", got " +
             KindName(v.kind()) + " (" + v.ToToml() + ")";
    }
    return false;
  }
  if (!s.allowed.empty()) {
    const auto ok_word = [&](const std::string& word) {
      const auto* b = s.allowed.values;
      return std::any_of(b, b + s.allowed.count, [&](const char* a) { return word == a; });
    };
    bool ok = false;
    if (const auto* one = v.as_string()) {
      ok = ok_word(*one);
    } else if (const auto* many = v.as_strings()) {
      ok = std::all_of(many->begin(), many->end(), ok_word);
    }
    if (!ok) {
      if (err) {
        *err = std::string{s.toml_key} + " accepts " + Expected(s) + ", not " + v.ToToml();
      }
      return false;
    }
  }
  return true;
}

/// One value's text after the `=`. Sets `parsed` false with a reason rather than guessing at intent:
/// a bare `code_tier = jit` (unquoted) is a person's mistake, not a spelling the format supports.
bool ParseValue(const std::string& rest, std::string* why, Value* out) {
  std::size_t i = 0;
  if (!rest.empty() && rest[0] == '[') {
    std::vector<std::string> list;
    i = 1;
    for (;;) {
      i = rest.find_first_not_of(" \t,", i);
      if (i == std::string::npos) {
        *why = "unterminated [ ]";
        return false;
      }
      if (rest[i] == ']') {
        *out = Value::Strs(std::move(list));
        return true;
      }
      std::string item;
      if (!ReadQuoted(rest, i, &item)) {
        *why = "expected a quoted string inside the array";
        return false;
      }
      list.push_back(std::move(item));
    }
  }
  std::string one;
  if (!ReadQuoted(rest, i, &one)) {
    *why = "expected a quoted string or a [ \"string\", ... ] array";
    return false;
  }
  if (!Trim(rest.substr(i)).empty()) {
    *why = "trailing text after the value";
    return false;
  }
  *out = Value::Str(std::move(one));
  return true;
}

}  // namespace

const char* NameOf(Name n) {
  switch (n) {
    case Name::kTier: return "tier";
    case Name::kGfxExecutor: return "gfx_executor";
    case Name::kCodeTier: return "code_tier";
    case Name::kPatches: return "patches";
    case Name::kCount: break;
  }
  return "<beyond the schema>";
}

const char* KindName(Kind k) {
  switch (k) {
    case Kind::kString: return "string";
    case Kind::kStrings: return "string array";
  }
  return "<not a kind>";
}

const char* SourceName(Source s) {
  switch (s) {
    case Source::kOverride: return "override";
    case Source::kProbe: return "probe";
    case Source::kSchemaDefault: return "default";
    case Source::kNone: return "none";
  }
  return "<not a source>";
}

const Schema* SchemaOf(Name n) {
  const auto i = static_cast<std::size_t>(n);
  return i < std::size(kSchema) ? &kSchema[i] : nullptr;
}

const Schema* SchemaOfKey(const std::string& toml_key) {
  for (const auto& s : kSchema) {
    if (toml_key == s.toml_key) return &s;
  }
  return nullptr;
}

std::string Value::ToToml() const {
  std::string out;
  if (kind_ == Kind::kString) {
    EscapeInto(out, s_);
    return out;
  }
  out.push_back('[');
  for (std::size_t i = 0; i < list_.size(); ++i) {
    if (i) out += ", ";
    EscapeInto(out, list_[i]);
  }
  out.push_back(']');
  return out;
}

bool Store::SetRow(const std::string& row, Name name, Value value, std::string* err) {
  const Schema* s = SchemaOf(name);
  if (s == nullptr) {
    if (err) *err = std::string{"the schema has no setting "} + NameOf(name);
    return false;
  }
  if (!Validate(*s, value, err)) return false;
  overrides_[row][static_cast<std::size_t>(name)] = std::move(value);
  return true;
}

std::optional<Value> Store::Effective(const std::string& row, Name name, Source* source) const {
  const Schema* s = SchemaOf(name);
  if (s == nullptr) {
    if (source) *source = Source::kNone;
    return std::nullopt;
  }
  const auto r = overrides_.find(row);
  if (r != overrides_.end()) {
    const auto v = r->second.find(static_cast<std::size_t>(name));
    if (v != r->second.end()) {
      if (source) *source = Source::kOverride;
      return v->second;
    }
  }
  if (s->probe_supplied) {
    const auto p = probes_.find(name);
    if (p != probes_.end()) {
      if (source) *source = Source::kProbe;
      return p->second.value;
    }
  }
  if (source) *source = Source::kSchemaDefault;
  return s->default_value;
}

std::vector<std::string> Store::ApplyProbes(std::vector<ProbeResult> results) {
  std::vector<std::string> refused;
  for (auto& p : results) {
    const Schema* s = SchemaOf(p.name);
    if (s == nullptr) {
      refused.push_back(std::string{"probe: the schema has no setting "} + NameOf(p.name));
      continue;
    }
    if (!s->probe_supplied) {
      // The AD-18 case, spelled out in the refusal, because a launcher that trips it is one line from
      // sandboxing every app because a probe happened to run.
      refused.push_back(std::string{"probe: "} + s->toml_key +
                        " is not probe-supplied — only the user chooses it, and T1 is never a silent "
                        "fallback (AD-18)");
      continue;
    }
    std::string err;
    if (!Validate(*s, p.value, &err)) {
      refused.push_back("probe: " + err);
      continue;
    }
    if (probes_.count(p.name)) {
      refused.push_back(std::string{"probe: "} + s->toml_key +
                        " was supplied twice; the later answer won (" + p.evidence + ")");
    }
    const Name name = p.name;
    probes_[name] = std::move(p);
  }
  return refused;
}

std::vector<std::string> Store::LoadTomlRow(const std::string& text, const std::string& row) {
  std::vector<std::string> reports;
  const auto report = [&](unsigned line_no, const std::string& what) {
    reports.push_back("line " + std::to_string(line_no) + ": " + what);
  };

  std::string section;
  bool have_section = false;
  std::map<Name, unsigned> seen;

  std::size_t start = 0;
  unsigned line_no = 0;
  while (start <= text.size()) {
    const std::size_t nl = text.find('\n', start);
    const std::string line =
        Trim(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
    start = nl == std::string::npos ? text.size() + 1 : nl + 1;
    ++line_no;
    if (line.empty() || line[0] == '#') continue;  // whole-line comments only, by design

    if (line.front() == '[') {
      const std::size_t close = line.rfind(']');
      if (close == std::string::npos || close < 1) {
        report(line_no, "unterminated section header: " + line);
        continue;
      }
      std::string name = Trim(line.substr(1, close - 1));
      if (name.size() > 1 && name.front() == '"') {
        std::size_t i = 0;
        std::string quoted;
        if (!ReadQuoted(name, i, &quoted)) {
          report(line_no, "unterminated quote in the section header: " + line);
          continue;
        }
        name = std::move(quoted);
      }
      section = std::move(name);
      have_section = true;
      seen.clear();
      continue;
    }

    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) {
      report(line_no, "not a key = value line: " + line);
      continue;
    }
    const std::string key = Trim(line.substr(0, eq));
    const std::string rest = Trim(line.substr(eq + 1));

    const Schema* s = SchemaOfKey(key);
    if (s == nullptr) {
      // Xenia's "wrong TOML section" and every other silent-ignore interface in docs/research/08
      // started at this line, so the report names the keys the schema does accept.
      report(line_no,
             "unknown key '" + key + "' under section '" +
                 (have_section ? section : std::string{"<no section>"}) + "'; the schema accepts " +
                 Join(SchemaKeys(), ", ") + " — not applied");
      continue;
    }
    if (!have_section || section != row) {
      report(line_no, "key '" + key + "' is under section '" +
                          (have_section ? section : std::string{"<no section>"}) + "', not '" + row +
                          "'; not applied");
      continue;
    }
    if (seen.count(s->name)) {
      report(line_no, "'" + key + "' was already set on line " + std::to_string(seen.at(s->name)) +
                          "; the later line wins");
    }

    Value value;
    std::string why;
    if (!ParseValue(rest, &why, &value)) {
      report(line_no, "'" + key + "' — " + why + " (this key wants " + Expected(*s) +
                          "), not applied");
      continue;
    }
    std::string err;
    if (!SetRow(row, s->name, std::move(value), &err)) {
      report(line_no, "'" + key + "' — " + err);
      continue;
    }
    seen[s->name] = line_no;
  }

  unknown_.insert(unknown_.end(), reports.begin(), reports.end());
  return reports;
}

std::string Store::SaveTomlRow(const std::string& row) const {
  std::string out = "# retrobug profile, schema " + std::to_string(kSchemaVersion) + "\n[";
  EscapeInto(out, row);
  out += "]\n";
  const auto r = overrides_.find(row);
  if (r == overrides_.end()) return out;
  for (const auto& s : kSchema) {
    const auto v = r->second.find(static_cast<std::size_t>(s.name));
    if (v == r->second.end()) continue;
    out += std::string{s.toml_key} + " = " + v->second.ToToml() + "\n";
  }
  return out;
}

std::vector<std::string> Store::OverridesOfNothingRow(const std::string& row) const {
  std::vector<std::string> out;
  const auto r = overrides_.find(row);
  if (r == overrides_.end()) return out;
  for (const auto& [idx, value] : r->second) {
    const Schema* s = SchemaOf(static_cast<Name>(idx));
    if (s == nullptr) continue;
    // An override under a name no probe supplied. It still resolves — refusing a user's explicit word
    // would be a worse silence than honouring it — but it is overriding the compiled default, which
    // AD-12 says is not what a profile is for, so C17 gets to say so.
    if (probes_.count(s->name) == 0) {
      out.push_back(std::string{s->toml_key} + "=" + value.ToToml());
    }
  }
  return out;
}

void Store::EchoRow(diag::Recorder& rec, const std::string& stage, const std::string& row,
                    const std::string& evidence) const {
  std::string requested;
  const auto r = overrides_.find(row);
  if (r != overrides_.end()) {
    for (const auto& [idx, value] : r->second) {
      const Schema* s = SchemaOf(static_cast<Name>(idx));
      if (!requested.empty()) requested += ", ";
      requested += std::string{s == nullptr ? "?" : s->toml_key} + "=" + value.ToToml();
    }
  }
  if (requested.empty()) requested = "no overrides";

  std::string active;
  for (const auto& s : kSchema) {
    Source src = Source::kNone;
    const auto v = Effective(row, s.name, &src);
    if (!active.empty()) active += ", ";
    active += std::string{s.toml_key} + "=" + (v ? v->ToToml() : std::string{"<unset>"}) + " (" +
              SourceName(src) + ")";
  }

  const auto inert = OverridesOfNothingRow(row);
  std::string note = evidence + "; schema v" + std::to_string(kSchemaVersion);
  if (!inert.empty()) {
    note += "; overriding the compiled default, nothing probed beneath them: " + Join(inert, ", ");
  }
  if (!unknown_.empty()) {
    note += "; " + std::to_string(unknown_.size()) + " refused line(s) in this store, first: " +
            unknown_.front();
  }
  rec.Require(diag::Field::kProfileApplied);
  rec.Record(diag::Field::kProfileApplied, stage, std::move(requested), std::move(active),
             inert.empty() && unknown_.empty() ? diag::Outcome::kActive : diag::Outcome::kInert,
             std::move(note));
}

std::vector<Name> Store::names() const {
  std::vector<Name> out;
  for (const auto& s : kSchema) out.push_back(s.name);
  return out;
}

bool Store::has_probe(Name n) const { return probes_.count(n) != 0; }

const ProbeResult* Store::probe(Name n) const {
  const auto p = probes_.find(n);
  return p == probes_.end() ? nullptr : &p->second;
}

}  // namespace rb::profile
