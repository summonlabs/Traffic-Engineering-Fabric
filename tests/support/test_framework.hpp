// Traffic Engineering Fabric - minimal deterministic test framework.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deliberately tiny: tests are proof obligations, so the framework does not
// manage timeouts, retries, parallelism or watchdogs. A test either completes or
// the process does not - a hang is a defect to diagnose, never to suppress.
#pragma once

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace tef::test {

class Failure : public std::runtime_error {
 public:
  explicit Failure(const std::string& message) : std::runtime_error(message) {}
};

[[noreturn]] void fail_at(const char* file, int line, const std::string& message);

using TestFunction = void (*)();

struct TestCase {
  std::string name;
  TestFunction function = nullptr;
  std::string file;
  int line = 0;
};

std::vector<TestCase>& registry();
void register_test(const char* name, TestFunction function, const char* file, int line);

struct Registrar {
  Registrar(const char* name, TestFunction function, const char* file, int line) {
    register_test(name, function, file, line);
  }
};

int run_all(int argc, char** argv);

// Rendering a value for a failure message. Types with an ADL-visible
// to_string() (every enum in this library has one) render by name; anything
// streamable renders through operator<<; everything else renders as a
// placeholder rather than failing to compile.
template <class T>
std::string describe(const T& value) {
  if constexpr (requires(const T& candidate) {
                  { to_string(candidate) } -> std::convertible_to<std::string_view>;
                }) {
    return std::string(to_string(value));
  } else if constexpr (requires(std::ostream& stream, const T& candidate) { stream << candidate; }) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return "<unprintable>";
  }
}

}  // namespace tef::test

#define TEF_TEST(test_name)                                                            \
  static void test_name();                                                             \
  static ::tef::test::Registrar test_name##_registrar(#test_name, &test_name, __FILE__, \
                                                      __LINE__);                       \
  static void test_name()

#define TEF_FAIL(message) ::tef::test::fail_at(__FILE__, __LINE__, (message))

#define TEF_CHECK(condition)                                                           \
  do {                                                                                 \
    if (!(condition)) {                                                                \
      TEF_FAIL("check failed: " #condition);                                           \
    }                                                                                  \
  } while (false)

#define TEF_CHECK_MSG(condition, message)                                              \
  do {                                                                                 \
    if (!(condition)) {                                                                \
      std::ostringstream tef_message_stream;                                           \
      tef_message_stream << "check failed: " #condition << " (" << (message) << ")";   \
      TEF_FAIL(tef_message_stream.str());                                              \
    }                                                                                  \
  } while (false)

// The operands are captured BY VALUE. Binding a reference would dangle whenever an
// operand is a reference returned from a temporary (for example *optional_value or
// engine.get(ref)->id.str()), which is a real defect that AddressSanitizer caught.
#define TEF_CHECK_EQ(actual, expected)                                                 \
  do {                                                                                 \
    const auto tef_actual_value = (actual);                                             \
    const auto tef_expected_value = (expected);                                         \
    if (!(tef_actual_value == tef_expected_value)) {                                    \
      std::ostringstream tef_equality_stream;                                          \
      tef_equality_stream << "expected " #actual " == " #expected << "\n    actual:   " \
                          << ::tef::test::describe(tef_actual_value)                    \
                          << "\n    expected: "                                         \
                          << ::tef::test::describe(tef_expected_value);                 \
      TEF_FAIL(tef_equality_stream.str());                                             \
    }                                                                                  \
  } while (false)

#define TEF_CHECK_NE(actual, unexpected)                                               \
  do {                                                                                 \
    if ((actual) == (unexpected)) {                                                     \
      TEF_FAIL("expected " #actual " != " #unexpected);                                 \
    }                                                                                  \
  } while (false)

#define TEF_CHECK_LT(a, b)                                                             \
  do {                                                                                 \
    const auto tef_lhs_value = (a);                                                     \
    const auto tef_rhs_value = (b);                                                     \
    if (!(tef_lhs_value < tef_rhs_value)) {                                             \
      std::ostringstream tef_order_stream;                                             \
      tef_order_stream << "expected " #a " < " #b << "\n    lhs: "                      \
                       << ::tef::test::describe(tef_lhs_value)                          \
                       << "\n    rhs: " << ::tef::test::describe(tef_rhs_value);         \
      TEF_FAIL(tef_order_stream.str());                                                \
    }                                                                                  \
  } while (false)

#define TEF_CHECK_LE(a, b)                                                             \
  do {                                                                                 \
    const auto tef_lhs_value = (a);                                                     \
    const auto tef_rhs_value = (b);                                                     \
    if (!(tef_lhs_value <= tef_rhs_value)) {                                            \
      std::ostringstream tef_order_stream;                                             \
      tef_order_stream << "expected " #a " <= " #b << "\n    lhs: "                     \
                       << ::tef::test::describe(tef_lhs_value)                          \
                       << "\n    rhs: " << ::tef::test::describe(tef_rhs_value);         \
      TEF_FAIL(tef_order_stream.str());                                                \
    }                                                                                  \
  } while (false)
