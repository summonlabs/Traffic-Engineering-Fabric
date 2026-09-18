// Traffic Engineering Fabric - SHA-256 and CRC-32C.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "tef/digest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace tef {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline std::uint32_t rotr(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32u - count));
}

constexpr std::uint32_t crc32c_entry(std::uint32_t seed) noexcept {
  std::uint32_t crc = seed;
  for (int bit = 0; bit < 8; ++bit) {
    crc = (crc >> 1) ^ ((crc & 1u) != 0u ? 0x82F63B78u : 0u);
  }
  return crc;
}

constexpr std::array<std::uint32_t, 256> make_crc32c_table() {
  std::array<std::uint32_t, 256> table{};
  std::uint32_t seed = 0;
  for (auto& entry : table) {
    entry = crc32c_entry(seed);
    ++seed;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

constexpr std::array<char, 16> kHexDigits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                            '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string Digest::hex() const {
  std::string out;
  out.reserve(64);
  for (std::uint8_t b : bytes) {
    out.push_back(kHexDigits[static_cast<std::size_t>(b >> 4)]);
    out.push_back(kHexDigits[static_cast<std::size_t>(b & 0x0Fu)]);
  }
  return out;
}

std::optional<Digest> Digest::from_hex(std::string_view text) {
  if (text.size() != 64) return std::nullopt;
  Digest out;
  for (std::size_t i = 0; i < 32; ++i) {
    const int high = hex_value(text[i * 2]);
    const int low = hex_value(text[i * 2 + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    out.bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return out;
}

Sha256::Sha256() {
  state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
}

void Sha256::compress(const std::uint8_t block[64]) noexcept {
  std::uint32_t w[64];
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::byte> data) noexcept {
  if (finished_) return;
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t size = data.size();
  total_bytes_ += size;

  if (buffered_ > 0) {
    const std::size_t needed = 64 - buffered_;
    const std::size_t take = size < needed ? size : needed;
    std::memcpy(buffer_.data() + buffered_, p, take);
    buffered_ += take;
    p += take;
    size -= take;
    if (buffered_ == 64) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }

  while (size >= 64) {
    compress(p);
    p += 64;
    size -= 64;
  }

  if (size > 0) {
    std::memcpy(buffer_.data() + buffered_, p, size);
    buffered_ += size;
  }
}

void Sha256::update(std::string_view data) noexcept {
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

Digest Sha256::finish() noexcept {
  Digest out;
  if (!finished_) {
    const std::uint64_t bit_length = total_bytes_ * 8u;
    const std::uint8_t padding = 0x80;
    update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&padding), 1));
    const std::uint8_t zero = 0;
    while (buffered_ != 56) {
      update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&zero), 1));
    }
    std::uint8_t length_bytes[8];
    for (int i = 0; i < 8; ++i) {
      length_bytes[i] = static_cast<std::uint8_t>(bit_length >> (8 * (7 - i)));
    }
    update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(length_bytes), 8));
    finished_ = true;
  }
  for (std::size_t i = 0; i < 8; ++i) {
    out.bytes[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24);
    out.bytes[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
    out.bytes[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
    out.bytes[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
  }
  return out;
}

Digest Sha256::hash(std::span<const std::byte> data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

Digest Sha256::hash(std::string_view data) noexcept {
  return hash(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::byte b : data) {
    crc = kCrc32cTable[(crc ^ static_cast<std::uint8_t>(b)) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

Digest digest_with_domain(std::string_view domain, std::span<const std::byte> payload) noexcept {
  Sha256 hasher;
  const std::uint32_t length = static_cast<std::uint32_t>(domain.size());
  std::uint8_t header[4];
  for (int i = 0; i < 4; ++i) header[i] = static_cast<std::uint8_t>(length >> (8 * (3 - i)));
  hasher.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(header), 4));
  hasher.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(domain.data()), domain.size()));
  hasher.update(payload);
  return hasher.finish();
}

}  // namespace tef
