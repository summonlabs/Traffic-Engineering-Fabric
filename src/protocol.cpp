// Traffic Engineering Fabric - publisher/coordinator protocol codecs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "tef/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "tef/binary.hpp"
#include "tef/persist.hpp"
#include "tef/version.hpp"

namespace tef {
namespace {

void write_binding(Writer& w, const MutationBinding& binding) {
  w.str(binding.publisher.str());
  w.raw(binding.publisher_boot.bytes.data(), binding.publisher_boot.bytes.size());
  w.u64(binding.coordinator_incarnation.value());
  w.u64(binding.coordinator_epoch);
  w.str(binding.attempt.str());
  w.u64(binding.attempt_generation.value());
}

bool read_binding(Reader& r, MutationBinding& binding) {
  std::string text;
  if (!r.str(text, Limits::max_name_length)) return false;
  const auto publisher = PublisherId::parse(text);
  if (!publisher) return false;
  binding.publisher = *publisher;
  if (!r.raw(binding.publisher_boot.bytes.data(), binding.publisher_boot.bytes.size())) return false;
  std::uint64_t value = 0;
  if (!r.u64(value)) return false;
  const auto incarnation = CoordinatorIncarnation::parse(value);
  if (!incarnation) return false;
  binding.coordinator_incarnation = *incarnation;
  if (!r.u64(binding.coordinator_epoch)) return false;
  if (!r.str(text, Limits::max_name_length)) return false;
  const auto attempt = AttemptId::parse(text);
  if (!attempt) return false;
  binding.attempt = *attempt;
  if (!r.u64(value)) return false;
  const auto generation = AttemptGeneration::parse(value);
  if (!generation) return false;
  binding.attempt_generation = *generation;
  return true;
}

void write_plan_ref(Writer& w, const PlanRef& ref) {
  w.str(ref.id.str());
  w.u64(ref.generation.value());
}

bool read_plan_ref(Reader& r, PlanRef& ref) {
  std::string text;
  if (!r.str(text, Limits::max_name_length)) return false;
  if (!text.empty()) {
    const auto id = PlanId::parse(text);
    if (!id) return false;
    ref.id = *id;
  }
  std::uint64_t value = 0;
  if (!r.u64(value)) return false;
  if (ref.id.valid()) {
    const auto generation = PlanGeneration::parse(value);
    if (!generation) return false;
    ref.generation = *generation;
  }
  return true;
}

bool read_plan_ref_required(Reader& r, PlanRef& ref) {
  if (!read_plan_ref(r, ref)) return false;
  return ref.valid();
}

void write_churn(Writer& w, const ChurnReport& churn) {
  w.u8(static_cast<std::uint8_t>(churn.decision));
  w.i64(churn.incumbent_score);
  w.i64(churn.proposal_score);
  w.i64(churn.improvement_permille);
  w.i64(churn.required_improvement_permille);
  w.i64(churn.moved_bandwidth);
  w.i64(churn.total_bandwidth);
  w.i64(churn.moved_permille);
  w.i64(churn.max_moved_permille);
  w.u32(churn.demands_changed);
  w.u32(churn.max_affected_demands);
  w.u32(churn.paths_added);
  w.u32(churn.paths_removed);
  w.u32(churn.failure_domains_changed);
  w.i64(churn.operational_risk_permille);
  w.str(churn.rationale);
}

bool read_churn(Reader& r, ChurnReport& churn) {
  std::uint8_t decision = 0;
  if (!r.u8(decision)) return false;
  if (decision > static_cast<std::uint8_t>(ChurnDecision::reject_regression)) return false;
  churn.decision = static_cast<ChurnDecision>(decision);
  if (!r.i64(churn.incumbent_score)) return false;
  if (!r.i64(churn.proposal_score)) return false;
  if (!r.i64(churn.improvement_permille)) return false;
  if (!r.i64(churn.required_improvement_permille)) return false;
  if (!r.i64(churn.moved_bandwidth)) return false;
  if (!r.i64(churn.total_bandwidth)) return false;
  if (!r.i64(churn.moved_permille)) return false;
  if (!r.i64(churn.max_moved_permille)) return false;
  if (!r.u32(churn.demands_changed)) return false;
  if (!r.u32(churn.max_affected_demands)) return false;
  if (!r.u32(churn.paths_added)) return false;
  if (!r.u32(churn.paths_removed)) return false;
  if (!r.u32(churn.failure_domains_changed)) return false;
  if (!r.i64(churn.operational_risk_permille)) return false;
  return r.str(churn.rationale);
}

template <class T, class Decoder>
Result<T> decode_strict(std::span<const std::byte> payload, Decoder decoder) {
  Reader reader(payload);
  T message;
  if (!decoder(reader, message)) {
    return fail_as<T>(ErrorCode::malformed_input, "the frame payload is malformed or truncated");
  }
  if (reader.failed()) {
    return fail_as<T>(ErrorCode::malformed_input, "the frame payload is malformed or truncated");
  }
  if (!reader.at_end()) {
    return fail_as<T>(ErrorCode::trailing_input, "the frame payload has trailing bytes");
  }
  return message;
}

const char* kMalformed = "the message payload is malformed";

}  // namespace

std::string_view to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::hello_request: return "hello_request";
    case MessageType::hello_response: return "hello_response";
    case MessageType::register_publisher_request: return "register_publisher_request";
    case MessageType::register_publisher_response: return "register_publisher_response";
    case MessageType::plan_request: return "plan_request";
    case MessageType::plan_response: return "plan_response";
    case MessageType::commit_request: return "commit_request";
    case MessageType::commit_response: return "commit_response";
    case MessageType::supersede_request: return "supersede_request";
    case MessageType::supersede_response: return "supersede_response";
    case MessageType::retire_request: return "retire_request";
    case MessageType::retire_response: return "retire_response";
    case MessageType::query_request: return "query_request";
    case MessageType::query_response: return "query_response";
    case MessageType::explanation_request: return "explanation_request";
    case MessageType::explanation_response: return "explanation_response";
    case MessageType::ping: return "ping";
    case MessageType::pong: return "pong";
    case MessageType::error_response: return "error_response";
    case MessageType::shutdown_notice: return "shutdown_notice";
  }
  return "unknown";
}

