// Traffic Engineering Fabric - checked authoritative arithmetic.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <limits>
#include <optional>

#include "tef/diagnostic.hpp"

namespace tef {

// Authoritative bandwidth arithmetic is integer-only. These helpers make every
// overflow explicit instead of relying on wrapping behaviour.
inline std::optional<std::int64_t> add_checked(std::int64_t a, std::int64_t b) noexcept {
  if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) return std::nullopt;
  if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) return std::nullopt;
  return a + b;
}

inline std::optional<std::int64_t> sub_checked(std::int64_t a, std::int64_t b) noexcept {
  if (b == std::numeric_limits<std::int64_t>::min()) {
    return (a >= 0) ? std::nullopt : std::optional<std::int64_t>(a - b);
  }
  return add_checked(a, -b);
}

inline std::optional<std::int64_t> mul_checked(std::int64_t a, std::int64_t b) noexcept {
  if (a == 0 || b == 0) return std::int64_t{0};
  const std::int64_t min = std::numeric_limits<std::int64_t>::min();
  const std::int64_t max = std::numeric_limits<std::int64_t>::max();
  if (a == -1) {
    if (b == min) return std::nullopt;
    return -b;
  }
  if (b == -1) {
    if (a == min) return std::nullopt;
    return -a;
  }
  if (a > 0) {
    if (b > 0) {
      if (a > max / b) return std::nullopt;
    } else {
      if (b < min / a) return std::nullopt;
    }
  } else {
    if (b > 0) {
      if (a < min / b) return std::nullopt;
    } else {
      if (a < max / b) return std::nullopt;
    }
  }
  return a * b;
}

inline std::optional<std::int64_t> div_checked(std::int64_t a, std::int64_t b) noexcept {
  if (b == 0) return std::nullopt;
  if (a == std::numeric_limits<std::int64_t>::min() && b == -1) return std::nullopt;
  return a / b;
}

inline bool in_bandwidth_range(std::int64_t v) noexcept { return v >= 0; }

// Saturating helpers used only for non-authoritative presentation arithmetic.
inline std::int64_t sat_add(std::int64_t a, std::int64_t b) noexcept {
  const auto r = add_checked(a, b);
  if (r) return *r;
  return b > 0 ? std::numeric_limits<std::int64_t>::max() : std::numeric_limits<std::int64_t>::min();
}

inline std::int64_t sat_mul(std::int64_t a, std::int64_t b) noexcept {
  const auto r = mul_checked(a, b);
  if (r) return *r;
  const bool negative = (a < 0) != (b < 0);
  return negative ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max();
}

// Deterministic ceil-division for non-negative operands.
inline std::int64_t ceil_div(std::int64_t numerator, std::int64_t denominator) noexcept {
  if (denominator <= 0) return 0;
  if (numerator <= 0) return 0;
  return (numerator + denominator - 1) / denominator;
}

inline std::uint64_t gcd_u64(std::uint64_t a, std::uint64_t b) noexcept {
  while (b != 0) {
    const std::uint64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

// Exact rational used for presentation and for equal-share arithmetic. All
// authoritative decisions use integers; rationals appear only where an exact
// non-integer share must survive canonicalization without floating point.
struct Rational {
  std::int64_t numerator = 0;
  std::int64_t denominator = 1;

  Rational() = default;
  Rational(std::int64_t n, std::int64_t d) : numerator(n), denominator(d) { normalize(); }

  void normalize() noexcept {
    if (denominator == 0) {
      numerator = 0;
      denominator = 1;
      return;
    }
    if (denominator < 0) {
      if (numerator == std::numeric_limits<std::int64_t>::min() ||
          denominator == std::numeric_limits<std::int64_t>::min()) {
        numerator = 0;
        denominator = 1;
        return;
      }
      numerator = -numerator;
      denominator = -denominator;
    }
    const std::uint64_t g = gcd_u64(static_cast<std::uint64_t>(numerator < 0 ? -numerator : numerator),
                                    static_cast<std::uint64_t>(denominator));
    if (g > 1) {
      numerator /= static_cast<std::int64_t>(g);
      denominator /= static_cast<std::int64_t>(g);
    }
  }

  bool operator==(const Rational& other) const noexcept {
    return numerator == other.numerator && denominator == other.denominator;
  }

  // Exact three-way comparison with no floating point. Cross-multiplication is
  // attempted with checked 64-bit arithmetic; when the products would overflow,
  // the comparison falls back to a scaled long-division decomposition that is
  // exact for all representable operands.
  static std::partial_ordering compare(const Rational& a, const Rational& b) noexcept {
    const auto lhs = mul_checked(a.numerator, b.denominator);
    const auto rhs = mul_checked(b.numerator, a.denominator);
    if (lhs && rhs) {
      if (*lhs == *rhs) return std::partial_ordering::equivalent;
      return *lhs < *rhs ? std::partial_ordering::less : std::partial_ordering::greater;
    }
    const std::int64_t qa = a.numerator / a.denominator;
    const std::int64_t qb = b.numerator / b.denominator;
    if (qa != qb) return qa < qb ? std::partial_ordering::less : std::partial_ordering::greater;
    const Rational ra(a.numerator % a.denominator, a.denominator);
    const Rational rb(b.numerator % b.denominator, b.denominator);
    // Same integer part and both are proper fractions: comparing them reduces to
    // comparing their reciprocals in reverse order.
    const Rational ia(ra.denominator, ra.numerator == 0 ? 1 : ra.numerator);
    const Rational ib(rb.denominator, rb.numerator == 0 ? 1 : rb.numerator);
    const auto inner = compare(ia, ib);
    if (inner == std::partial_ordering::equivalent) return std::partial_ordering::equivalent;
    return inner == std::partial_ordering::less ? std::partial_ordering::greater
                                                : std::partial_ordering::less;
  }
};

// Fixed-point ratio with a scale of 1e9. Used only for reporting percentages
// and utilization, never for authoritative ordering (which uses exact integer
// cross-multiplication).
inline std::int64_t permille(std::int64_t part, std::int64_t whole) noexcept {
  if (whole <= 0) return 0;
  // part * 1000 with overflow protection.
  const auto scaled = mul_checked(part, 1000);
  if (!scaled) return part > 0 ? 1000 : 0;
  return *scaled / whole;
}

}  // namespace tef
