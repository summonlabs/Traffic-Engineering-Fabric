// Traffic Engineering Fabric - minimal deterministic test framework.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "support/test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace tef::test {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

void fail_at(const char* file, int line, const std::string& message) {
  throw Failure(std::string(file) + ":" + std::to_string(line) + ": " + message);
}

void register_test(const char* name, TestFunction function, const char* file, int line) {
  TestCase test;
  test.name = name;
  test.function = function;
  test.file = file;
  test.line = line;
  registry().push_back(std::move(test));
}

int run_all(int argc, char** argv) {
  const char* filter = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "--filter=", 9) == 0) filter = argv[i] + 9;
  }

  std::size_t passed = 0;
  std::size_t failed = 0;
  std::size_t skipped = 0;
  for (const auto& test : registry()) {
    if (filter != nullptr && test.name.find(filter) == std::string::npos) {
      ++skipped;
      continue;
    }
    std::string outcome;
    bool ok = true;
    try {
      test.function();
    } catch (const Failure& failure) {
      ok = false;
      outcome = failure.what();
    } catch (const std::exception& error) {
      ok = false;
      outcome = std::string("unexpected exception: ") + error.what();
    } catch (...) {
      ok = false;
      outcome = "unexpected non-standard exception";
    }
    if (ok) {
      ++passed;
      std::printf("[ PASS ] %s\n", test.name.c_str());
    } else {
      ++failed;
      std::printf("[ FAIL ] %s\n         %s\n", test.name.c_str(), outcome.c_str());
    }
    std::fflush(stdout);
  }

  std::printf("----\npassed=%zu failed=%zu skipped=%zu total=%zu\n", passed, failed, skipped,
              registry().size());
  std::fflush(stdout);
  return failed == 0 ? 0 : 1;
}

}  // namespace tef::test