std::vector<std::byte> encode_allocation(const Allocation& allocation) {
  Writer w;
  w.u16(static_cast<std::uint16_t>(kDurableFormatVersion));
  w.u32(static_cast<std::uint32_t>(allocation.demands.size()));
  for (const auto& demand : allocation.demands) {
    w.str(demand.demand.str());
    w.u64(demand.generation.value());
    w.i64(demand.minimum);
    w.i64(demand.desired);
    w.i64(demand.maximum);
    w.i64(demand.granted);
    w.i64(demand.reserved);
    w.i64(demand.effective);
    w.u32(static_cast<std::uint32_t>(demand.shares.size()));
    for (const auto& share : demand.shares) {
      w.str(share.path.str());
      w.u64(share.generation.value());
      w.i64(share.granted);
      w.i64(share.reserved);
    }
  }
  w.i64(allocation.total_granted);
  w.i64(allocation.total_reserved);
  w.boolean(allocation.degraded);
  return w.bytes();
}

Result<Allocation> decode_allocation(std::span<const std::byte> payload) {
  Reader r(payload);
  Allocation allocation;
  std::uint16_t version = 0;
  if (!r.u16(version)) return fail_as<Allocation>(ErrorCode::truncated_input, "allocation version is truncated");
  if (version != kDurableFormatVersion) {
    return fail_as<Allocation>(ErrorCode::unsupported_version, "unsupported allocation format version");
  }
  std::uint32_t count = 0;
  if (!r.u32(count)) return fail_as<Allocation>(ErrorCode::truncated_input, "allocation demand count is truncated");
  if (count > Limits::max_demands) return fail_as<Allocation>(ErrorCode::oversized_input, "allocation demand count exceeds the bound");
  for (std::uint32_t i = 0; i < count; ++i) {
    DemandAllocation demand;
    std::string text;
    if (!r.str(text, Limits::max_name_length)) return fail_as<Allocation>(ErrorCode::malformed_input, kMalformed);
    const auto id = DemandId::parse(text);
    if (!id) return fail_as<Allocation>(ErrorCode::malformed_input, "allocation demand id is invalid");
    demand.demand = *id;
    std::uint64_t value = 0;
    if (!r.u64(value)) return fail_as<Allocation>(ErrorCode::truncated_input, "allocation demand generation is truncated");
    const auto generation = DemandGeneration::parse(value);
    if (!generation) return fail_as<Allocation>(ErrorCode::invalid_generation, "allocation demand generation is zero");
    demand.generation = *generation;
    if (!r.i64(demand.minimum) || !r.i64(demand.desired) || !r.i64(demand.maximum) ||
        !r.i64(demand.granted) || !r.i64(demand.reserved) || !r.i64(demand.effective)) {
      return fail_as<Allocation>(ErrorCode::truncated_input, "allocation demand quantities are truncated");
    }
    std::uint32_t shares = 0;
    if (!r.u32(shares)) return fail_as<Allocation>(ErrorCode::truncated_input, "allocation share count is truncated");
    if (shares > Limits::max_paths_per_demand) return fail_as<Allocation>(ErrorCode::oversized_input, "allocation share count exceeds the bound");
    for (std::uint32_t j = 0; j < shares; ++j) {
      PathShare share;
      if (!r.str(text, Limits::max_name_length)) return fail_as<Allocation>(ErrorCode::malformed_input, kMalformed);
      const auto path = PathId::parse(text);
      if (!path) return fail_as<Allocation>(ErrorCode::malformed_input, "allocation path id is invalid");
      share.path = *path;
      if (!r.u64(value)) return fail_as<Allocation>(ErrorCode::truncated_input, "allocation path generation is truncated");
      const auto path_generation = PathGeneration::parse(value);
      if (!path_generation) return fail_as<Allocation>(ErrorCode::invalid_generation, "allocation path generation is zero");
      share.generation = *path_generation;
      if (!r.i64(share.granted) || !r.i64(share.reserved)) {
        return fail_as<Allocation>(ErrorCode::truncated_input, "allocation share quantities are truncated");
      }
      demand.shares.push_back(std::move(share));
    }
    allocation.demands.push_back(std::move(demand));
  }
  if (!r.i64(allocation.total_granted) || !r.i64(allocation.total_reserved)) {
    return fail_as<Allocation>(ErrorCode::truncated_input, "allocation totals are truncated");
  }
  if (!r.boolean(allocation.degraded)) return fail_as<Allocation>(ErrorCode::truncated_input, "allocation degraded flag is truncated");
  if (r.failed()) return fail_as<Allocation>(ErrorCode::malformed_input, kMalformed);
  if (!r.at_end()) return fail_as<Allocation>(ErrorCode::trailing_input, "allocation payload has trailing bytes");
  return allocation;
}

