// Traffic Engineering Fabric - strongly typed identities and generations.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "tef/limits.hpp"

namespace tef {

// Canonical entity names. The accepted alphabet is deliberately narrow so that
// byte-wise ordering is total, locale independent, and stable across platforms
// and compiler versions. Names are never used as filesystem paths.
bool is_valid_name(std::string_view value) noexcept;

// Strongly typed identity. Two identities with different tags are different
// types and cannot be compared or interchanged.
template <class Tag>
class Id {
 public:
  using tag = Tag;

  Id() = default;

  static std::optional<Id> parse(std::string_view value) {
    if (!is_valid_name(value)) return std::nullopt;
    Id id;
    id.value_.assign(value);
    return id;
  }

  // Decoding path: the byte string has already been accepted by the reader's
  // structural checks, but it must still satisfy name canonicality.
  static std::optional<Id> from_raw(std::string_view value) { return parse(value); }

  bool valid() const noexcept { return !value_.empty(); }
  const std::string& str() const noexcept { return value_; }

  friend bool operator==(const Id& a, const Id& b) noexcept { return a.value_ == b.value_; }
  friend std::strong_ordering operator<=>(const Id& a, const Id& b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  std::string value_;
};

// Monotone generation counter. Zero is reserved for "absent / unknown" and is
// never a legal authoritative generation: an unset generation must be rejected,
// never silently treated as current.
template <class Tag>
class Gen {
 public:
  using tag = Tag;
  using value_type = std::uint64_t;

  Gen() = default;

  static constexpr Gen first() noexcept {
    Gen g;
    g.value_ = 1;
    return g;
  }

  static std::optional<Gen> parse(std::uint64_t value) noexcept {
    if (value == 0) return std::nullopt;
    Gen g;
    g.value_ = value;
    return g;
  }

  bool valid() const noexcept { return value_ != 0; }
  std::uint64_t value() const noexcept { return value_; }

  // Advances the generation. Saturation is explicit: an exhausted counter
  // becomes invalid rather than wrapping to a generation that already existed.
  Gen next() const noexcept {
    Gen g;
    if (value_ != 0 && value_ != UINT64_MAX) g.value_ = value_ + 1;
    return g;
  }

  friend bool operator==(const Gen&, const Gen&) noexcept = default;
  friend std::strong_ordering operator<=>(const Gen&, const Gen&) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

// A 128-bit boot identity. Every process incarnation has a fresh boot id; a
// boot id is never reused, so records written by a dead incarnation can never be
// mistaken for the live one.
struct BootId {
  std::array<std::uint8_t, 16> bytes{};

  bool valid() const noexcept {
    for (std::uint8_t b : bytes) {
      if (b != 0) return true;
    }
    return false;
  }

  std::string hex() const;

  friend bool operator==(const BootId&, const BootId&) noexcept = default;
  friend std::strong_ordering operator<=>(const BootId&, const BootId&) noexcept = default;
};

// Deterministic boot-id derivation for tests and for reproducible runs. In
// production the coordinator derives boot ids from OS entropy; the derivation
// helper exists so that fixtures can be reproduced exactly.
BootId derive_boot_id(std::uint64_t hi, std::uint64_t lo) noexcept;
BootId random_boot_id() noexcept;

}  // namespace tef
