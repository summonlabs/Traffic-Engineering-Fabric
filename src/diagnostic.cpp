// Traffic Engineering Fabric - diagnostic rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "tef/diagnostic.hpp"

#include <string>

namespace tef {

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::ok: return "OK";
    case ErrorCode::invalid_argument: return "INVALID_ARGUMENT";
    case ErrorCode::malformed_input: return "MALFORMED_INPUT";
    case ErrorCode::truncated_input: return "TRUNCATED_INPUT";
    case ErrorCode::trailing_input: return "TRAILING_INPUT";
    case ErrorCode::oversized_input: return "OVERSIZED_INPUT";
    case ErrorCode::unsupported_version: return "UNSUPPORTED_VERSION";
    case ErrorCode::integrity_failure: return "INTEGRITY_FAILURE";
    case ErrorCode::unsupported_feature: return "UNSUPPORTED_FEATURE";
    case ErrorCode::duplicate_identity: return "DUPLICATE_IDENTITY";
    case ErrorCode::unknown_reference: return "UNKNOWN_REFERENCE";
    case ErrorCode::cyclic_reference: return "CYCLIC_REFERENCE";
    case ErrorCode::invalid_generation: return "INVALID_GENERATION";
    case ErrorCode::missing_provenance: return "MISSING_PROVENANCE";
    case ErrorCode::limit_exceeded: return "LIMIT_EXCEEDED";
    case ErrorCode::numeric_overflow: return "NUMERIC_OVERFLOW";
    case ErrorCode::numeric_invalid: return "NUMERIC_INVALID";
    case ErrorCode::stale_authority: return "STALE_AUTHORITY";
    case ErrorCode::conflicting_authority: return "CONFLICTING_AUTHORITY";
    case ErrorCode::authority_regression: return "AUTHORITY_REGRESSION";
    case ErrorCode::fenced: return "FENCED";
    case ErrorCode::epoch_mismatch: return "EPOCH_MISMATCH";
    case ErrorCode::unauthorized: return "UNAUTHORIZED";
    case ErrorCode::invalid_transition: return "INVALID_TRANSITION";
    case ErrorCode::already_committed: return "ALREADY_COMMITTED";
    case ErrorCode::not_found: return "NOT_FOUND";
    case ErrorCode::superseded: return "SUPERSEDED";
    case ErrorCode::retired: return "RETIRED";
    case ErrorCode::io_failure: return "IO_FAILURE";
    case ErrorCode::transport_failure: return "TRANSPORT_FAILURE";
    case ErrorCode::timeout: return "TIMEOUT";
    case ErrorCode::cancellation: return "CANCELLATION";
    case ErrorCode::shutdown: return "SHUTDOWN";
    case ErrorCode::capacity_exhausted: return "CAPACITY_EXHAUSTED";
    case ErrorCode::internal_error: return "INTERNAL_ERROR";
    case ErrorCode::unsupported_objective: return "UNSUPPORTED_OBJECTIVE";
    case ErrorCode::solver_limit: return "SOLVER_LIMIT";
    case ErrorCode::churn_bound: return "CHURN_BOUND";
    case ErrorCode::degraded_not_permitted: return "DEGRADED_NOT_PERMITTED";
  }
  return "UNKNOWN";
}

std::string Error::format() const {
  if (ok()) return std::string("OK");
  std::string out(to_string(code_));
  out += ": ";
  out += detail_;
  return out;
}

}  // namespace tef