std::vector<std::byte> encode(const HelloRequest& message) {
  Writer w;
  w.u16(message.protocol_version);
  return w.bytes();
}

Result<HelloRequest> decode_hello_request(std::span<const std::byte> payload) {
  return decode_strict<HelloRequest>(payload, [](Reader& r, HelloRequest& m) { return r.u16(m.protocol_version); });
}

std::vector<std::byte> encode(const HelloResponse& message) {
  Writer w;
  w.u16(message.protocol_version);
  w.u64(message.coordinator_epoch);
  w.u64(message.coordinator_incarnation.value());
  w.raw(message.coordinator_boot.bytes.data(), message.coordinator_boot.bytes.size());
  w.boolean(message.accepting);
  return w.bytes();
}

Result<HelloResponse> decode_hello_response(std::span<const std::byte> payload) {
  return decode_strict<HelloResponse>(payload, [](Reader& r, HelloResponse& m) {
    if (!r.u16(m.protocol_version)) return false;
    if (!r.u64(m.coordinator_epoch)) return false;
    std::uint64_t value = 0;
    if (!r.u64(value)) return false;
    const auto incarnation = CoordinatorIncarnation::parse(value);
    if (!incarnation) return false;
    m.coordinator_incarnation = *incarnation;
    if (!r.raw(m.coordinator_boot.bytes.data(), m.coordinator_boot.bytes.size())) return false;
    return r.boolean(m.accepting);
  });
}

std::vector<std::byte> encode(const RegisterPublisherRequest& message) {
  Writer w;
  w.str(message.publisher.str());
  w.raw(message.boot.bytes.data(), message.boot.bytes.size());
  w.u64(message.coordinator_incarnation.value());
  w.u64(message.coordinator_epoch);
  w.str(message.description);
  return w.bytes();
}

