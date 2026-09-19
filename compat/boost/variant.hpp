/*
 * RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
 *
 * Header-only stand-in for the Boost names dynarmic uses from <boost/variant.hpp>:
 *   boost::variant, boost::apply_visitor, boost::static_visitor, boost::get,
 *   boost::recursive_wrapper
 * (the icl containers live in icl/). std::variant covers all of them; the API shape belongs to
 * Boost, which is BSL-1.0, and this implementation is RetroBug's.
 *
 * See ../README.md for the deletion condition. Nothing in RetroBug's own code includes this —
 * only external/dynarmic does, which is what keeps the surface finite.
 */
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace boost {

/**
 * Holds a self-referential alternative whose type is still incomplete where the variant is
 * declared — which is exactly what dynarmic does: ir/terminal.h forward-declares If, CheckBit
 * and CheckHalt above the `Terminal` alias.
 *
 * Aliasing this to `T` is not enough: std::variant computes its traits at the alias, so `T`
 * would have to be complete there. Storing a pointer defers that requirement, as Boost does.
 * Visitors keep compiling because they reach the value through the conversion operator, so
 * dynarmic's `EmitA32Terminal(..., const Term::If&, ...)` call sites are untouched. Direct
 * member access on a wrapper (t.then) would not compile, and dynarmic never does it.
 */
template <typename T>
class recursive_wrapper {
 public:
  /// Always holds a real `T`, as Boost does: `operator const T&()` dereferences this unconditionally,
  /// so a null `held_` would be undefined behaviour reachable through a perfectly legal
  /// default-construction. Only instantiated if someone actually default-constructs a wrapper, which
  /// dynarmic does not — its variant's first alternative is the unwrapped `Invalid`.
  recursive_wrapper() : held_(std::make_unique<T>()) {}
  recursive_wrapper(const T& value) : held_(std::make_unique<T>(value)) {}
  recursive_wrapper(T&& value) : held_(std::make_unique<T>(std::move(value))) {}
  recursive_wrapper(const recursive_wrapper& other) : held_(std::make_unique<T>(*other.held_)) {}
  recursive_wrapper(recursive_wrapper&&) noexcept = default;

  recursive_wrapper& operator=(const recursive_wrapper& other) {
    held_ = std::make_unique<T>(*other.held_);
    return *this;
  }
  recursive_wrapper& operator=(recursive_wrapper&&) noexcept = default;
  recursive_wrapper& operator=(const T& value) {
    held_ = std::make_unique<T>(value);
    return *this;
  }

  /// The conversion Boost provides, and the reason dynarmic's `EmitA32Terminal(..., IR::Term::If
  /// terminal, ...)` by-value parameters keep compiling. Only the const form exists, because a
  /// non-const `operator T&()` would give the wrapper two equally-ranked user-defined conversions
  /// into a `const T&` parameter. Reachable values that need mutation go through `value()` below,
  /// which is also what apply_visitor uses rather than relying on this conversion.
  operator const T&() const { return *held_; }
  /// Direct access, in both const-nesses. `apply_visitor` below uses this to hand the visitor the
  /// wrapped value rather than the wrapper, which is what Boost's visitor dispatch does.
  T& value() { return *held_; }
  const T& value() const { return *held_; }
  const T* get() const { return held_.get(); }

  friend bool operator==(const recursive_wrapper& a, const recursive_wrapper& b) {
    return *a.held_ == *b.held_;
  }

 private:
  std::unique_ptr<T> held_;
};

/**
 * std::variant plus Boost's `which()`.
 *
 * A distinct type is unavoidable because `which()` is member syntax and cannot be added to
 * std::variant. Deriving rather than wrapping keeps the alternative list, indices and value
 * operations identical; the cost is that std::visit cannot deduce through a derived type, so
 * apply_visitor below down-casts explicitly rather than forwarding straight through.
 */
template <typename... Types>
class variant : public std::variant<Types...> {
 public:
  using base = std::variant<Types...>;
  using base::base;
  using base::operator=;

  std::size_t which() const { return this->index(); }
};

namespace detail {

template <typename T>
struct is_recursive_wrapper : std::false_type {};
template <typename T>
struct is_recursive_wrapper<recursive_wrapper<T>> : std::true_type {};

/**
 * Invoke a visitor on one alternative, unwrapping a `recursive_wrapper` first.
 *
 * This is not a convenience. Boost's visitor dispatch tests `is_recursive_wrapper` on each
 * alternative and forwards the *held* value, so a visitor's `operator()(const If&)` overload is what
 * gets selected. Forwarding the wrapper instead — which is what a bare `std::visit` does — leaves
 * dynarmic's terminals ambiguous: `EmitA32Terminal` has one overload per alternative taking it by
 * value plus one taking the whole `Terminal` variant, and a `recursive_wrapper<CheckHalt>` argument
 * reaches either through exactly one user-defined conversion, so neither outranks the other and
 * every wrapped alternative fails to compile (`If`, `CheckBit`, `CheckHalt` — the three wrapped
 * ones — and nothing else, which is what identified it). Unwrapped, the specific overload is an
 * exact match and the variant overload is not a candidate at all.
 */
template <typename Visitor, typename Alternative>
decltype(auto) invoke_visitor(Visitor&& visitor, Alternative&& alt) {
  if constexpr (is_recursive_wrapper<std::remove_cvref_t<Alternative>>::value) {
    return static_cast<Visitor&&>(visitor)(alt.value());
  } else {
    return static_cast<Visitor&&>(visitor)(static_cast<Alternative&&>(alt));
  }
}

}  // namespace detail

/// Present only because dynarmic's Common::detail::VariantVisitor derives from it; the
/// inherited `Lambda::operator()` is what std::visit actually calls.
template <typename ResultT>
struct static_visitor {
  using result_type = ResultT;
};

/// The three overloads exist so the down-cast to the `std::variant` base keeps the caller's
/// value-category and const-ness, which is what decides whether a visitor sees `T&` or `const T&`.
template <typename Visitor, typename... Alt>
decltype(auto) apply_visitor(Visitor&& visitor, variant<Alt...>& v) {
  return std::visit(
      [&](auto&& a) -> decltype(auto) { return detail::invoke_visitor(visitor, a); },
      static_cast<std::variant<Alt...>&>(v));
}

template <typename Visitor, typename... Alt>
decltype(auto) apply_visitor(Visitor&& visitor, const variant<Alt...>& v) {
  return std::visit(
      [&](auto&& a) -> decltype(auto) { return detail::invoke_visitor(visitor, a); },
      static_cast<const std::variant<Alt...>&>(v));
}

template <typename Visitor, typename... Alt>
decltype(auto) apply_visitor(Visitor&& visitor, variant<Alt...>&& v) {
  return std::visit(
      [&](auto&& a) -> decltype(auto) { return detail::invoke_visitor(visitor, std::move(a)); },
      static_cast<std::variant<Alt...>&&>(v));
}

/// `boost::get<T>(&v)` — pointer-returning, null when the active alternative is not T.
template <typename T, typename... Alt>
const T* get(const variant<Alt...>* v) {
  return std::get_if<T>(static_cast<const std::variant<Alt...>*>(v));
}

template <typename T, typename... Alt>
T* get(variant<Alt...>* v) {
  return std::get_if<T>(static_cast<std::variant<Alt...>*>(v));
}

}  // namespace boost
