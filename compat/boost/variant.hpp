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
  recursive_wrapper() = default;
  recursive_wrapper(const T& value) : held_(std::make_unique<T>(value)) {}
  recursive_wrapper(T&& value) : held_(std::make_unique<T>(std::move(value))) {}
  recursive_wrapper(const recursive_wrapper& other)
      : held_(other.held_ ? std::make_unique<T>(*other.held_) : nullptr) {}
  recursive_wrapper(recursive_wrapper&&) noexcept = default;

  recursive_wrapper& operator=(const recursive_wrapper& other) {
    held_ = other.held_ ? std::make_unique<T>(*other.held_) : nullptr;
    return *this;
  }
  recursive_wrapper& operator=(recursive_wrapper&&) noexcept = default;
  recursive_wrapper& operator=(const T& value) {
    held_ = std::make_unique<T>(value);
    return *this;
  }

  operator const T&() const { return *held_; }
  operator T&() { return *held_; }
  const T* get() const { return held_.get(); }

  friend bool operator==(const recursive_wrapper& a, const recursive_wrapper& b) {
    if (!a.held_ || !b.held_) {
      return a.held_ == b.held_;
    }
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

/// Present only because dynarmic's Common::detail::VariantVisitor derives from it; the
/// inherited `Lambda::operator()` is what std::visit actually calls.
template <typename ResultT>
struct static_visitor {
  using result_type = ResultT;
};

template <typename Visitor, typename... Alt>
decltype(auto) apply_visitor(Visitor&& visitor, variant<Alt...>& v) {
  return std::visit(std::forward<Visitor>(visitor), static_cast<std::variant<Alt...>&>(v));
}

template <typename Visitor, typename... Alt>
decltype(auto) apply_visitor(Visitor&& visitor, const variant<Alt...>& v) {
  return std::visit(std::forward<Visitor>(visitor), static_cast<const std::variant<Alt...>&>(v));
}

template <typename Visitor, typename... Alt>
decltype(auto) apply_visitor(Visitor&& visitor, variant<Alt...>&& v) {
  return std::visit(std::forward<Visitor>(visitor), static_cast<std::variant<Alt...>&&>(v));
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