Result<RegisterPublisherRequest> decode_register_publisher_request(std::span<const std::byte> payload) {
  return decode_strict<RegisterPublisherRequest>(payload, [](Reader& r, RegisterPublisherRequest& m) {
    std::string text;
    if (!r.str(text, Limits::max_name_length)) return false;
    const auto publisher = PublisherId::parse(text);
    if (!publisher) return false;
    m.publisher = *publisher;
    if (!r.raw(m.boot.bytes.data(), m.boot.bytes.size())) return false;
    std::uint64_t value = 0;
    if (!r.u64(value)) return false;
    const auto incarnation = CoordinatorIncarnation::parse(value);
    if (!incarnation) return false;
    m.coordinator_incarnation = *incarnation;
    if (!r.u64(m.coordinator_epoch)) return false;
    return r.str(m.description);
  });
}

std::vector<std::byte> encode(const RegisterPublisherResponse& message) {
  Writer w;
  w.boolean(message.accepted);
  w.boolean(message.fenced);
  w.u64(message.coordinator_epoch);
  w.u64(message.coordinator_incarnation.value());
  w.str(message.detail);
  return w.bytes();
}

Result<RegisterPublisherResponse> decode_register_publisher_response(std::span<const std::byte> payload) {
  return decode_strict<RegisterPublisherResponse>(payload, [](Reader& r, RegisterPublisherResponse& m) {
    if (!r.boolean(m.accepted)) return false;
    if (!r.boolean(m.fenced)) return false;
    if (!r.u64(m.coordinator_epoch)) return false;
    std::uint64_t value = 0;
    if (!r.u64(value)) return false;
    const auto incarnation = CoordinatorIncarnation::parse(value);
    if (!incarnation) return false;
    m.coordinator_incarnation = *incarnation;
    return r.str(m.detail);
  });
}

std::vector<std::byte> encode(const PlanRequest& message) {
  Writer w;
  write_binding(w, message.binding);
  w.str(message.plan_id.str());
  w.blob(std::span<const std::byte>(message.snapshot.data(), message.snapshot.size()));
  w.boolean(message.allow_degraded);
  w.boolean(message.has_incumbent);
  w.blob(std::span<const std::byte>(message.incumbent.data(), message.incumbent.size()));
  write_plan_ref(w, message.incumbent_ref);
  return w.bytes();
}

Result<PlanRequest> decode_plan_request(std::span<const std::byte> payload) {
  return decode_strict<PlanRequest>(payload, [](Reader& r, PlanRequest& m) {
    if (!read_binding(r, m.binding)) return false;
    std::string text;
    if (!r.str(text, Limits::max_name_length)) return false;
    const auto plan_id = PlanId::parse(text);
    if (!plan_id) return false;
    m.plan_id = *plan_id;
    if (!r.blob(m.snapshot)) return false;
    if (!r.boolean(m.allow_degraded)) return false;
    if (!r.boolean(m.has_incumbent)) return false;
    if (!r.blob(m.incumbent)) return false;
    return read_plan_ref(r, m.incumbent_ref);
  });
}

std::vector<std::byte> encode(const PlanResponse& message) {
  Writer w;
  w.u16(static_cast<std::uint16_t>(message.code));
  w.str(message.detail);
  write_plan_ref(w, message.plan);
  w.u16(static_cast<std::uint16_t>(message.state));
  w.u16(static_cast<std::uint16_t>(message.applicability));
  w.u16(static_cast<std::uint16_t>(message.feasibility));
  w.i64(message.objective_score);
  w.digest(message.plan_digest);
  w.digest(message.explanation_digest);
  w.str(message.explanation_json);
  w.boolean(message.superseded_incumbent);
  write_plan_ref(w, message.incumbent);
  return w.bytes();
}

Result<PlanResponse> decode_plan_response(std::span<const std::byte> payload) {
  return decode_strict<PlanResponse>(payload, [](Reader& r, PlanResponse& m) {
    std::uint16_t code = 0;
    if (!r.u16(code)) return false;
    m.code = static_cast<ErrorCode>(code);
    if (!r.str(m.detail)) return false;
    if (!read_plan_ref(r, m.plan)) return false;
    std::uint16_t state = 0;
    std::uint16_t applicability = 0;
    std::uint16_t feasibility = 0;
    if (!r.u16(state)) return false;
    if (!r.u16(applicability)) return false;
    if (!r.u16(feasibility)) return false;
    if (state > static_cast<std::uint16_t>(PlanState::retired)) return false;
    if (applicability > static_cast<std::uint16_t>(PlanApplicability::retired)) return false;
    if (feasibility > static_cast<std::uint16_t>(FeasibilityStatus::solver_limit_reached)) return false;
    m.state = static_cast<PlanState>(state);
    m.applicability = static_cast<PlanApplicability>(applicability);
    m.feasibility = static_cast<FeasibilityStatus>(feasibility);
    if (!r.i64(m.objective_score)) return false;
    if (!r.digest(m.plan_digest)) return false;
    if (!r.digest(m.explanation_digest)) return false;
    if (!r.str(m.explanation_json, Limits::max_frame_payload)) return false;
    if (!r.boolean(m.superseded_incumbent)) return false;
    return read_plan_ref(r, m.incumbent);
  });
}

