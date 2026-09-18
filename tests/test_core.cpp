// Traffic Engineering Fabric - core primitive proofs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "support/test_framework.hpp"
#include "tef/binary.hpp"
#include "tef/digest.hpp"
#include "tef/identity.hpp"
#include "tef/model.hpp"
#include "tef/numeric.hpp"

using namespace tef;

TEF_TEST(sha256_matches_published_vectors) {
  TEF_CHECK_EQ(Sha256::hash(std::string_view("")).hex(),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  TEF_CHECK_EQ(Sha256::hash(std::string_view("abc")).hex(),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  TEF_CHECK_EQ(
      Sha256::hash(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")).hex(),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  std::string million(1000000, 'a');
  TEF_CHECK_EQ(Sha256::hash(std::string_view(million)).hex(),
               std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEF_TEST(sha256_incremental_matches_oneshot) {
  const std::string payload(5000, 'q');
  Sha256 streaming;
  for (std::size_t offset = 0; offset < payload.size(); offset += 7) {
    const std::size_t count = std::min<std::size_t>(7, payload.size() - offset);
    streaming.update(std::string_view(payload).substr(offset, count));
  }
  TEF_CHECK_EQ(streaming.finish().hex(), Sha256::hash(std::string_view(payload)).hex());
}

TEF_TEST(crc32c_matches_published_vector) {
  TEF_CHECK_EQ(crc32c(std::string_view("123456789")), 0xE3069283u);
  TEF_CHECK_EQ(crc32c(std::string_view("")), 0u);
}

TEF_TEST(digest_hex_round_trip) {
  const Digest digest = Sha256::hash(std::string_view("traffic-engineering-fabric"));
  const auto parsed = Digest::from_hex(digest.hex());
  TEF_CHECK(parsed.has_value());
  TEF_CHECK(*parsed == digest);
  TEF_CHECK(!Digest::from_hex("not-hex").has_value());
  TEF_CHECK(!Digest::from_hex(std::string(63, 'a')).has_value());
  TEF_CHECK(!Digest::from_hex(std::string(64, 'z')).has_value());
}

TEF_TEST(domain_separation_changes_digest) {
  const auto payload = std::span<const std::byte>();
  TEF_CHECK_NE(digest_with_domain("tef.one", payload), digest_with_domain("tef.two", payload));
}

TEF_TEST(name_validation_is_strict) {
  TEF_CHECK(is_valid_name("plan-1"));
  TEF_CHECK(is_valid_name("Path.A:1"));
  TEF_CHECK(!is_valid_name(""));
  TEF_CHECK(!is_valid_name("-leading"));
  TEF_CHECK(!is_valid_name("trailing-"));
  TEF_CHECK(!is_valid_name("has space"));
  TEF_CHECK(!is_valid_name(".."));
  TEF_CHECK(!is_valid_name(std::string(Limits::max_name_length + 1, 'a')));
  TEF_CHECK(is_valid_name(std::string(Limits::max_name_length, 'a')));
  TEF_CHECK(!is_valid_name("semi;colon"));
}

TEF_TEST(generation_zero_is_never_valid) {
  TEF_CHECK(!PlanGeneration{}.valid());
  TEF_CHECK(!PlanGeneration::parse(0).has_value());
  TEF_CHECK(PlanGeneration::parse(1).has_value());
  TEF_CHECK_EQ(PlanGeneration::first().next().value(), 2u);
  const auto exhausted = Gen<PlanGenerationTag>::parse(UINT64_MAX).value().next();
  TEF_CHECK(!exhausted.valid());
}

TEF_TEST(boot_id_derivation_is_deterministic_and_nonzero) {
  const BootId a = derive_boot_id(1, 2);
  const BootId b = derive_boot_id(1, 2);
  TEF_CHECK(a == b);
  TEF_CHECK(a.valid());
  TEF_CHECK_NE(derive_boot_id(0, 0), BootId{});
  TEF_CHECK_EQ(a.hex().size(), 32u);
}

TEF_TEST(checked_arithmetic_reports_overflow) {
  TEF_CHECK(!add_checked(INT64_MAX, 1).has_value());
  TEF_CHECK(!sub_checked(INT64_MIN, 1).has_value());
  TEF_CHECK(!mul_checked(INT64_MAX, 2).has_value());
  TEF_CHECK(!mul_checked(INT64_MIN, -1).has_value());
  TEF_CHECK(!div_checked(1, 0).has_value());
  TEF_CHECK(!div_checked(INT64_MIN, -1).has_value());
  TEF_CHECK_EQ(*add_checked(1, 2), 3);
  TEF_CHECK_EQ(*mul_checked(-3, 4), -12);
  TEF_CHECK_EQ(sat_add(INT64_MAX, 5), INT64_MAX);
  TEF_CHECK_EQ(ceil_div(0, 5), 0);
  TEF_CHECK_EQ(ceil_div(6, 5), 2);
}

TEF_TEST(rational_normalizes_and_compares_exactly) {
  const Rational third(2, 6);
  TEF_CHECK_EQ(third.numerator, 1);
  TEF_CHECK_EQ(third.denominator, 3);
  TEF_CHECK(Rational::compare(Rational(1, 3), Rational(1, 2)) == std::partial_ordering::less);
  TEF_CHECK(Rational::compare(Rational(1, 2), Rational(1, 2)) == std::partial_ordering::equivalent);
  TEF_CHECK(Rational::compare(Rational(7, 3), Rational(9, 4)) == std::partial_ordering::greater);
  TEF_CHECK(Rational::compare(Rational(1, 3), Rational(1000000007, 3000000021)) ==
            std::partial_ordering::equivalent);
}

TEF_TEST(binary_writer_is_big_endian_and_stable) {
  Writer writer;
  writer.u8(0x12);
  writer.u16(0x3456);
  writer.u32(0x789ABCDEu);
  writer.u64(0x0F1E2D3C4B5A6978ull);
  writer.i64(-2);
  writer.boolean(true);
  writer.str("abc");
  const auto& bytes = writer.bytes();
  TEF_CHECK_EQ(bytes.size(), std::size_t{1 + 2 + 4 + 8 + 8 + 1 + 4 + 3});
  TEF_CHECK_EQ(static_cast<int>(bytes[0]), 0x12);
  TEF_CHECK_EQ(static_cast<int>(bytes[1]), 0x34);
  TEF_CHECK_EQ(static_cast<int>(bytes[2]), 0x56);
  TEF_CHECK_EQ(static_cast<int>(bytes[3]), 0x78);
  TEF_CHECK_EQ(static_cast<int>(bytes[bytes.size() - 3]), static_cast<int>('a'));
}

TEF_TEST(binary_reader_rejects_truncation_and_trailing_bytes) {
  Writer writer;
  writer.u32(7);
  writer.str("payload");
  const auto& bytes = writer.bytes();

  Reader truncated(std::span<const std::byte>(bytes.data(), 2));
  std::uint32_t value = 0;
  TEF_CHECK(!truncated.u32(value));
  TEF_CHECK(truncated.failed());

  Reader exact(std::span<const std::byte>(bytes.data(), bytes.size()));
  std::string text;
  TEF_CHECK(exact.u32(value));
  TEF_CHECK_EQ(value, 7u);
  TEF_CHECK(exact.str(text));
  TEF_CHECK_EQ(text, std::string("payload"));
  TEF_CHECK(exact.at_end());

  Writer extra;
  extra.u32(7);
  extra.str("payload");
  extra.u8(1);
  Reader trailing(std::span<const std::byte>(extra.bytes().data(), extra.bytes().size()));
  TEF_CHECK(trailing.u32(value));
  TEF_CHECK(trailing.str(text));
  TEF_CHECK(!trailing.at_end());
}

TEF_TEST(binary_reader_rejects_oversized_length_prefix) {
  Writer writer;
  writer.u32(0xFFFFFFFFu);
  writer.raw("x", 1);
  Reader reader(std::span<const std::byte>(writer.bytes().data(), writer.bytes().size()));
  std::string text;
  TEF_CHECK(!reader.str(text, 16));
  TEF_CHECK(reader.failed());
}

TEF_TEST(reader_rejects_non_canonical_boolean) {
  Writer writer;
  writer.u8(2);
  Reader reader(std::span<const std::byte>(writer.bytes().data(), writer.bytes().size()));
  bool value = false;
  TEF_CHECK(!reader.boolean(value));
  TEF_CHECK(reader.failed());
}

int main(int argc, char** argv) { return tef::test::run_all(argc, argv); }
