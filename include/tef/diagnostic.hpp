// Traffic Engineering Fabric - typed diagnostics.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace tef {

// Failure taxonomy. Codes are stable, serialized, and part of the public API.
enum class ErrorCode : std::uint16_t {
  ok = 0,
  // Structural / encoding.
  invalid_argument = 1,
  malformed_input = 2,
  truncated_input = 3,
  trailing_input = 4,
  oversized_input = 5,
  unsupported_version = 6,
  integrity_failure = 7,
  unsupported_feature = 8,
  // Model semantics.
  duplicate_identity = 9,
  unknown_reference = 10,
  cyclic_reference = 11,
  invalid_generation = 12,
  missing_provenance = 13,
  limit_exceeded = 14,
  numeric_overflow = 15,
  numeric_invalid = 16,
  // Authority.
  stale_authority = 17,
  conflicting_authority = 18,
  authority_regression = 19,
  fenced = 20,
  epoch_mismatch = 21,
  unauthorized = 22,
  // Lifecycle.
  invalid_transition = 23,
  already_committed = 24,
  not_found = 25,
  superseded = 26,
  retired = 27,
  // Runtime.
  io_failure = 28,
  transport_failure = 29,
  timeout = 30,
  cancellation = 31,
  shutdown = 32,
  capacity_exhausted = 33,
  internal_error = 34,
  unsupported_objective = 35,
  solver_limit = 36,
  churn_bound = 37,
  degraded_not_permitted = 38,
};

std::string_view to_string(ErrorCode code) noexcept;

class Error {
 public:
  Error() = default;
  Error(ErrorCode code, std::string detail) : code_(code), detail_(std::move(detail)) {}

  ErrorCode code() const noexcept { return code_; }
  const std::string& detail() const noexcept { return detail_; }
  bool ok() const noexcept { return code_ == ErrorCode::ok; }

  std::string format() const;

 private:
  ErrorCode code_ = ErrorCode::ok;
  std::string detail_;
};

// Result<T>: explicit value-or-error. The library never throws and never
// silently substitutes a default for a failed operation.
template <class T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Error error) : storage_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  bool has_value() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return has_value(); }

  T& value() & { return std::get<0>(storage_); }
  const T& value() const& { return std::get<0>(storage_); }
  T&& value() && { return std::get<0>(std::move(storage_)); }

  const Error& error() const& { return std::get<1>(storage_); }

  T value_or(T fallback) const {
    return has_value() ? std::get<0>(storage_) : std::move(fallback);
  }

 private:
  std::variant<T, Error> storage_;
};

// Result<void> equivalent for operations that only report success/failure.
class Status {
 public:
  Status() = default;
  Status(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  bool ok() const noexcept { return error_.ok(); }
  explicit operator bool() const noexcept { return ok(); }
  const Error& error() const noexcept { return error_; }
  ErrorCode code() const noexcept { return error_.code(); }
  std::string format() const { return error_.format(); }

  static Status success() { return Status(); }

 private:
  Error error_;
};

inline Status fail(ErrorCode code, std::string detail) { return Status(Error(code, std::move(detail))); }

template <class T>
inline Result<T> fail_as(ErrorCode code, std::string detail) {
  return Result<T>(Error(code, std::move(detail)));
}

}  // namespace tef