std::vector<std::byte> encode(const CommitRequest& message) {
  Writer w;
  write_binding(w, message.binding);
  write_plan_ref(w, message.plan);
  w.str(message.commit.str());
  w.u64(message.commit_generation.value());
  return w.bytes();
}

Result<CommitRequest> decode_commit_request(std::span<const std::byte> payload) {
  return decode_strict<CommitRequest>(payload, [](Reader& r, CommitRequest& m) {
    if (!read_binding(r, m.binding)) return false;
    if (!read_plan_ref_required(r, m.plan)) return false;
    std::string text;
    if (!r.str(text, Limits::max_name_length)) return false;
    const auto commit = CommitId::parse(text);
    if (!commit) return false;
    m.commit = *commit;
    std::uint64_t value = 0;
    if (!r.u64(value)) return false;
    const auto generation = CommitGeneration::parse(value);
    if (!generation) return false;
    m.commit_generation = *generation;
    return true;
  });
}

std::vector<std::byte> encode(const CommitResponse& message) {
  Writer w;
  w.u16(static_cast<std::uint16_t>(message.code));
  w.str(message.detail);
  write_plan_ref(w, message.plan);
  w.u16(static_cast<std::uint16_t>(message.state));
  w.u64(message.commit_generation.value());
  w.digest(message.commit_digest);
  w.boolean(message.idempotent_replay);
  write_churn(w, message.churn);
  return w.bytes();
}

Result<CommitResponse> decode_commit_response(std::span<const std::byte> payload) {
  return decode_strict<CommitResponse>(payload, [](Reader& r, CommitResponse& m) {
    std::uint16_t code = 0;
    if (!r.u16(code)) return false;
    m.code = static_cast<ErrorCode>(code);
    if (!r.str(m.detail)) return false;
    if (!read_plan_ref(r, m.plan)) return false;
    std::uint16_t state = 0;
    if (!r.u16(state)) return false;
    if (state > static_cast<std::uint16_t>(PlanState::retired)) return false;
    m.state = static_cast<PlanState>(state);
    std::uint64_t value = 0;
    if (!r.u64(value)) return false;
    if (value != 0) {
      const auto generation = CommitGeneration::parse(value);
      if (!generation) return false;
      m.commit_generation = *generation;
    }
    if (!r.digest(m.commit_digest)) return false;
    if (!r.boolean(m.idempotent_replay)) return false;
    return read_churn(r, m.churn);
  });
}

std::vector<std::byte> encode(const SupersedeRequest& message) {
  Writer w;
  write_binding(w, message.binding);
  write_plan_ref(w, message.incumbent);
  write_plan_ref(w, message.replacement);
  return w.bytes();
}

Result<SupersedeRequest> decode_supersede_request(std::span<const std::byte> payload) {
  return decode_strict<SupersedeRequest>(payload, [](Reader& r, SupersedeRequest& m) {
    if (!read_binding(r, m.binding)) return false;
    if (!read_plan_ref_required(r, m.incumbent)) return false;
    return read_plan_ref_required(r, m.replacement);
  });
}

std::vector<std::byte> encode(const SupersedeResponse& message) {
  Writer w;
  w.u16(static_cast<std::uint16_t>(message.code));
  w.str(message.detail);
  write_plan_ref(w, message.incumbent);
  write_plan_ref(w, message.replacement);
  return w.bytes();
}

Result<SupersedeResponse> decode_supersede_response(std::span<const std::byte> payload) {
  return decode_strict<SupersedeResponse>(payload, [](Reader& r, SupersedeResponse& m) {
    std::uint16_t code = 0;
    if (!r.u16(code)) return false;
    m.code = static_cast<ErrorCode>(code);
    if (!r.str(m.detail)) return false;
    if (!read_plan_ref(r, m.incumbent)) return false;
    return read_plan_ref(r, m.replacement);
  });
}

