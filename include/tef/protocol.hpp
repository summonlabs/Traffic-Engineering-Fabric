// Traffic Engineering Fabric - publisher/coordinator protocol.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "tef/allocation.hpp"
#include "tef/authority.hpp"
#include "tef/diagnostic.hpp"
#include "tef/model.hpp"
#include "tef/net.hpp"
#include "tef/plan.hpp"

namespace tef {

enum class MessageType : std::uint16_t {
  hello_request = 1,
  hello_response = 2,
  register_publisher_request = 3,
  register_publisher_response = 4,
  plan_request = 5,
  plan_response = 6,
  commit_request = 7,
  commit_response = 8,
  supersede_request = 9,
  supersede_response = 10,
  retire_request = 11,
  retire_response = 12,
  query_request = 13,
  query_response = 14,
  explanation_request = 15,
  explanation_response = 16,
  ping = 17,
  pong = 18,
  error_response = 19,
  shutdown_notice = 20,
};

std::string_view to_string(MessageType type) noexcept;

// Every mutation frame carries the full binding: which publisher, which boot
// incarnation of that publisher, which coordinator incarnation and epoch, and
// which attempt. A frame that fails any part of this binding is rejected before
// it can touch authoritative state.
struct MutationBinding {
  PublisherId publisher;
  BootId publisher_boot;
  CoordinatorIncarnation coordinator_incarnation;
  std::uint64_t coordinator_epoch = 0;
  AttemptId attempt;
  AttemptGeneration attempt_generation;

  bool valid() const noexcept {
    return publisher.valid() && publisher_boot.valid() && coordinator_incarnation.valid() &&
           coordinator_epoch != 0 && attempt.valid() && attempt_generation.valid();
  }
};

struct HelloRequest {
  std::uint16_t protocol_version = 0;
};

struct HelloResponse {
  std::uint16_t protocol_version = 0;
  std::uint64_t coordinator_epoch = 0;
  CoordinatorIncarnation coordinator_incarnation;
  BootId coordinator_boot;
  bool accepting = false;
};

struct RegisterPublisherRequest {
  PublisherId publisher;
  BootId boot;
  CoordinatorIncarnation coordinator_incarnation;
  std::uint64_t coordinator_epoch = 0;
  std::string description;
};

struct RegisterPublisherResponse {
  bool accepted = false;
  bool fenced = false;
  std::uint64_t coordinator_epoch = 0;
  CoordinatorIncarnation coordinator_incarnation;
  std::string detail;
};

struct PlanRequest {
  MutationBinding binding;
  PlanId plan_id;                    // the publisher chooses the plan identity
  std::vector<std::byte> snapshot;   // canonical snapshot encoding
  bool allow_degraded = false;
  bool has_incumbent = false;
  std::vector<std::byte> incumbent;  // canonical allocation encoding
  PlanRef incumbent_ref;
};

struct PlanResponse {
  ErrorCode code = ErrorCode::ok;
  std::string detail;
  PlanRef plan;
  PlanState state = PlanState::declared;
  PlanApplicability applicability = PlanApplicability::not_evaluated;
  FeasibilityStatus feasibility = FeasibilityStatus::feasible;
  std::int64_t objective_score = 0;
  Digest plan_digest;
  Digest explanation_digest;
  std::string explanation_json;   // bounded, machine readable
  bool superseded_incumbent = false;
  PlanRef incumbent;
};

struct CommitRequest {
  MutationBinding binding;
  PlanRef plan;
  CommitId commit;
  CommitGeneration commit_generation;
};

struct CommitResponse {
  ErrorCode code = ErrorCode::ok;
  std::string detail;
  PlanRef plan;
  PlanState state = PlanState::declared;
  CommitGeneration commit_generation;
  Digest commit_digest;
  bool idempotent_replay = false;
  ChurnReport churn;
};

struct SupersedeRequest {
  MutationBinding binding;
  PlanRef incumbent;
  PlanRef replacement;
};

struct SupersedeResponse {
  ErrorCode code = ErrorCode::ok;
  std::string detail;
  PlanRef incumbent;
  PlanRef replacement;
};

struct RetireRequest {
  MutationBinding binding;
  PlanRef plan;
};

struct RetireResponse {
  ErrorCode code = ErrorCode::ok;
  std::string detail;
  PlanRef plan;
  PlanState state = PlanState::declared;
};

struct QueryRequest {
  PlanRef plan;
  bool include_explanation = false;
};

struct QueryResponse {
  ErrorCode code = ErrorCode::ok;
  std::string detail;
  Plan plan;
  PlanApplicability applicability = PlanApplicability::not_evaluated;
  std::string explanation_json;
};

struct ErrorResponse {
  ErrorCode code = ErrorCode::invalid_argument;
  std::string detail;
};

// Canonical encoders/decoders. Encode functions never throw; decode functions
// validate every field and reject trailing bytes.
std::vector<std::byte> encode(const HelloRequest& message);
std::vector<std::byte> encode(const HelloResponse& message);
std::vector<std::byte> encode(const RegisterPublisherRequest& message);
std::vector<std::byte> encode(const RegisterPublisherResponse& message);
std::vector<std::byte> encode(const PlanRequest& message);
std::vector<std::byte> encode(const PlanResponse& message);
std::vector<std::byte> encode(const CommitRequest& message);
std::vector<std::byte> encode(const CommitResponse& message);
std::vector<std::byte> encode(const SupersedeRequest& message);
std::vector<std::byte> encode(const SupersedeResponse& message);
std::vector<std::byte> encode(const RetireRequest& message);
std::vector<std::byte> encode(const RetireResponse& message);
std::vector<std::byte> encode(const QueryRequest& message);
std::vector<std::byte> encode(const QueryResponse& message);
std::vector<std::byte> encode(const ErrorResponse& message);

Result<HelloRequest> decode_hello_request(std::span<const std::byte> payload);
Result<HelloResponse> decode_hello_response(std::span<const std::byte> payload);
Result<RegisterPublisherRequest> decode_register_publisher_request(std::span<const std::byte> payload);
Result<RegisterPublisherResponse> decode_register_publisher_response(std::span<const std::byte> payload);
Result<PlanRequest> decode_plan_request(std::span<const std::byte> payload);
Result<PlanResponse> decode_plan_response(std::span<const std::byte> payload);
Result<CommitRequest> decode_commit_request(std::span<const std::byte> payload);
Result<CommitResponse> decode_commit_response(std::span<const std::byte> payload);
Result<SupersedeRequest> decode_supersede_request(std::span<const std::byte> payload);
Result<SupersedeResponse> decode_supersede_response(std::span<const std::byte> payload);
Result<RetireRequest> decode_retire_request(std::span<const std::byte> payload);
Result<RetireResponse> decode_retire_response(std::span<const std::byte> payload);
Result<QueryRequest> decode_query_request(std::span<const std::byte> payload);
Result<QueryResponse> decode_query_response(std::span<const std::byte> payload);
Result<ErrorResponse> decode_error_response(std::span<const std::byte> payload);

// Canonical allocation encoding (used for incumbents on the wire).
std::vector<std::byte> encode_allocation(const Allocation& allocation);
Result<Allocation> decode_allocation(std::span<const std::byte> payload);

}  // namespace tef
