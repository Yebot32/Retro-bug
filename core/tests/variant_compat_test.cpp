/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the boost::variant shim (compat/boost/variant.hpp).
 *
 * Why this exists separately from the interval shim's tests: the variant shim's job is to keep
 * Dynarmic's A32 terminals compiling *as written*, and the property that decides whether it does is
 * not visible in a build log. Dynarmic's ir/terminal.h declares self-referential alternatives through
 * recursive_wrapper, and its AArch64 back end then dispatches on them with an overload set that has
 * one function per alternative taking it **by value** plus one taking the whole variant. Whether that
 * resolves comes down to what `apply_visitor` hands the visitor: Boost unwraps, a bare std::visit
 * does not, and unwrapped-vs-wrapped is the difference between an exact match and an ambiguous call.
 * Three of the ten alternatives — the three wrapped ones — were affected, which is what made the
 * failure look like a puzzle rather than a missing feature (docs/30-m0-notes.md §Step 5b).
 *
 * Nothing here is RetroBug's own code path: only external/dynarmic includes this header, and this is
 * the assertion that the stand-in is the same shape as the thing it replaces.
 */
#include <boost/variant.hpp>

#include <cstdio>
#include <string>
#include <type_traits>

namespace {

int failures = 0;
int checks = 0;

void fail(const char* what, int line, const std::string& detail = {}) {
  ++failures;
  std::printf("FAIL  %s  (line %d)%s%s\n", what, line, detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK(...)                 \
  do {                             \
    ++checks;                      \
    if (!(__VA_ARGS__)) fail(#__VA_ARGS__, __LINE__); \
  } while (0)

#define CHECK_CTX(cond, detail)              \
  do {                                       \
    ++checks;                                \
    if (!(cond)) fail(#cond, __LINE__, detail); \
  } while (0)

/**
 * The declaration order dynarmic uses, reproduced exactly: the wrapped types are incomplete at the
 * point the alias is formed, so the only way the alias can compile is if the wrapper defers the
 * requirement (compat/boost/variant.hpp holds a unique_ptr for this reason; aliasing to T does not).
 */
namespace Term {

struct If;
struct CheckBit;
struct CheckHalt;

struct Invalid {};
struct Interpret {};
struct LinkBlock {};

using Terminal = boost::variant<Invalid, Interpret, LinkBlock, boost::recursive_wrapper<If>,
                                boost::recursive_wrapper<CheckBit>,
                                boost::recursive_wrapper<CheckHalt>>;

struct If {
  Terminal then_;
  Terminal else_;
  int id = 0;
};
struct CheckBit {
  Terminal then_;
  Terminal else_;
  int flag = 0;
};
struct CheckHalt {
  Terminal else_;
};

}  // namespace Term

// ------------------------------------------------------------------ the dispatch property

/// What a visitor actually receives for each alternative. If the shim forwards the wrapper, the
/// first assertion below stops being true and the overload set after it stops compiling.
void visitor_receives_the_unwrapped_value() {
  Term::Terminal wrapped{Term::If{}};
  Term::Terminal plain{Term::Interpret{}};

  std::string seen;
  boost::apply_visitor(
      [&](const auto& x) {
        using T = std::remove_cvref_t<decltype(x)>;
        if constexpr (std::is_same_v<T, boost::recursive_wrapper<Term::If>>) {
          seen = "wrapper";
        } else if constexpr (std::is_same_v<T, Term::If>) {
          seen = "If";
        } else if constexpr (std::is_same_v<T, Term::Interpret>) {
          seen = "Interpret";
        } else {
          seen = "other";
        }
      },
      wrapped);
  CHECK_CTX(seen == "If", "visitor saw " + seen + " for a recursive_wrapper<If> alternative");

  seen.clear();
  boost::apply_visitor(
      [&](const auto& x) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(x)>, Term::Interpret>) seen = "Interpret";
      },
      plain);
  CHECK_CTX(seen == "Interpret", "unwrapped alternatives must still reach the visitor, saw " + seen);

  // const and mutable views of the same alternative: Boost gives a const variant's visitor a const
  // reference, and dynarmic's by-value parameters rely on being able to read either.
  const Term::Terminal& as_const = wrapped;
  CHECK(std::is_same_v<std::remove_cvref_t<decltype(std::declval<const Term::If&>())>, Term::If>);
  boost::apply_visitor([&](const auto& x) { CHECK(std::is_const_v<std::remove_reference_t<decltype(x)>>); },
                       as_const);
}

/// Dynarmic's exact overload shape: one per alternative by value, plus one taking the whole variant
/// that re-dispatches. With a wrapper reaching the lambda, the call inside the generic lambda is
/// ambiguous for the three wrapped alternatives — the compile error that blocked the arm64 lane.
std::string emitted;

void Emit(Term::Invalid) { emitted += "invalid,"; }
void Emit(Term::Interpret) { emitted += "interpret,"; }
void Emit(Term::LinkBlock) { emitted += "linkblock,"; }
void Emit(Term::If) { emitted += "if,"; }
void Emit(Term::CheckBit) { emitted += "checkbit,"; }
void Emit(Term::CheckHalt) { emitted += "checkhalt,"; }

void Emit(Term::Terminal terminal) {
  boost::apply_visitor([&](const auto& t) { Emit(t); }, terminal);
}

void terminal_dispatch_selects_the_right_overload() {
  const Term::Terminal cases[]{
      Term::Terminal{Term::Invalid{}},   Term::Terminal{Term::Interpret{}},
      Term::Terminal{Term::LinkBlock{}}, Term::Terminal{Term::If{}},
      Term::Terminal{Term::CheckBit{}},  Term::Terminal{Term::CheckHalt{}},
  };
  emitted.clear();
  for (const auto& t : cases) Emit(t);
  CHECK_CTX(emitted == "invalid,interpret,linkblock,if,checkbit,checkhalt,", "emitted: " + emitted);
}

// ------------------------------------------------------------------ value semantics

void wrapped_values_are_deep_copied() {
  // recursive_wrapper owns through a unique_ptr, so a copied variant must not share the held object
  // and must not alias it either: two variants naming the same If would otherwise be a use-after-
  // free waiting for whichever one dies first.
  Term::If nested;
  nested.id = 7;
  Term::Terminal a{std::move(nested)};
  Term::Terminal b{a};

  int seen_a = 0, seen_b = 0;
  boost::apply_visitor([&](const auto& t) { using T = std::remove_cvref_t<decltype(t)>;
                                             if constexpr (std::is_same_v<T, Term::If>) seen_a = t.id; },
                       a);
  boost::apply_visitor([&](const auto& t) { using T = std::remove_cvref_t<decltype(t)>;
                                             if constexpr (std::is_same_v<T, Term::If>) seen_b = t.id; },
                       b);
  CHECK(seen_a == 7);
  CHECK(seen_b == 7);

  // Assigning the bare value through the wrapper's converting assignment, then reading it back.
  b = Term::If{{}, {}, 9};
  boost::apply_visitor(
      [&](const auto& t) {
        using T = std::remove_cvref_t<decltype(t)>;
        if constexpr (std::is_same_v<T, Term::If>) CHECK_CTX(t.id == 9, "copied id " + std::to_string(t.id));
      },
      b);
}

void which_and_get_agree_with_the_alternatives() {
  Term::Terminal t{Term::CheckBit{}};
  CHECK(t.which() == 4);  // Invalid, Interpret, LinkBlock, If, CheckBit
  // boost::get<T> takes the *unwrapped* type where dynarmic asks for it — its one use on a terminal
  // is boost::get<IR::Term::Interpret>(&terminal) in a64_merge_interpret_blocks.cpp.
  CHECK(boost::get<Term::Interpret>(&t) == nullptr);
  Term::Terminal i{Term::Interpret{}};
  CHECK(boost::get<Term::Interpret>(&i) != nullptr);
  CHECK(i.which() == 1);
}

}  // namespace

int main() {
  visitor_receives_the_unwrapped_value();
  terminal_dispatch_selects_the_right_overload();
  wrapped_values_are_deep_copied();
  which_and_get_agree_with_the_alternatives();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