std::vector<std::byte> encode(const RetireRequest& message) {
  Writer w;
  write_binding(w, message.binding);
  write_plan_ref(w, message.plan);
  return w.bytes();
}

Result<RetireRequest> decode_retire_request(std::span<const std::byte> payload) {
  return decode_strict<RetireRequest>(payload, [](Reader& r, RetireRequest& m) {
    if (!read_binding(r, m.binding)) return false;
    return read_plan_ref_required(r, m.plan);
  });
}

std::vector<std::byte> encode(const RetireResponse& message) {
  Writer w;
  w.u16(static_cast<std::uint16_t>(message.code));
  w.str(message.detail);
  write_plan_ref(w, message.plan);
  w.u16(static_cast<std::uint16_t>(message.state));
  return w.bytes();
}

Result<RetireResponse> decode_retire_response(std::span<const std::byte> payload) {
  return decode_strict<RetireResponse>(payload, [](Reader& r, RetireResponse& m) {
    std::uint16_t code = 0;
    if (!r.u16(code)) return false;
    m.code = static_cast<ErrorCode>(code);
    if (!r.str(m.detail)) return false;
    if (!read_plan_ref(r, m.plan)) return false;
    std::uint16_t state = 0;
    if (!r.u16(state)) return false;
    if (state > static_cast<std::uint16_t>(PlanState::retired)) return false;
    m.state = static_cast<PlanState>(state);
    return true;
  });
}

std::vector<std::byte> encode(const QueryRequest& message) {
  Writer w;
  write_plan_ref(w, message.plan);
  w.boolean(message.include_explanation);
  return w.bytes();
}

Result<QueryRequest> decode_query_request(std::span<const std::byte> payload) {
  return decode_strict<QueryRequest>(payload, [](Reader& r, QueryRequest& m) {
    if (!read_plan_ref_required(r, m.plan)) return false;
    return r.boolean(m.include_explanation);
  });
}

std::vector<std::byte> encode(const QueryResponse& message) {
  Writer w;
  w.u16(static_cast<std::uint16_t>(message.code));
  w.str(message.detail);
  w.u16(static_cast<std::uint16_t>(message.applicability));
  w.blob(std::span<const std::byte>(reinterpret_cast<const std::byte*>(message.explanation_json.data()),
                                    message.explanation_json.size()));
  const std::vector<std::byte> plan = encode_plan(message.plan);
  w.blob(std::span<const std::byte>(plan.data(), plan.size()));
  return w.bytes();
}

Result<QueryResponse> decode_query_response(std::span<const std::byte> payload) {
  return decode_strict<QueryResponse>(payload, [](Reader& r, QueryResponse& m) {
    std::uint16_t code = 0;
    if (!r.u16(code)) return false;
    m.code = static_cast<ErrorCode>(code);
    if (!r.str(m.detail)) return false;
    std::uint16_t applicability = 0;
    if (!r.u16(applicability)) return false;
    if (applicability > static_cast<std::uint16_t>(PlanApplicability::retired)) return false;
    m.applicability = static_cast<PlanApplicability>(applicability);
    std::vector<std::byte> explanation;
    if (!r.blob(explanation)) return false;
    m.explanation_json.assign(reinterpret_cast<const char*>(explanation.data()), explanation.size());
    std::vector<std::byte> plan;
    if (!r.blob(plan)) return false;
    const Result<Plan> decoded = decode_plan(std::span<const std::byte>(plan.data(), plan.size()));
    if (!decoded.has_value()) return false;
    m.plan = decoded.value();
    return true;
  });
}

std::vector<std::byte> encode(const ErrorResponse& message) {
  Writer w;
  w.u16(static_cast<std::uint16_t>(message.code));
  w.str(message.detail);
  return w.bytes();
}

Result<ErrorResponse> decode_error_response(std::span<const std::byte> payload) {
  return decode_strict<ErrorResponse>(payload, [](Reader& r, ErrorResponse& m) {
    std::uint16_t code = 0;
    if (!r.u16(code)) return false;
    m.code = static_cast<ErrorCode>(code);
    return r.str(m.detail);
  });
}

}  // namespace tef
