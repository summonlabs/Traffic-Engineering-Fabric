// Traffic Engineering Fabric - canonical big-endian binary codec.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tef/digest.hpp"
#include "tef/diagnostic.hpp"
#include "tef/limits.hpp"

namespace tef {

// Canonical byte writer. Every integer is big-endian and fixed width; every
// variable-length field is length-prefixed. No padding, no alignment, no
// host-endian leakage. Two structurally equal values always produce identical
// bytes.
class Writer {
 public:
  Writer() = default;

  void u8(std::uint8_t v) { raw(&v, 1); }

  void u16(std::uint16_t v) {
    const std::uint8_t b[2] = {static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)};
    raw(b, 2);
  }

  void u32(std::uint32_t v) {
    std::uint8_t b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<std::uint8_t>(v >> (8 * (3 - i)));
    raw(b, 4);
  }

  void u64(std::uint64_t v) {
    std::uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<std::uint8_t>(v >> (8 * (7 - i)));
    raw(b, 8);
  }

  void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
  void boolean(bool v) { u8(v ? 1u : 0u); }

  void raw(const void* data, std::size_t size) {
    if (size == 0) return;
    const auto* p = static_cast<const std::byte*>(data);
    bytes_.insert(bytes_.end(), p, p + size);
  }

  void raw(std::span<const std::byte> data) { raw(data.data(), data.size()); }

  void str(std::string_view text) {
    u32(static_cast<std::uint32_t>(text.size()));
    raw(text.data(), text.size());
  }

  void blob(std::span<const std::byte> data) {
    u32(static_cast<std::uint32_t>(data.size()));
    raw(data);
  }

  void digest(const Digest& d) { raw(d.bytes.data(), d.bytes.size()); }

  const std::vector<std::byte>& bytes() const noexcept { return bytes_; }
  std::vector<std::byte>& bytes() noexcept { return bytes_; }
  std::size_t size() const noexcept { return bytes_.size(); }
  bool empty() const noexcept { return bytes_.empty(); }

  Digest finish_digest(std::string_view domain) const noexcept {
    return digest_with_domain(domain, std::span<const std::byte>(bytes_.data(), bytes_.size()));
  }

  std::span<const std::byte> view() const noexcept {
    return std::span<const std::byte>(bytes_.data(), bytes_.size());
  }

 private:
  std::vector<std::byte> bytes_;
};

// Canonical reader with strict bounds checking. Any short read, trailing byte,
// oversized length prefix, or non-canonical value flips the reader into a failed
// state; subsequent reads are no-ops and the failure is reported once.
class Reader {
 public:
  explicit Reader(std::span<const std::byte> data) : data_(data) {}

  bool failed() const noexcept { return failed_; }
  void fail() noexcept { failed_ = true; }
  std::size_t remaining() const noexcept { return failed_ ? 0 : data_.size() - offset_; }
  std::size_t offset() const noexcept { return offset_; }
  bool at_end() const noexcept { return !failed_ && offset_ == data_.size(); }

  bool u8(std::uint8_t& out) {
    if (!ensure(1)) return false;
    out = static_cast<std::uint8_t>(data_[offset_]);
    offset_ += 1;
    return true;
  }

  bool u16(std::uint16_t& out) {
    if (!ensure(2)) return false;
    out = static_cast<std::uint16_t>((static_cast<std::uint16_t>(byte(0)) << 8) |
                                     static_cast<std::uint16_t>(byte(1)));
    offset_ += 2;
    return true;
  }

  bool u32(std::uint32_t& out) {
    if (!ensure(4)) return false;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | byte(static_cast<std::size_t>(i));
    out = v;
    offset_ += 4;
    return true;
  }

  bool u64(std::uint64_t& out) {
    if (!ensure(8)) return false;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | byte(static_cast<std::size_t>(i));
    out = v;
    offset_ += 8;
    return true;
  }

  bool i64(std::int64_t& out) {
    std::uint64_t v = 0;
    if (!u64(v)) return false;
    out = static_cast<std::int64_t>(v);
    return true;
  }

  bool boolean(bool& out) {
    std::uint8_t v = 0;
    if (!u8(v)) return false;
    if (v > 1) {
      failed_ = true;
      return false;
    }
    out = v == 1;
    return true;
  }

  bool raw(void* destination, std::size_t size) {
    if (!ensure(size)) return false;
    std::memcpy(destination, data_.data() + offset_, size);
    offset_ += size;
    return true;
  }

  bool str(std::string& out, std::size_t max_length = Limits::max_text_length) {
    std::uint32_t length = 0;
    if (!u32(length)) return false;
    if (length > max_length) {
      failed_ = true;
      return false;
    }
    if (!ensure(length)) return false;
    out.assign(reinterpret_cast<const char*>(data_.data() + offset_), length);
    offset_ += length;
    return true;
  }

  bool blob(std::vector<std::byte>& out, std::size_t max_length = Limits::max_frame_payload) {
    std::uint32_t length = 0;
    if (!u32(length)) return false;
    if (length > max_length) {
      failed_ = true;
      return false;
    }
    if (!ensure(length)) return false;
    out.assign(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
               data_.begin() + static_cast<std::ptrdiff_t>(offset_ + length));
    offset_ += length;
    return true;
  }

  bool digest(Digest& out) {
    if (!ensure(out.bytes.size())) return false;
    std::memcpy(out.bytes.data(), data_.data() + offset_, out.bytes.size());
    offset_ += out.bytes.size();
    return true;
  }

  bool skip(std::size_t count) { return ensure(count) ? (offset_ += count, true) : false; }

  std::span<const std::byte> rest() const noexcept {
    if (failed_) return {};
    return data_.subspan(offset_);
  }

 private:
  bool ensure(std::size_t count) {
    if (failed_) return false;
    if (count > data_.size() - offset_) {
      failed_ = true;
      return false;
    }
    return true;
  }

  std::uint32_t byte(std::size_t index) const {
    return static_cast<std::uint32_t>(data_[offset_ + index]);
  }

  std::span<const std::byte> data_;
  std::size_t offset_ = 0;
  bool failed_ = false;
};

}  // namespace tef
