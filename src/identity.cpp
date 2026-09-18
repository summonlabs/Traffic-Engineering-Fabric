// Traffic Engineering Fabric - identity implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "tef/identity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>

#include "tef/limits.hpp"

namespace tef {
namespace {

bool is_name_char(char c) noexcept {
  const unsigned char u = static_cast<unsigned char>(c);
  if (u >= 'A' && u <= 'Z') return true;
  if (u >= 'a' && u <= 'z') return true;
  if (u >= '0' && u <= '9') return true;
  switch (u) {
    case '.': case '_': case '-': case ':': case '/': case '+':
      return true;
    default:
      return false;
  }
}

}  // namespace

bool is_valid_name(std::string_view value) noexcept {
  if (value.empty()) return false;
  if (value.size() > Limits::max_name_length) return false;

  // Reject path-hostile values outright: identities never become file names,
  // but defence in depth costs nothing here.
  if (value == "." || value == "..") return false;

  char previous = '\0';
  for (char c : value) {
    if (!is_name_char(c)) return false;
    previous = c;
  }
  // Leading and trailing separators are rejected so that canonical spelling is
  // unique and sorting is stable.
  if (value.front() == '.' || value.front() == '-' || value.front() == ':' || value.front() == '/' ||
      value.front() == '+' || value.front() == '_') {
    return false;
  }
  if (previous == '.' || previous == '-' || previous == ':' || previous == '/' || previous == '+' ||
      previous == '_') {
    return false;
  }
  return true;
}

std::string BootId::hex() const {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (std::uint8_t b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0Fu]);
  }
  return out;
}

BootId derive_boot_id(std::uint64_t hi, std::uint64_t lo) noexcept {
  BootId id;
  for (int i = 0; i < 8; ++i) {
    id.bytes[static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>(hi >> (8 * (7 - static_cast<unsigned>(i))));
    id.bytes[static_cast<std::size_t>(8 + i)] =
        static_cast<std::uint8_t>(lo >> (8 * (7 - static_cast<unsigned>(i))));
  }
  if (!id.valid()) {
    // Never hand out an all-zero (invalid) boot identity.
    id.bytes[15] = 1;
  }
  return id;
}

BootId random_boot_id() noexcept {
  std::random_device device;
  std::uniform_int_distribution<std::uint64_t> distribution;
  const std::uint64_t hi = (static_cast<std::uint64_t>(device()) << 32) ^ distribution(device);
  const std::uint64_t lo = (static_cast<std::uint64_t>(device()) << 32) ^ distribution(device);
  return derive_boot_id(hi, lo);
}

}  // namespace tef
