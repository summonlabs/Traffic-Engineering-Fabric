// Traffic Engineering Fabric - canonical digests and integrity checks.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tef/diagnostic.hpp"

namespace tef {

// SHA-256 digest. Used for canonical input digests, explanation digests, and
// durable record integrity.
struct Digest {
  std::array<std::uint8_t, 32> bytes{};

  bool is_zero() const noexcept {
    for (std::uint8_t b : bytes) {
      if (b != 0) return false;
    }
    return true;
  }

  std::string hex() const;
  static std::optional<Digest> from_hex(std::string_view text);

  friend bool operator==(const Digest&, const Digest&) noexcept = default;
  friend std::strong_ordering operator<=>(const Digest&, const Digest&) noexcept = default;
};

class Sha256 {
 public:
  Sha256();
  void update(std::span<const std::byte> data) noexcept;
  void update(std::string_view data) noexcept;
  Digest finish() noexcept;

  static Digest hash(std::span<const std::byte> data) noexcept;
  static Digest hash(std::string_view data) noexcept;

 private:
  void compress(const std::uint8_t block[64]) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffered_ = 0;
  bool finished_ = false;
};

// CRC-32C (Castagnoli). Used for frame and record integrity where a fast,
// non-cryptographic check is appropriate.
std::uint32_t crc32c(std::span<const std::byte> data) noexcept;
inline std::uint32_t crc32c(std::string_view data) noexcept {
  return crc32c(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

// Convenience: digest of a byte buffer.
inline Digest digest_of(std::span<const std::byte> data) noexcept { return Sha256::hash(data); }

// Domain separation: every digest is prefixed with a stable label so that two
// structurally identical payloads in different roles cannot collide.
Digest digest_with_domain(std::string_view domain, std::span<const std::byte> payload) noexcept;

}  // namespace tef
