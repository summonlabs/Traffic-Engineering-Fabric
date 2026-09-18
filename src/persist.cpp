// Traffic Engineering Fabric - durable, versioned, integrity-checked storage.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// On-disk layout (fixed file names, no directory scanning, no user-controlled
// paths):
//
//   <directory>/manifest.tefm    magic + format version; created on first open
//   <directory>/snapshot.tefs    full rewrite of the durable record log
//   <directory>/journal.tefj     append-only record log after the snapshot
//   <directory>/*.tmp            in-progress atomic replacements, removed on open
//
// Every record is framed with a magic, format version, type, length, monotonic
// sequence number, the digest of the previous record (a hash chain) and a
// CRC-32C over the payload, and is terminated by a SHA-256 over the frame. A
// torn or corrupted tail is detected, the valid prefix is preserved, and the
// file is truncated at the last valid record rather than reinterpreting damaged
// bytes.
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "tef/persist.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "tef/binary.hpp"
#include "tef/digest.hpp"
#include "tef/numeric.hpp"
#include "tef/version.hpp"

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace tef {
namespace {

constexpr std::uint32_t kRecordMagic = 0x54454652u;    // 'TEFR'
constexpr std::uint32_t kJournalMagic = 0x5445464Au;   // 'TEFJ'
constexpr std::uint32_t kSnapshotMagic = 0x54454653u;  // 'TEFS'
constexpr std::uint32_t kManifestMagic = 0x5445464Du;  // 'TEFM'

constexpr std::size_t kRecordHeaderSize = 56;
constexpr std::size_t kJournalHeaderSize = 32;
constexpr std::size_t kSnapshotHeaderSize = 40;
constexpr std::size_t kDigestSize = 32;
constexpr std::size_t kManifestSize = 16;

// Bound on the memory a single record may consume while recovering.
constexpr std::size_t kMaxRecordPayload = Limits::max_frame_payload;
// Bound on the total size of a durable file this runtime is willing to read.
constexpr std::uintmax_t kMaxFileBytes = 512ull * 1024ull * 1024ull;

const char* kManifestName = "manifest.tefm";
const char* kSnapshotName = "snapshot.tefs";
const char* kJournalName = "journal.tefj";
const char* kSnapshotTempName = "snapshot.tefs.tmp";
const char* kJournalTempName = "journal.tefj.tmp";
const char* kManifestTempName = "manifest.tefm.tmp";

class File {
 public:
  File() = default;
  ~File() { close(); }
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  bool open_write(const std::filesystem::path& path) {
    close();
    handle_ = std::fopen(path.string().c_str(), "wb");
    return handle_ != nullptr;
  }

  bool open_read(const std::filesystem::path& path) {
    close();
    handle_ = std::fopen(path.string().c_str(), "rb");
    return handle_ != nullptr;
  }

  bool open_append(const std::filesystem::path& path) {
    close();
    handle_ = std::fopen(path.string().c_str(), "ab");
    return handle_ != nullptr;
  }

  bool open_read_write(const std::filesystem::path& path) {
    close();
    handle_ = std::fopen(path.string().c_str(), "r+b");
    return handle_ != nullptr;
  }

  void close() {
    if (handle_ != nullptr) {
      std::fclose(handle_);
      handle_ = nullptr;
    }
  }

  bool valid() const noexcept { return handle_ != nullptr; }

  bool write(const void* data, std::size_t size) {
    if (handle_ == nullptr) return false;
    if (size == 0) return true;
    return std::fwrite(data, 1, size, handle_) == size;
  }

  bool read(void* data, std::size_t size) {
    if (handle_ == nullptr) return false;
    if (size == 0) return true;
    return std::fread(data, 1, size, handle_) == size;
  }

  bool flush() {
    if (handle_ == nullptr) return false;
    if (std::fflush(handle_) != 0) return false;
    return sync();
  }

  bool sync() {
    if (handle_ == nullptr) return false;
#if defined(_WIN32)
    return ::_commit(::_fileno(handle_)) == 0;
#else
    return ::fsync(::fileno(handle_)) == 0;
#endif
  }

  bool truncate_at(std::uint64_t offset) {
    if (handle_ == nullptr) return false;
    if (std::fflush(handle_) != 0) return false;
#if defined(_WIN32)
    if (::_chsize_s(::_fileno(handle_), static_cast<long long>(offset)) != 0) return false;
#else
    if (::ftruncate(::fileno(handle_), static_cast<off_t>(offset)) != 0) return false;
#endif
    return sync();
  }

  std::uint64_t tell() {
    if (handle_ == nullptr) return 0;
    const long position = std::ftell(handle_);
    return position < 0 ? 0 : static_cast<std::uint64_t>(position);
  }

  bool seek(std::uint64_t offset) {
    if (handle_ == nullptr) return false;
#if defined(_WIN32)
    return ::_fseeki64(handle_, static_cast<long long>(offset), SEEK_SET) == 0;
#else
    return std::fseek(handle_, static_cast<long>(offset), SEEK_SET) == 0;
#endif
  }

  std::uint64_t size() {
    if (handle_ == nullptr) return 0;
    const std::uint64_t position = tell();
    if (!seek(0)) return 0;
    if (!seek_end()) return 0;
    const std::uint64_t end = tell();
    if (!seek(position)) return 0;
    return end;
  }

  bool seek_end() {
    if (handle_ == nullptr) return false;
#if defined(_WIN32)
    return ::_fseeki64(handle_, 0, SEEK_END) == 0;
#else
    return std::fseek(handle_, 0, SEEK_END) == 0;
#endif
  }

 private:
  std::FILE* handle_ = nullptr;
};

bool atomic_replace(const std::filesystem::path& source, const std::filesystem::path& target,
                    std::string& error) {
#if defined(_WIN32)
  if (::MoveFileExW(source.wstring().c_str(), target.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
    return true;
  }
  error = "MoveFileEx failed with error " + std::to_string(::GetLastError());
  return false;
#else
  std::error_code code;
  std::filesystem::rename(source, target, code);
  if (code) {
    error = code.message();
    return false;
  }
  return true;
#endif
}

void write_u16(std::uint8_t* out, std::uint16_t value) {
  out[0] = static_cast<std::uint8_t>(value >> 8);
  out[1] = static_cast<std::uint8_t>(value);
}

void write_u32(std::uint8_t* out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * (3 - i)));
}

void write_u64(std::uint8_t* out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * (7 - i)));
}

std::uint16_t read_u16(const std::uint8_t* in) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) |
                                    static_cast<std::uint16_t>(in[1]));
}

std::uint32_t read_u32(const std::uint8_t* in) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value = (value << 8) | in[i];
  return value;
}

std::uint64_t read_u64(const std::uint8_t* in) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value = (value << 8) | in[i];
  return value;
}

// The full authority vector is persisted, not just its digest: a durable plan
// must be re-comparable against a live authority field by field after a restart.
void write_authority(Writer& w, const AuthorityVector& authority) {
  w.u64(authority.fabric_epoch.value());
  w.u64(authority.topology_generation.value());
  w.u64(authority.link_state_generation.value());
  w.u64(authority.path_authority_generation.value());
  w.u64(authority.failure_domain_generation.value());
  w.str(authority.capacity.id.str());
  w.u64(authority.capacity.generation.value());
  w.str(authority.reservations.id.str());
  w.u64(authority.reservations.generation.value());
  w.str(authority.policy.id.str());
  w.u64(authority.policy.generation.value());
  w.str(authority.objective.id.str());
  w.u64(authority.objective.generation.value());
  w.str(authority.candidate_set.id.str());
  w.u64(authority.candidate_set.generation.value());
  w.digest(authority.demand_set_digest);
  w.digest(authority.candidate_set_digest);
  w.digest(authority.resource_catalog_digest);
  w.digest(authority.reservation_set_digest);
  w.digest(authority.policy_digest);
  w.digest(authority.objective_digest);
}

bool read_authority(Reader& r, AuthorityVector& authority) {
  std::uint64_t value = 0;
  std::string text;
  if (!r.u64(value)) return false;
  const auto epoch = FabricEpoch::parse(value);
  if (!epoch) return false;
  authority.fabric_epoch = *epoch;
  if (!r.u64(value)) return false;
  const auto topology = TopologyGeneration::parse(value);
  if (!topology) return false;
  authority.topology_generation = *topology;
  if (!r.u64(value)) return false;
  const auto link_state = LinkStateGeneration::parse(value);
  if (!link_state) return false;
  authority.link_state_generation = *link_state;
  if (!r.u64(value)) return false;
  const auto path_authority = PathAuthorityGeneration::parse(value);
  if (!path_authority) return false;
  authority.path_authority_generation = *path_authority;
  if (!r.u64(value)) return false;
  const auto failure_domain = FailureDomainGeneration::parse(value);
  if (!failure_domain) return false;
  authority.failure_domain_generation = *failure_domain;

  const auto read_ref = [&r](auto& ref, std::string& out_text, std::uint64_t& out_value) {
    if (!r.str(out_text, Limits::max_name_length)) return false;
    if (!r.u64(out_value)) return false;
    (void)ref;
    return true;
  };
  std::uint64_t generation = 0;
  if (!read_ref(authority.capacity, text, generation)) return false;
  const auto capacity_id = CapacitySnapshotId::parse(text);
  const auto capacity_generation = CapacitySnapshotGeneration::parse(generation);
  if (!capacity_id || !capacity_generation) return false;
  authority.capacity = CapacitySnapshotRef{*capacity_id, *capacity_generation};
  if (!read_ref(authority.reservations, text, generation)) return false;
  const auto reservation_id = ReservationSnapshotId::parse(text);
  const auto reservation_generation = ReservationSnapshotGeneration::parse(generation);
  if (!reservation_id || !reservation_generation) return false;
  authority.reservations = ReservationSnapshotRef{*reservation_id, *reservation_generation};
  if (!read_ref(authority.policy, text, generation)) return false;
  const auto policy_id = PolicyId::parse(text);
  const auto policy_generation = PolicyGeneration::parse(generation);
  if (!policy_id || !policy_generation) return false;
  authority.policy = PolicyRef{*policy_id, *policy_generation};
  if (!read_ref(authority.objective, text, generation)) return false;
  const auto objective_id = ObjectiveProfileId::parse(text);
  const auto objective_generation = ObjectiveProfileGeneration::parse(generation);
  if (!objective_id || !objective_generation) return false;
  authority.objective = ObjectiveProfileRef{*objective_id, *objective_generation};
  if (!read_ref(authority.candidate_set, text, generation)) return false;
  const auto candidate_id = CandidateSetId::parse(text);
  const auto candidate_generation = CandidateSetGeneration::parse(generation);
  if (!candidate_id || !candidate_generation) return false;
  authority.candidate_set = CandidateSetRef{*candidate_id, *candidate_generation};

  return r.digest(authority.demand_set_digest) && r.digest(authority.candidate_set_digest) &&
         r.digest(authority.resource_catalog_digest) && r.digest(authority.reservation_set_digest) &&
         r.digest(authority.policy_digest) && r.digest(authority.objective_digest);
}

void encode_record_image(const DurableRecord& record, std::vector<std::byte>& out) {
  out.clear();
  out.resize(kRecordHeaderSize);
  std::uint8_t* header = reinterpret_cast<std::uint8_t*>(out.data());
  write_u32(header, kRecordMagic);
  write_u16(header + 4, static_cast<std::uint16_t>(kDurableFormatVersion));
  write_u16(header + 6, static_cast<std::uint16_t>(record.type));
  write_u32(header + 8, static_cast<std::uint32_t>(record.payload.size()));
  write_u64(header + 12, record.sequence);
  std::memcpy(header + 20, record.previous_digest.bytes.data(), kDigestSize);
  write_u32(header + 52, crc32c(std::span<const std::byte>(record.payload.data(), record.payload.size())));
  out.insert(out.end(), record.payload.begin(), record.payload.end());
  Digest computed = Sha256::hash(std::span<const std::byte>(out.data(), out.size()));
  out.insert(out.end(), reinterpret_cast<const std::byte*>(computed.bytes.data()),
             reinterpret_cast<const std::byte*>(computed.bytes.data()) + computed.bytes.size());
}

}  // namespace

std::string_view to_string(RecordType type) noexcept {
  switch (type) {
    case RecordType::plan: return "plan";
    case RecordType::policy: return "policy";
    case RecordType::objective_profile: return "objective_profile";
    case RecordType::fence: return "fence";
    case RecordType::coordinator_epoch: return "coordinator_epoch";
    case RecordType::audit: return "audit";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Payload codecs
// ---------------------------------------------------------------------------

std::vector<std::byte> encode_plan(const Plan& plan) {
  Writer w;
  w.u16(static_cast<std::uint16_t>(kDurableFormatVersion));
  w.str(plan.id.str());
  w.u64(plan.generation.value());
  w.u16(static_cast<std::uint16_t>(plan.state));
  w.u16(static_cast<std::uint16_t>(plan.applicability));
  w.str(plan.attempt.str());
  w.u64(plan.attempt_generation.value());
  w.str(plan.publisher.str());
  w.raw(plan.publisher_boot.bytes.data(), plan.publisher_boot.bytes.size());
  w.u64(plan.coordinator_incarnation.value());
  write_authority(w, plan.authority);
  w.u32(static_cast<std::uint32_t>(plan.components.size()));
  for (const auto& component : plan.components) {
    w.u16(static_cast<std::uint16_t>(component.term));
    w.i64(component.weight);
    w.i64(component.raw);
    w.i64(component.weighted);
  }
  w.u32(static_cast<std::uint32_t>(plan.alternatives.size()));
  for (const auto& alternative : plan.alternatives) {
    w.str(alternative.subject);
    w.str(alternative.reason);
    w.i64(alternative.delta);
  }
  w.str(plan.feasibility.summary);
  w.u16(static_cast<std::uint16_t>(plan.feasibility.status));
  w.u32(static_cast<std::uint32_t>(plan.feasibility.binding.size()));
  for (const auto& constraint : plan.feasibility.binding) {
    w.u16(static_cast<std::uint16_t>(constraint.kind));
    w.str(constraint.subject);
    w.str(constraint.demand.str());
    w.str(constraint.resource.str());
    w.str(constraint.path.str());
    w.i64(constraint.required);
    w.i64(constraint.available);
    w.i64(constraint.slack);
    w.str(constraint.detail);
  }
  w.u32(static_cast<std::uint32_t>(plan.allocation.demands.size()));
  for (const auto& demand : plan.allocation.demands) {
    w.str(demand.demand.str());
    w.u64(demand.generation.value());
    w.i64(demand.minimum);
    w.i64(demand.desired);
    w.i64(demand.maximum);
    w.i64(demand.granted);
    w.i64(demand.reserved);
    w.i64(demand.effective);
    w.i64(demand.observed_applied);
    w.boolean(demand.applied_known);
    w.i64(demand.shortfall_against_minimum);
    w.i64(demand.shortfall_against_desired);
    w.u32(static_cast<std::uint32_t>(demand.shares.size()));
    for (const auto& share : demand.shares) {
      w.str(share.path.str());
      w.u64(share.generation.value());
      w.i64(share.minimum);
      w.i64(share.desired);
      w.i64(share.granted);
      w.i64(share.reserved);
      w.i64(share.delta_from_incumbent);
      w.boolean(share.newly_used);
    }
  }
  w.u32(static_cast<std::uint32_t>(plan.allocation.resources.size()));
  for (const auto& resource : plan.allocation.resources) {
    w.str(resource.resource.str());
    w.u64(resource.generation.value());
    w.i64(resource.usable_capacity);
    w.i64(resource.committed_load);
    w.i64(resource.reserved);
    w.i64(resource.available);
    w.i64(resource.allocated);
    w.i64(resource.headroom);
    w.i64(resource.utilization_permille);
    w.boolean(resource.saturated);
  }
  w.i64(plan.allocation.total_granted);
  w.i64(plan.allocation.total_reserved);
  w.boolean(plan.allocation.degraded);
  w.u8(static_cast<std::uint8_t>(plan.churn.decision));
  w.i64(plan.churn.incumbent_score);
  w.i64(plan.churn.proposal_score);
  w.i64(plan.churn.improvement_permille);
  w.i64(plan.churn.moved_bandwidth);
  w.i64(plan.churn.moved_permille);
  w.u32(plan.churn.demands_changed);
  w.u32(plan.churn.paths_added);
  w.u32(plan.churn.paths_removed);
  w.u32(plan.churn.failure_domains_changed);
  w.i64(plan.churn.operational_risk_permille);
  w.str(plan.churn.rationale);
  w.i64(plan.objective_score);
  w.str(plan.explanation.str());
  w.digest(plan.explanation_digest);
  w.str(plan.supersedes.id.str());
  w.u64(plan.supersedes.generation.value());
  w.str(plan.superseded_by.id.str());
  w.u64(plan.superseded_by.generation.value());
  w.str(plan.commit.str());
  w.u64(plan.commit_generation.value());
  w.u64(plan.committed_tick);
  w.u64(plan.declared_tick);
  w.u64(plan.proposed_tick);
  return w.bytes();
}

Result<Plan> decode_plan(std::span<const std::byte> payload) {
  Reader r(payload);
  Plan plan;
  std::uint16_t version = 0;
  if (!r.u16(version)) return fail_as<Plan>(ErrorCode::truncated_input, "plan record version is truncated");
  if (version != kDurableFormatVersion) {
    return fail_as<Plan>(ErrorCode::unsupported_version, "unsupported plan record version");
  }
  std::string text;
  std::uint64_t value = 0;
  if (!r.str(text, Limits::max_name_length)) return fail_as<Plan>(ErrorCode::malformed_input, "plan id is malformed");
  const auto id = PlanId::parse(text);
  if (!id) return fail_as<Plan>(ErrorCode::malformed_input, "plan id is invalid");
  plan.id = *id;
  if (!r.u64(value)) return fail_as<Plan>(ErrorCode::truncated_input, "plan generation is truncated");
  const auto generation = PlanGeneration::parse(value);
  if (!generation) return fail_as<Plan>(ErrorCode::invalid_generation, "plan generation is zero");
  plan.generation = *generation;
  std::uint16_t state = 0;
  std::uint16_t applicability = 0;
  if (!r.u16(state)) return fail_as<Plan>(ErrorCode::truncated_input, "plan state is truncated");
  if (!r.u16(applicability)) return fail_as<Plan>(ErrorCode::truncated_input, "plan applicability is truncated");
  if (state > static_cast<std::uint16_t>(PlanState::retired)) {
    return fail_as<Plan>(ErrorCode::malformed_input, "plan state is out of range");
  }
  if (applicability > static_cast<std::uint16_t>(PlanApplicability::retired)) {
    return fail_as<Plan>(ErrorCode::malformed_input, "plan applicability is out of range");
  }
  plan.state = static_cast<PlanState>(state);
  plan.applicability = static_cast<PlanApplicability>(applicability);
  if (!r.str(text, Limits::max_name_length)) return fail_as<Plan>(ErrorCode::malformed_input, "attempt id is malformed");
  const auto attempt = AttemptId::parse(text);
  if (!attempt) return fail_as<Plan>(ErrorCode::malformed_input, "attempt id is invalid");
  plan.attempt = *attempt;
  if (!r.u64(value)) return fail_as<Plan>(ErrorCode::truncated_input, "attempt generation is truncated");
  const auto attempt_generation = AttemptGeneration::parse(value);
  if (!attempt_generation) return fail_as<Plan>(ErrorCode::invalid_generation, "attempt generation is zero");
  plan.attempt_generation = *attempt_generation;
  if (!r.str(text, Limits::max_name_length)) return fail_as<Plan>(ErrorCode::malformed_input, "publisher id is malformed");
  const auto publisher = PublisherId::parse(text);
  if (!publisher) return fail_as<Plan>(ErrorCode::malformed_input, "publisher id is invalid");
  plan.publisher = *publisher;
  if (!r.raw(plan.publisher_boot.bytes.data(), plan.publisher_boot.bytes.size())) {
    return fail_as<Plan>(ErrorCode::truncated_input, "publisher boot id is truncated");
  }
  if (!plan.publisher_boot.valid()) {
    return fail_as<Plan>(ErrorCode::malformed_input, "publisher boot id is not a valid incarnation");
  }
  if (!r.u64(value)) return fail_as<Plan>(ErrorCode::truncated_input, "coordinator incarnation is truncated");
  const auto incarnation = CoordinatorIncarnation::parse(value);
  if (!incarnation) return fail_as<Plan>(ErrorCode::invalid_generation, "coordinator incarnation is zero");
  plan.coordinator_incarnation = *incarnation;
  if (!read_authority(r, plan.authority)) {
    return fail_as<Plan>(ErrorCode::malformed_input, "the plan authority vector is malformed");
  }
  if (!plan.authority.valid()) {
    return fail_as<Plan>(ErrorCode::invalid_generation, "the plan authority vector is incomplete");
  }

  std::uint32_t count = 0;
  if (!r.u32(count)) return fail_as<Plan>(ErrorCode::truncated_input, "objective component count is truncated");
  if (count > 16) return fail_as<Plan>(ErrorCode::oversized_input, "objective component count exceeds the bound");
  for (std::uint32_t i = 0; i < count; ++i) {
    ObjectiveComponent component;
    std::uint16_t term = 0;
    if (!r.u16(term)) return fail_as<Plan>(ErrorCode::truncated_input, "objective term is truncated");
    if (term > static_cast<std::uint16_t>(ObjectiveTerm::minimize_path_count)) {
      return fail_as<Plan>(ErrorCode::malformed_input, "objective term is out of range");
    }
    component.term = static_cast<ObjectiveTerm>(term);
    if (!r.i64(component.weight)) return fail_as<Plan>(ErrorCode::truncated_input, "objective weight is truncated");
    if (!r.i64(component.raw)) return fail_as<Plan>(ErrorCode::truncated_input, "objective raw value is truncated");
    if (!r.i64(component.weighted)) return fail_as<Plan>(ErrorCode::truncated_input, "objective weighted value is truncated");
    component.unit = std::string(to_string(component.term));
    plan.components.push_back(std::move(component));
  }

  if (!r.u32(count)) return fail_as<Plan>(ErrorCode::truncated_input, "alternative count is truncated");
  if (count > Limits::max_alternatives) return fail_as<Plan>(ErrorCode::oversized_input, "alternative count exceeds the bound");
  for (std::uint32_t i = 0; i < count; ++i) {
    RejectedAlternative alternative;
    if (!r.str(alternative.subject) || !r.str(alternative.reason)) {
      return fail_as<Plan>(ErrorCode::malformed_input, "alternative entry is malformed");
    }
    if (!r.i64(alternative.delta)) return fail_as<Plan>(ErrorCode::truncated_input, "alternative delta is truncated");
    plan.alternatives.push_back(std::move(alternative));
  }

  if (!r.str(plan.feasibility.summary)) return fail_as<Plan>(ErrorCode::malformed_input, "feasibility summary is malformed");
  std::uint16_t status = 0;
  if (!r.u16(status)) return fail_as<Plan>(ErrorCode::truncated_input, "feasibility status is truncated");
  if (status > static_cast<std::uint16_t>(FeasibilityStatus::solver_limit_reached)) {
    return fail_as<Plan>(ErrorCode::malformed_input, "feasibility status is out of range");
  }
  plan.feasibility.status = static_cast<FeasibilityStatus>(status);
  if (!r.u32(count)) return fail_as<Plan>(ErrorCode::truncated_input, "binding constraint count is truncated");
  if (count > Limits::max_explanation_entries) {
    return fail_as<Plan>(ErrorCode::oversized_input, "binding constraint count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    BindingConstraint constraint;
    std::uint16_t kind = 0;
    if (!r.u16(kind)) return fail_as<Plan>(ErrorCode::truncated_input, "constraint kind is truncated");
    if (kind > static_cast<std::uint16_t>(ConstraintKind::min_cut_certificate)) {
      return fail_as<Plan>(ErrorCode::malformed_input, "constraint kind is out of range");
    }
    constraint.kind = static_cast<ConstraintKind>(kind);
    if (!r.str(constraint.subject)) return fail_as<Plan>(ErrorCode::malformed_input, "constraint subject is malformed");
    if (!r.str(text, Limits::max_name_length)) return fail_as<Plan>(ErrorCode::malformed_input, "constraint demand is malformed");
    if (!text.empty()) {
      const auto demand_id = DemandId::parse(text);
      if (!demand_id) return fail_as<Plan>(ErrorCode::malformed_input, "constraint demand id is invalid");
      constraint.demand = *demand_id;
    }
    if (!r.str(text, Limits::max_name_length)) return fail_as<Plan>(ErrorCode::malformed_input, "constraint resource is malformed");
    if (!text.empty()) {
      const auto resource_id = ResourceId::parse(text);
      if (!resource_id) return fail_as<Plan>(ErrorCode::malformed_input, "constraint resource id is invalid");
      constraint.resource = *resource_id;
    }
    if (!r.str(text, Limits::max_name_length)) return fail_as<Plan>(ErrorCode::malformed_input, "constraint path is malformed");
    if (!text.empty()) {
      const auto path_id = PathId::parse(text);
      if (!path_id) return fail_as<Plan>(ErrorCode::malformed_input, "constraint path id is invalid");
      constraint.path = *path_id;
    }
    if (!r.i64(constraint.required)) return fail_as<Plan>(ErrorCode::truncated_input, "constraint required is truncated");
    if (!r.i64(constraint.available)) return fail_as<Plan>(ErrorCode::truncated_input, "constraint available is truncated");
    if (!r.i64(constraint.slack)) return fail_as<Plan>(ErrorCode::truncated_input, "constraint slack is truncated");
    if (!r.str(constraint.detail)) return fail_as<Plan>(ErrorCode::malformed_input, "constraint detail is malformed");
    plan.feasibility.binding.push_back(std::move(constraint));
  }

  if (!r.u32(count)) return fail_as<Plan>(ErrorCode::truncated_input, "allocation demand count is truncated");
  if (count > Limits::max_demands) return fail_as<Plan>(ErrorCode::oversized_input, "allocation demand count exceeds the bound");
  for (std::uint32_t i = 0; i < count; ++i) {
    DemandAllocation demand;
    if (!r.str(text, Limits::max_name_length)) return fail_as<Plan>(ErrorCode::malformed_input, "allocation demand id is malformed");
    const auto demand_id = DemandId::parse(text);
    if (!demand_id) return fail_as<Plan>(ErrorCode::malformed_input, "allocation demand id is invalid");
    demand.demand = *demand_id;
    if (!r.u64(value)) return fail_as<Plan>(ErrorCode::truncated_input, "allocation demand generation is truncated");
    const auto demand_generation = DemandGeneration::parse(value);
    if (!demand_generation) return fail_as<Plan>(ErrorCode::invalid_generation, "allocation demand generation is zero");
    demand.generation = *demand_generation;
    if (!r.i64(demand.minimum) || !r.i64(demand.desired) || !r.i64(demand.maximum) ||
        !r.i64(demand.granted) || !r.i64(demand.reserved) || !r.i64(demand.effective) ||
        !r.i64(demand.observed_applied)) {
      return fail_as<Plan>(ErrorCode::truncated_input, "allocation demand quantities are truncated");
    }
    if (!r.boolean(demand.applied_known)) return fail_as<Plan>(ErrorCode::truncated_input, "allocation applied flag is truncated");
    if (!r.i64(demand.shortfall_against_minimum) || !r.i64(demand.shortfall_against_desired)) {
      return fail_as<Plan>(ErrorCode::truncated_input, "allocation shortfalls are truncated");
    }
    std::uint32_t shares = 0;
    if (!r.u32(shares)) return fail_as<Plan>(ErrorCode::truncated_input, "path share count is truncated");
    if (shares > Limits::max_paths_per_demand) {
      return fail_as<Plan>(ErrorCode::oversized_input, "path share count exceeds the bound");
    }
    for (std::uint32_t j = 0; j < shares; ++j) {
      PathShare share;
      if (!r.str(text, Limits::max_name_length)) return fail_as<Plan>(ErrorCode::malformed_input, "path share id is malformed");
      const auto path_id = PathId::parse(text);
      if (!path_id) return fail_as<Plan>(ErrorCode::malformed_input, "path share id is invalid");
      share.path = *path_id;
      if (!r.u64(value)) return fail_as<Plan>(ErrorCode::truncated_input, "path share generation is truncated");
      const auto path_generation = PathGeneration::parse(value);
      if (!path_generation) return fail_as<Plan>(ErrorCode::invalid_generation, "path share generation is zero");
      share.generation = *path_generation;
      if (!r.i64(share.minimum) || !r.i64(share.desired) || !r.i64(share.granted) ||
          !r.i64(share.reserved) || !r.i64(share.delta_from_incumbent)) {
        return fail_as<Plan>(ErrorCode::truncated_input, "path share quantities are truncated");
      }
      if (!r.boolean(share.newly_used)) return fail_as<Plan>(ErrorCode::truncated_input, "path share flag is truncated");
      demand.shares.push_back(std::move(share));
    }
    plan.allocation.demands.push_back(std::move(demand));
  }

  if (!r.u32(count)) return fail_as<Plan>(ErrorCode::truncated_input, "allocation resource count is truncated");
  if (count > Limits::max_resources) return fail_as<Plan>(ErrorCode::oversized_input, "allocation resource count exceeds the bound");
  for (std::uint32_t i = 0; i < count; ++i) {
    ResourceUtilization resource;
    if (!r.str(text, Limits::max_name_length)) return fail_as<Plan>(ErrorCode::malformed_input, "allocation resource id is malformed");
    const auto resource_id = ResourceId::parse(text);
    if (!resource_id) return fail_as<Plan>(ErrorCode::malformed_input, "allocation resource id is invalid");
    resource.resource = *resource_id;
    if (!r.u64(value)) return fail_as<Plan>(ErrorCode::truncated_input, "allocation resource generation is truncated");
    const auto resource_generation = ResourceGeneration::parse(value);
    if (!resource_generation) return fail_as<Plan>(ErrorCode::invalid_generation, "allocation resource generation is zero");
    resource.generation = *resource_generation;
    if (!r.i64(resource.usable_capacity) || !r.i64(resource.committed_load) ||
        !r.i64(resource.reserved) || !r.i64(resource.available) || !r.i64(resource.allocated) ||
        !r.i64(resource.headroom) || !r.i64(resource.utilization_permille)) {
      return fail_as<Plan>(ErrorCode::truncated_input, "allocation resource quantities are truncated");
    }
    if (!r.boolean(resource.saturated)) return fail_as<Plan>(ErrorCode::truncated_input, "allocation resource flag is truncated");
    plan.allocation.resources.push_back(std::move(resource));
  }

  if (!r.i64(plan.allocation.total_granted) || !r.i64(plan.allocation.total_reserved)) {
    return fail_as<Plan>(ErrorCode::truncated_input, "allocation totals are truncated");
  }
  if (!r.boolean(plan.allocation.degraded)) return fail_as<Plan>(ErrorCode::truncated_input, "allocation degraded flag is truncated");

  std::uint8_t decision = 0;
  if (!r.u8(decision)) return fail_as<Plan>(ErrorCode::truncated_input, "churn decision is truncated");
  if (decision > static_cast<std::uint8_t>(ChurnDecision::reject_regression)) {
    return fail_as<Plan>(ErrorCode::malformed_input, "churn decision is out of range");
  }
  plan.churn.decision = static_cast<ChurnDecision>(decision);
  if (!r.i64(plan.churn.incumbent_score) || !r.i64(plan.churn.proposal_score) ||
      !r.i64(plan.churn.improvement_permille) || !r.i64(plan.churn.moved_bandwidth) ||
      !r.i64(plan.churn.moved_permille)) {
    return fail_as<Plan>(ErrorCode::truncated_input, "churn quantities are truncated");
  }
  if (!r.u32(plan.churn.demands_changed) || !r.u32(plan.churn.paths_added) ||
      !r.u32(plan.churn.paths_removed) || !r.u32(plan.churn.failure_domains_changed)) {
    return fail_as<Plan>(ErrorCode::truncated_input, "churn counters are truncated");
  }
  if (!r.i64(plan.churn.operational_risk_permille)) return fail_as<Plan>(ErrorCode::truncated_input, "churn risk is truncated");
  if (!r.str(plan.churn.rationale)) return fail_as<Plan>(ErrorCode::malformed_input, "churn rationale is malformed");
  if (!r.i64(plan.objective_score)) return fail_as<Plan>(ErrorCode::truncated_input, "objective score is truncated");
  if (!r.str(text, Limits::max_name_length)) return fail_as<Plan>(ErrorCode::malformed_input, "explanation id is malformed");
  if (!text.empty()) {
    const auto explanation_id = ExplanationId::parse(text);
    if (!explanation_id) return fail_as<Plan>(ErrorCode::malformed_input, "explanation id is invalid");
    plan.explanation = *explanation_id;
  }
  if (!r.digest(plan.explanation_digest)) return fail_as<Plan>(ErrorCode::truncated_input, "explanation digest is truncated");

  if (!r.str(text, Limits::max_name_length)) return fail_as<Plan>(ErrorCode::malformed_input, "supersedes id is malformed");
  if (!text.empty()) {
    const auto supersedes_id = PlanId::parse(text);
    if (!supersedes_id) return fail_as<Plan>(ErrorCode::malformed_input, "supersedes id is invalid");
    plan.supersedes.id = *supersedes_id;
  }
  if (!r.u64(value)) return fail_as<Plan>(ErrorCode::truncated_input, "supersedes generation is truncated");
  if (plan.supersedes.id.valid()) {
    const auto supersedes_generation = PlanGeneration::parse(value);
    if (!supersedes_generation) return fail_as<Plan>(ErrorCode::invalid_generation, "supersedes generation is zero");
    plan.supersedes.generation = *supersedes_generation;
  }
  if (!r.str(text, Limits::max_name_length)) return fail_as<Plan>(ErrorCode::malformed_input, "superseded-by id is malformed");
  if (!text.empty()) {
    const auto superseded_id = PlanId::parse(text);
    if (!superseded_id) return fail_as<Plan>(ErrorCode::malformed_input, "superseded-by id is invalid");
    plan.superseded_by.id = *superseded_id;
  }
  if (!r.u64(value)) return fail_as<Plan>(ErrorCode::truncated_input, "superseded-by generation is truncated");
  if (plan.superseded_by.id.valid()) {
    const auto superseded_generation = PlanGeneration::parse(value);
    if (!superseded_generation) return fail_as<Plan>(ErrorCode::invalid_generation, "superseded-by generation is zero");
    plan.superseded_by.generation = *superseded_generation;
  }
  if (!r.str(text, Limits::max_name_length)) return fail_as<Plan>(ErrorCode::malformed_input, "commit id is malformed");
  if (!text.empty()) {
    const auto commit_id = CommitId::parse(text);
    if (!commit_id) return fail_as<Plan>(ErrorCode::malformed_input, "commit id is invalid");
    plan.commit = *commit_id;
  }
  if (!r.u64(value)) return fail_as<Plan>(ErrorCode::truncated_input, "commit generation is truncated");
  if (plan.commit.valid()) {
    const auto commit_generation = CommitGeneration::parse(value);
    if (!commit_generation) return fail_as<Plan>(ErrorCode::invalid_generation, "commit generation is zero");
    plan.commit_generation = *commit_generation;
  }
  if (!r.u64(plan.committed_tick) || !r.u64(plan.declared_tick) || !r.u64(plan.proposed_tick)) {
    return fail_as<Plan>(ErrorCode::truncated_input, "plan ticks are truncated");
  }
  if (r.failed()) return fail_as<Plan>(ErrorCode::malformed_input, "plan record is malformed");
  if (!r.at_end()) return fail_as<Plan>(ErrorCode::trailing_input, "plan record has trailing bytes");
  return plan;
}

std::vector<std::byte> encode_policy(const Policy& policy) {
  Writer w;
  w.u16(static_cast<std::uint16_t>(kDurableFormatVersion));
  w.str(policy.id.str());
  w.u64(policy.generation.value());
  w.boolean(policy.allow_preemption);
  w.boolean(policy.require_minimums);
  w.boolean(policy.allow_degraded_commit);
  w.boolean(policy.require_failure_domain_diversity);
  w.i64(policy.max_utilization_permille);
  w.i64(policy.churn_improvement_threshold_permille);
  w.i64(policy.churn_max_moved_bandwidth_permille);
  w.i64(policy.churn_max_operational_risk_permille);
  w.u32(policy.churn_max_affected_demands);
  w.u32(policy.max_paths_per_demand);
  w.u32(static_cast<std::uint32_t>(policy.allowed_tenants.size()));
  for (const auto& tenant : policy.allowed_tenants) w.str(tenant.str());
  w.u32(static_cast<std::uint32_t>(policy.allowed_service_classes.size()));
  for (const auto& service_class : policy.allowed_service_classes) w.str(service_class.str());
  return w.bytes();
}

Result<Policy> decode_policy(std::span<const std::byte> payload) {
  Reader r(payload);
  Policy policy;
  std::uint16_t version = 0;
  if (!r.u16(version)) return fail_as<Policy>(ErrorCode::truncated_input, "policy record version is truncated");
  if (version != kDurableFormatVersion) return fail_as<Policy>(ErrorCode::unsupported_version, "unsupported policy record version");
  std::string text;
  if (!r.str(text, Limits::max_name_length)) return fail_as<Policy>(ErrorCode::malformed_input, "policy id is malformed");
  const auto id = PolicyId::parse(text);
  if (!id) return fail_as<Policy>(ErrorCode::malformed_input, "policy id is invalid");
  policy.id = *id;
  std::uint64_t generation = 0;
  if (!r.u64(generation)) return fail_as<Policy>(ErrorCode::truncated_input, "policy generation is truncated");
  const auto gen = PolicyGeneration::parse(generation);
  if (!gen) return fail_as<Policy>(ErrorCode::invalid_generation, "policy generation is zero");
  policy.generation = *gen;
  if (!r.boolean(policy.allow_preemption) || !r.boolean(policy.require_minimums) ||
      !r.boolean(policy.allow_degraded_commit) || !r.boolean(policy.require_failure_domain_diversity)) {
    return fail_as<Policy>(ErrorCode::malformed_input, "policy switches are malformed");
  }
  if (!r.i64(policy.max_utilization_permille) || !r.i64(policy.churn_improvement_threshold_permille) ||
      !r.i64(policy.churn_max_moved_bandwidth_permille) || !r.i64(policy.churn_max_operational_risk_permille)) {
    return fail_as<Policy>(ErrorCode::truncated_input, "policy thresholds are truncated");
  }
  if (!r.u32(policy.churn_max_affected_demands) || !r.u32(policy.max_paths_per_demand)) {
    return fail_as<Policy>(ErrorCode::truncated_input, "policy bounds are truncated");
  }
  std::uint32_t count = 0;
  if (!r.u32(count)) return fail_as<Policy>(ErrorCode::truncated_input, "policy tenant count is truncated");
  if (count > Limits::max_constraint_refs * 4u) return fail_as<Policy>(ErrorCode::oversized_input, "policy tenant count exceeds the bound");
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!r.str(text, Limits::max_name_length)) return fail_as<Policy>(ErrorCode::malformed_input, "policy tenant id is malformed");
    const auto tenant = TenantId::parse(text);
    if (!tenant) return fail_as<Policy>(ErrorCode::malformed_input, "policy tenant id is invalid");
    policy.allowed_tenants.push_back(*tenant);
  }
  if (!r.u32(count)) return fail_as<Policy>(ErrorCode::truncated_input, "policy service-class count is truncated");
  if (count > Limits::max_constraint_refs * 4u) return fail_as<Policy>(ErrorCode::oversized_input, "policy service-class count exceeds the bound");
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!r.str(text, Limits::max_name_length)) return fail_as<Policy>(ErrorCode::malformed_input, "policy service class is malformed");
    const auto service_class = ServiceClassId::parse(text);
    if (!service_class) return fail_as<Policy>(ErrorCode::malformed_input, "policy service class is invalid");
    policy.allowed_service_classes.push_back(*service_class);
  }
  if (r.failed()) return fail_as<Policy>(ErrorCode::malformed_input, "policy record is malformed");
  if (!r.at_end()) return fail_as<Policy>(ErrorCode::trailing_input, "policy record has trailing bytes");
  return policy;
}

std::vector<std::byte> encode_objective_profile(const ObjectiveProfile& profile) {
  Writer w;
  w.u16(static_cast<std::uint16_t>(kDurableFormatVersion));
  w.str(profile.id.str());
  w.u64(profile.generation.value());
  w.u32(static_cast<std::uint32_t>(profile.terms.size()));
  for (const auto& term : profile.terms) {
    w.u16(static_cast<std::uint16_t>(term.term));
    w.i64(term.weight);
  }
  return w.bytes();
}

Result<ObjectiveProfile> decode_objective_profile(std::span<const std::byte> payload) {
  Reader r(payload);
  ObjectiveProfile profile;
  std::uint16_t version = 0;
  if (!r.u16(version)) return fail_as<ObjectiveProfile>(ErrorCode::truncated_input, "objective record version is truncated");
  if (version != kDurableFormatVersion) return fail_as<ObjectiveProfile>(ErrorCode::unsupported_version, "unsupported objective record version");
  std::string text;
  if (!r.str(text, Limits::max_name_length)) return fail_as<ObjectiveProfile>(ErrorCode::malformed_input, "objective id is malformed");
  const auto id = ObjectiveProfileId::parse(text);
  if (!id) return fail_as<ObjectiveProfile>(ErrorCode::malformed_input, "objective id is invalid");
  profile.id = *id;
  std::uint64_t generation = 0;
  if (!r.u64(generation)) return fail_as<ObjectiveProfile>(ErrorCode::truncated_input, "objective generation is truncated");
  const auto gen = ObjectiveProfileGeneration::parse(generation);
  if (!gen) return fail_as<ObjectiveProfile>(ErrorCode::invalid_generation, "objective generation is zero");
  profile.generation = *gen;
  std::uint32_t count = 0;
  if (!r.u32(count)) return fail_as<ObjectiveProfile>(ErrorCode::truncated_input, "objective term count is truncated");
  if (count > 16) return fail_as<ObjectiveProfile>(ErrorCode::oversized_input, "objective term count exceeds the bound");
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint16_t ordinal = 0;
    std::int64_t weight = 0;
    if (!r.u16(ordinal)) return fail_as<ObjectiveProfile>(ErrorCode::truncated_input, "objective term is truncated");
    if (!r.i64(weight)) return fail_as<ObjectiveProfile>(ErrorCode::truncated_input, "objective weight is truncated");
    if (ordinal > static_cast<std::uint16_t>(ObjectiveTerm::minimize_path_count)) {
      return fail_as<ObjectiveProfile>(ErrorCode::malformed_input, "objective term is out of range");
    }
    ObjectiveWeight entry;
    entry.term = static_cast<ObjectiveTerm>(ordinal);
    entry.weight = weight;
    profile.terms.push_back(entry);
  }
  if (r.failed()) return fail_as<ObjectiveProfile>(ErrorCode::malformed_input, "objective record is malformed");
  if (!r.at_end()) return fail_as<ObjectiveProfile>(ErrorCode::trailing_input, "objective record has trailing bytes");
  return profile;
}

std::vector<std::byte> encode_fence(const FenceRecord& fence) {
  Writer w;
  w.u16(static_cast<std::uint16_t>(kDurableFormatVersion));
  w.str(fence.publisher.str());
  w.raw(fence.boot.bytes.data(), fence.boot.bytes.size());
  w.u64(fence.incarnation.value());
  w.str(fence.reason);
  w.u64(fence.tick);
  return w.bytes();
}

Result<FenceRecord> decode_fence(std::span<const std::byte> payload) {
  Reader r(payload);
  FenceRecord fence;
  std::uint16_t version = 0;
  if (!r.u16(version)) return fail_as<FenceRecord>(ErrorCode::truncated_input, "fence record version is truncated");
  if (version != kDurableFormatVersion) return fail_as<FenceRecord>(ErrorCode::unsupported_version, "unsupported fence record version");
  std::string text;
  if (!r.str(text, Limits::max_name_length)) return fail_as<FenceRecord>(ErrorCode::malformed_input, "fence publisher is malformed");
  const auto publisher = PublisherId::parse(text);
  if (!publisher) return fail_as<FenceRecord>(ErrorCode::malformed_input, "fence publisher is invalid");
  fence.publisher = *publisher;
  if (!r.raw(fence.boot.bytes.data(), fence.boot.bytes.size())) {
    return fail_as<FenceRecord>(ErrorCode::truncated_input, "fence boot id is truncated");
  }
  if (!fence.boot.valid()) return fail_as<FenceRecord>(ErrorCode::malformed_input, "fence boot id is invalid");
  std::uint64_t incarnation = 0;
  if (!r.u64(incarnation)) return fail_as<FenceRecord>(ErrorCode::truncated_input, "fence incarnation is truncated");
  const auto parsed_incarnation = CoordinatorIncarnation::parse(incarnation);
  if (!parsed_incarnation) return fail_as<FenceRecord>(ErrorCode::invalid_generation, "fence incarnation is zero");
  fence.incarnation = *parsed_incarnation;
  if (!r.str(fence.reason)) return fail_as<FenceRecord>(ErrorCode::malformed_input, "fence reason is malformed");
  if (!r.u64(fence.tick)) return fail_as<FenceRecord>(ErrorCode::truncated_input, "fence tick is truncated");
  if (r.failed()) return fail_as<FenceRecord>(ErrorCode::malformed_input, "fence record is malformed");
  if (!r.at_end()) return fail_as<FenceRecord>(ErrorCode::trailing_input, "fence record has trailing bytes");
  return fence;
}

std::vector<std::byte> encode_coordinator_epoch(const CoordinatorEpochRecord& record) {
  Writer w;
  w.u16(static_cast<std::uint16_t>(kDurableFormatVersion));
  w.str(record.coordinator.str());
  w.u64(record.incarnation.value());
  w.raw(record.boot.bytes.data(), record.boot.bytes.size());
  w.u64(record.epoch);
  return w.bytes();
}

Result<CoordinatorEpochRecord> decode_coordinator_epoch(std::span<const std::byte> payload) {
  Reader r(payload);
  CoordinatorEpochRecord record;
  std::uint16_t version = 0;
  if (!r.u16(version)) return fail_as<CoordinatorEpochRecord>(ErrorCode::truncated_input, "epoch record version is truncated");
  if (version != kDurableFormatVersion) return fail_as<CoordinatorEpochRecord>(ErrorCode::unsupported_version, "unsupported epoch record version");
  std::string text;
  if (!r.str(text, Limits::max_name_length)) return fail_as<CoordinatorEpochRecord>(ErrorCode::malformed_input, "coordinator id is malformed");
  const auto coordinator = CoordinatorId::parse(text);
  if (!coordinator) return fail_as<CoordinatorEpochRecord>(ErrorCode::malformed_input, "coordinator id is invalid");
  record.coordinator = *coordinator;
  std::uint64_t incarnation = 0;
  if (!r.u64(incarnation)) return fail_as<CoordinatorEpochRecord>(ErrorCode::truncated_input, "epoch incarnation is truncated");
  const auto parsed_incarnation = CoordinatorIncarnation::parse(incarnation);
  if (!parsed_incarnation) return fail_as<CoordinatorEpochRecord>(ErrorCode::invalid_generation, "epoch incarnation is zero");
  record.incarnation = *parsed_incarnation;
  if (!r.raw(record.boot.bytes.data(), record.boot.bytes.size())) {
    return fail_as<CoordinatorEpochRecord>(ErrorCode::truncated_input, "epoch boot id is truncated");
  }
  if (!record.boot.valid()) return fail_as<CoordinatorEpochRecord>(ErrorCode::malformed_input, "epoch boot id is invalid");
  if (!r.u64(record.epoch)) return fail_as<CoordinatorEpochRecord>(ErrorCode::truncated_input, "epoch value is truncated");
  if (record.epoch == 0) return fail_as<CoordinatorEpochRecord>(ErrorCode::invalid_generation, "epoch value is zero");
  if (r.failed()) return fail_as<CoordinatorEpochRecord>(ErrorCode::malformed_input, "epoch record is malformed");
  if (!r.at_end()) return fail_as<CoordinatorEpochRecord>(ErrorCode::trailing_input, "epoch record has trailing bytes");
  return record;
}

std::vector<std::byte> encode_audit(const AuditRecord& record) {
  Writer w;
  w.u16(static_cast<std::uint16_t>(kDurableFormatVersion));
  w.u64(record.tick);
  w.str(record.event);
  w.str(record.detail);
  w.digest(record.subject);
  return w.bytes();
}

Result<AuditRecord> decode_audit(std::span<const std::byte> payload) {
  Reader r(payload);
  AuditRecord record;
  std::uint16_t version = 0;
  if (!r.u16(version)) return fail_as<AuditRecord>(ErrorCode::truncated_input, "audit record version is truncated");
  if (version != kDurableFormatVersion) return fail_as<AuditRecord>(ErrorCode::unsupported_version, "unsupported audit record version");
  if (!r.u64(record.tick)) return fail_as<AuditRecord>(ErrorCode::truncated_input, "audit tick is truncated");
  if (!r.str(record.event)) return fail_as<AuditRecord>(ErrorCode::malformed_input, "audit event is malformed");
  if (!r.str(record.detail)) return fail_as<AuditRecord>(ErrorCode::malformed_input, "audit detail is malformed");
  if (!r.digest(record.subject)) return fail_as<AuditRecord>(ErrorCode::truncated_input, "audit subject is truncated");
  if (r.failed()) return fail_as<AuditRecord>(ErrorCode::malformed_input, "audit record is malformed");
  if (!r.at_end()) return fail_as<AuditRecord>(ErrorCode::trailing_input, "audit record has trailing bytes");
  return record;
}

}  // namespace

namespace tef {

struct PlanRepository::Impl {
  mutable std::mutex mutex;
  Options options;
  File journal;
  std::vector<DurableRecord> records;
  std::vector<FenceRecord> fences;
  RecoveryReport recovery;
  Counters counters;
  std::uint64_t last_sequence = 0;
  std::uint64_t snapshot_sequence = 0;
  std::uint64_t coordinator_epoch_value = 0;
  Digest last_record_digest;
  std::uint64_t records_since_compaction = 0;
  bool closed = false;
};

PlanRepository::PlanRepository() : impl_(std::make_unique<Impl>()) {}
PlanRepository::~PlanRepository() = default;

namespace {

std::string describe_error(const std::string& context, const std::string& detail) {
  if (detail.empty()) return context;
  return context + ": " + detail;
}

std::vector<std::byte> make_journal_header(std::uint64_t start_sequence) {
  std::vector<std::byte> header(kJournalHeaderSize);
  std::uint8_t* out = reinterpret_cast<std::uint8_t*>(header.data());
  write_u32(out, kJournalMagic);
  write_u16(out + 4, static_cast<std::uint16_t>(kDurableFormatVersion));
  write_u16(out + 6, 0);
  write_u64(out + 8, start_sequence);
  write_u64(out + 16, 0);
  const std::uint32_t checksum =
      crc32c(std::span<const std::byte>(header.data(), 24));
  write_u32(out + 24, checksum);
  write_u32(out + 28, 0);
  return header;
}

bool parse_journal_header(const std::vector<std::byte>& header, std::uint64_t& start_sequence,
                          std::string& error) {
  if (header.size() != kJournalHeaderSize) {
    error = "journal header is truncated";
    return false;
  }
  const std::uint8_t* in = reinterpret_cast<const std::uint8_t*>(header.data());
  if (read_u32(in) != kJournalMagic) {
    error = "journal magic does not match";
    return false;
  }
  if (read_u16(in + 4) != static_cast<std::uint16_t>(kDurableFormatVersion)) {
    error = "unsupported journal format version";
    return false;
  }
  if (crc32c(std::span<const std::byte>(header.data(), 24)) != read_u32(in + 24)) {
    error = "journal header checksum does not match";
    return false;
  }
  start_sequence = read_u64(in + 8);
  return true;
}

std::vector<std::byte> make_manifest() {
  std::vector<std::byte> manifest(kManifestSize);
  std::uint8_t* out = reinterpret_cast<std::uint8_t*>(manifest.data());
  write_u32(out, kManifestMagic);
  write_u16(out + 4, static_cast<std::uint16_t>(kDurableFormatVersion));
  write_u16(out + 6, 0);
  write_u64(out + 8, 0);
  return manifest;
}

bool write_atomic(const std::filesystem::path& target, const std::filesystem::path& temporary,
                  const std::vector<std::byte>& bytes, std::string& error) {
  File file;
  if (!file.open_write(temporary)) {
    error = "cannot create " + temporary.string();
    return false;
  }
  if (!file.write(bytes.data(), bytes.size())) {
    error = "cannot write " + temporary.string();
    file.close();
    return false;
  }
  if (!file.flush()) {
    error = "cannot flush " + temporary.string();
    file.close();
    return false;
  }
  file.close();
  return atomic_replace(temporary, target, error);
}

bool read_whole_file(const std::filesystem::path& path, std::vector<std::byte>& out,
                     std::string& error) {
  std::error_code code;
  const std::uintmax_t size = std::filesystem::file_size(path, code);
  if (code) {
    error = "cannot stat " + path.string();
    return false;
  }
  if (size > kMaxFileBytes) {
    error = "durable file exceeds the supported size bound: " + path.string();
    return false;
  }
  File file;
  if (!file.open_read(path)) {
    error = "cannot open " + path.string();
    return false;
  }
  out.assign(static_cast<std::size_t>(size), std::byte{0});
  if (size > 0 && !file.read(out.data(), static_cast<std::size_t>(size))) {
    error = "cannot read " + path.string();
    return false;
  }
  return true;
}

}  // namespace

Result<std::unique_ptr<PlanRepository>> PlanRepository::open(const Options& options) {
  if (options.directory.empty()) {
    return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::invalid_argument,
                                                    "a durable directory is required");
  }
  auto repository = std::unique_ptr<PlanRepository>(new PlanRepository());
  PlanRepository::Impl& impl = *repository->impl_;
  impl.options = options;
  impl.options.compact_after_records =
      std::max<std::size_t>(impl.options.compact_after_records, 1);

  std::error_code code;
  std::filesystem::create_directories(options.directory, code);
  if (code) {
    return fail_as<std::unique_ptr<PlanRepository>>(
        ErrorCode::io_failure, "cannot create the durable directory " + options.directory + ": " + code.message());
  }

  const std::filesystem::path directory(options.directory);
  const std::filesystem::path manifest_path = directory / kManifestName;
  const std::filesystem::path snapshot_path = directory / kSnapshotName;
  const std::filesystem::path journal_path = directory / kJournalName;
  const std::filesystem::path snapshot_temp = directory / kSnapshotTempName;
  const std::filesystem::path journal_temp = directory / kJournalTempName;
  const std::filesystem::path manifest_temp = directory / kManifestTempName;

  // An in-progress atomic replacement is discarded: it was never acknowledged.
  std::filesystem::remove(snapshot_temp, code);
  std::filesystem::remove(journal_temp, code);
  std::filesystem::remove(manifest_temp, code);

  std::string error;
  if (std::filesystem::exists(manifest_path)) {
    std::vector<std::byte> manifest;
    if (!read_whole_file(manifest_path, manifest, error)) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::io_failure, error);
    }
    if (manifest.size() != kManifestSize) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::integrity_failure,
                                                      "the durable manifest is malformed");
    }
    const std::uint8_t* in = reinterpret_cast<const std::uint8_t*>(manifest.data());
    if (read_u32(in) != kManifestMagic) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::integrity_failure,
                                                      "the durable manifest magic does not match");
    }
    if (read_u16(in + 4) != static_cast<std::uint16_t>(kDurableFormatVersion)) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::unsupported_version,
                                                      "the durable manifest format version is unsupported");
    }
    impl.recovery.manifest_present = true;
  } else {
    if (!write_atomic(manifest_path, manifest_temp, make_manifest(), error)) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::io_failure,
                                                      describe_error("cannot create the durable manifest", error));
    }
  }

  // ---- Snapshot ----------------------------------------------------------
  if (std::filesystem::exists(snapshot_path)) {
    std::vector<std::byte> bytes;
    if (!read_whole_file(snapshot_path, bytes, error)) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::io_failure, error);
    }
    if (bytes.size() < kSnapshotHeaderSize + kDigestSize) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::integrity_failure,
                                                      "the durable snapshot is truncated");
    }
    const std::uint8_t* in = reinterpret_cast<const std::uint8_t*>(bytes.data());
    if (read_u32(in) != kSnapshotMagic) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::integrity_failure,
                                                      "the durable snapshot magic does not match");
    }
    if (read_u16(in + 4) != static_cast<std::uint16_t>(kDurableFormatVersion)) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::unsupported_version,
                                                      "the durable snapshot format version is unsupported");
    }
    const std::uint64_t snapshot_sequence = read_u64(in + 8);
    const std::uint64_t payload_length = read_u64(in + 24);
    const std::uint32_t payload_crc = read_u32(in + 32);
    if (payload_length > bytes.size() - kSnapshotHeaderSize - kDigestSize) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::integrity_failure,
                                                      "the durable snapshot payload length is inconsistent");
    }
    const auto payload_span =
        std::span<const std::byte>(bytes.data() + kSnapshotHeaderSize, static_cast<std::size_t>(payload_length));
    if (crc32c(payload_span) != payload_crc) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::integrity_failure,
                                                      "the durable snapshot payload checksum does not match");
    }
    Digest stored;
    std::memcpy(stored.bytes.data(), bytes.data() + kSnapshotHeaderSize + payload_length, kDigestSize);
    Digest computed = Sha256::hash(
        std::span<const std::byte>(bytes.data(), kSnapshotHeaderSize + static_cast<std::size_t>(payload_length)));
    if (!(stored == computed)) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::integrity_failure,
                                                      "the durable snapshot digest does not match");
    }

    std::size_t offset = 0;
    Digest previous;
    while (offset < payload_span.size()) {
      if (payload_span.size() - offset < kRecordHeaderSize + kDigestSize) {
        return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::integrity_failure,
                                                        "the durable snapshot contains a torn record");
      }
      const std::uint8_t* record_header =
          reinterpret_cast<const std::uint8_t*>(payload_span.data() + offset);
      if (read_u32(record_header) != kRecordMagic) {
        return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::integrity_failure,
                                                        "the durable snapshot record magic does not match");
      }
      const std::uint32_t length = read_u32(record_header + 8);
      const std::uint64_t sequence = read_u64(record_header + 12);
      const std::size_t total = kRecordHeaderSize + length + kDigestSize;
      if (total > payload_span.size() - offset) {
        return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::integrity_failure,
                                                        "the durable snapshot record length is inconsistent");
      }
      Digest record_previous;
      std::memcpy(record_previous.bytes.data(), record_header + 20, kDigestSize);
      Digest record_stored;
      std::memcpy(record_stored.bytes.data(), payload_span.data() + offset + kRecordHeaderSize + length,
                  kDigestSize);
      Digest record_computed =
          Sha256::hash(std::span<const std::byte>(payload_span.data() + offset, kRecordHeaderSize + length));
      if (!(record_stored == record_computed)) {
        return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::integrity_failure,
                                                        "a durable snapshot record digest does not match");
      }
      if (!(record_previous == previous)) {
        return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::integrity_failure,
                                                        "the durable snapshot hash chain is broken");
      }
      DurableRecord record;
      record.type = static_cast<RecordType>(read_u16(record_header + 6));
      record.sequence = sequence;
      record.previous_digest = record_previous;
      record.record_digest = record_stored;
      record.payload.assign(payload_span.begin() + static_cast<std::ptrdiff_t>(offset + kRecordHeaderSize),
                            payload_span.begin() +
                                static_cast<std::ptrdiff_t>(offset + kRecordHeaderSize + length));
      previous = record_stored;
      impl.records.push_back(std::move(record));
      offset += total;
    }
    impl.snapshot_sequence = snapshot_sequence;
    impl.last_sequence = snapshot_sequence;
    impl.last_record_digest = previous;
    impl.recovery.snapshot_loaded = true;
    impl.recovery.snapshot_sequence = snapshot_sequence;
  }

  // ---- Journal -----------------------------------------------------------
  std::uint64_t journal_start = impl.last_sequence + 1;
  if (std::filesystem::exists(journal_path)) {
    File journal;
    if (!journal.open_read_write(journal_path)) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::io_failure,
                                                      "cannot open the durable journal for recovery");
    }
    const std::uint64_t journal_size = journal.size();
    if (journal_size < kJournalHeaderSize) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::integrity_failure,
                                                      "the durable journal header is truncated");
    }
    std::vector<std::byte> header(kJournalHeaderSize);
    if (!journal.read(header.data(), kJournalHeaderSize)) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::io_failure,
                                                      "cannot read the durable journal header");
    }
    std::uint64_t start_sequence = 0;
    if (!parse_journal_header(header, start_sequence, error)) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::integrity_failure,
                                                      describe_error("the durable journal header is invalid", error));
    }
    journal_start = start_sequence;

    std::uint64_t offset = kJournalHeaderSize;
    std::uint64_t valid_offset = offset;
    Digest previous = impl.records.empty() ? Digest{} : impl.records.back().record_digest;
    while (true) {
      if (journal_size - offset < kRecordHeaderSize + kDigestSize) break;
      if (!journal.seek(offset)) break;
      std::vector<std::byte> frame(kRecordHeaderSize);
      if (!journal.read(frame.data(), kRecordHeaderSize)) break;
      const std::uint8_t* record_header = reinterpret_cast<const std::uint8_t*>(frame.data());
      if (read_u32(record_header) != kRecordMagic) break;
      if (read_u16(record_header + 4) != static_cast<std::uint16_t>(kDurableFormatVersion)) break;
      const std::uint16_t raw_type = read_u16(record_header + 6);
      if (raw_type < static_cast<std::uint16_t>(RecordType::plan) ||
          raw_type > static_cast<std::uint16_t>(RecordType::audit)) {
        break;
      }
      const std::uint32_t length = read_u32(record_header + 8);
      if (length > kMaxRecordPayload) break;
      const std::uint64_t total = static_cast<std::uint64_t>(kRecordHeaderSize) + length + kDigestSize;
      if (total > journal_size - offset) break;

      std::vector<std::byte> body(static_cast<std::size_t>(length) + kDigestSize);
      if (!journal.read(body.data(), body.size())) break;
      const std::uint32_t payload_crc = read_u32(record_header + 52);
      if (crc32c(std::span<const std::byte>(body.data(), length)) != payload_crc) break;

      std::vector<std::byte> image(frame);
      image.insert(image.end(), body.begin(), body.end() - static_cast<std::ptrdiff_t>(kDigestSize));
      Digest computed = Sha256::hash(std::span<const std::byte>(image.data(), image.size()));
      Digest stored;
      std::memcpy(stored.bytes.data(), body.data() + length, kDigestSize);
      if (!(stored == computed)) break;

      Digest record_previous;
      std::memcpy(record_previous.bytes.data(), record_header + 20, kDigestSize);
      if (!(record_previous == previous)) break;

      const std::uint64_t sequence = read_u64(record_header + 12);
      if (sequence <= impl.snapshot_sequence) {
        // Already covered by the snapshot: verified, then skipped.
        previous = stored;
        offset += total;
        valid_offset = offset;
        continue;
      }
      if (sequence != impl.last_sequence + 1) break;

      DurableRecord record;
      record.type = static_cast<RecordType>(raw_type);
      record.sequence = sequence;
      record.previous_digest = record_previous;
      record.record_digest = stored;
      record.payload.assign(image.begin() + static_cast<std::ptrdiff_t>(kRecordHeaderSize), image.end());
      impl.records.push_back(std::move(record));
      impl.last_sequence = sequence;
      impl.last_record_digest = stored;
      previous = stored;
      offset += total;
      valid_offset = offset;
    }

    if (valid_offset < journal_size) {
      const std::uint64_t discarded = journal_size - valid_offset;
      impl.recovery.journal_bytes_truncated = discarded;
      impl.counters.truncated_bytes += discarded;
      if (!journal.truncate_at(valid_offset)) {
        return fail_as<std::unique_ptr<PlanRepository>>(
            ErrorCode::io_failure, "cannot truncate the durable journal at the last valid record");
      }
    }
    journal.close();

    if (!impl.journal.open_append(journal_path)) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::io_failure,
                                                      "cannot reopen the durable journal for appending");
    }
  } else {
    std::vector<std::byte> header = make_journal_header(journal_start);
    if (!write_atomic(journal_path, journal_temp, header, error)) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::io_failure,
                                                      describe_error("cannot create the durable journal", error));
    }
    if (!impl.journal.open_append(journal_path)) {
      return fail_as<std::unique_ptr<PlanRepository>>(ErrorCode::io_failure,
                                                      "cannot open the durable journal for appending");
    }
  }
  (void)journal_start;

  impl.recovery.records_recovered = impl.records.size();
  impl.records_since_compaction = impl.records.size();

  for (const auto& record : impl.records) {
    if (record.type == RecordType::coordinator_epoch) {
      const Result<CoordinatorEpochRecord> decoded = decode_coordinator_epoch(record.payload);
      if (decoded.has_value()) {
        impl.coordinator_epoch_value = std::max(impl.coordinator_epoch_value, decoded.value().epoch);
      }
    } else if (record.type == RecordType::fence) {
      const Result<FenceRecord> decoded = decode_fence(record.payload);
      if (decoded.has_value()) impl.fences.push_back(decoded.value());
    }
  }
  impl.recovery.coordinator_epoch = impl.coordinator_epoch_value;
  ++impl.counters.recoveries;
  return repository;
}

Status PlanRepository::append(RecordType type, std::vector<std::byte> payload) {
  std::unique_lock lock(impl_->mutex);
  if (impl_->closed) return fail(ErrorCode::shutdown, "the durable repository is closed");
  if (impl_->records.size() >= impl_->options.max_records) {
    return fail(ErrorCode::limit_exceeded, "the durable record bound has been reached");
  }
  if (payload.size() > kMaxRecordPayload) {
    return fail(ErrorCode::oversized_input, "the record payload exceeds the supported bound");
  }

  DurableRecord record;
  record.type = type;
  record.sequence = impl_->last_sequence + 1;
  record.previous_digest = impl_->last_record_digest;
  record.payload = std::move(payload);

  std::vector<std::byte> image;
  encode_record_image(record, image);
  if (!impl_->journal.write(image.data(), image.size())) {
    return fail(ErrorCode::io_failure, "the durable journal write failed");
  }
  if (impl_->options.fsync && !impl_->journal.flush()) {
    return fail(ErrorCode::io_failure, "the durable journal could not be synchronised");
  }

  record.record_digest = Sha256::hash(std::span<const std::byte>(
      image.data(), image.size() - kDigestSize));
  impl_->last_sequence = record.sequence;
  impl_->last_record_digest = record.record_digest;
  ++impl_->counters.appends;
  ++impl_->records_since_compaction;

  if (type == RecordType::coordinator_epoch) {
    const Result<CoordinatorEpochRecord> decoded = decode_coordinator_epoch(record.payload);
    if (decoded.has_value()) {
      impl_->coordinator_epoch_value =
          std::max(impl_->coordinator_epoch_value, decoded.value().epoch);
    }
  } else if (type == RecordType::fence) {
    const Result<FenceRecord> decoded = decode_fence(record.payload);
    if (decoded.has_value()) {
      if (impl_->fences.size() >= Limits::max_fences) {
        return fail(ErrorCode::limit_exceeded, "the durable fence bound has been reached");
      }
      impl_->fences.push_back(decoded.value());
    }
  }
  impl_->records.push_back(std::move(record));

  if (impl_->records_since_compaction >= impl_->options.compact_after_records) {
    const Status status = compact_locked();
    if (!status.ok()) return status;
  }
  return Status::success();
}

Status PlanRepository::compact_locked() {
  const std::filesystem::path directory(impl_->options.directory);
  const std::filesystem::path snapshot_path = directory / kSnapshotName;
  const std::filesystem::path journal_path = directory / kJournalName;
  const std::filesystem::path snapshot_temp = directory / kSnapshotTempName;
  const std::filesystem::path journal_temp = directory / kJournalTempName;

  std::vector<std::byte> payload;
  for (const auto& record : impl_->records) {
    std::vector<std::byte> image;
    encode_record_image(record, image);
    payload.insert(payload.end(), image.begin(), image.end());
  }

  std::vector<std::byte> snapshot(kSnapshotHeaderSize);
  std::uint8_t* out = reinterpret_cast<std::uint8_t*>(snapshot.data());
  write_u32(out, kSnapshotMagic);
  write_u16(out + 4, static_cast<std::uint16_t>(kDurableFormatVersion));
  write_u16(out + 6, 0);
  write_u64(out + 8, impl_->last_sequence);
  write_u64(out + 16, impl_->last_sequence + 1);
  write_u64(out + 24, payload.size());
  write_u32(out + 32, crc32c(std::span<const std::byte>(payload.data(), payload.size())));
  write_u32(out + 36, 0);
  snapshot.insert(snapshot.end(), payload.begin(), payload.end());
  Digest digest =
      Sha256::hash(std::span<const std::byte>(snapshot.data(), snapshot.size()));
  snapshot.insert(snapshot.end(), reinterpret_cast<const std::byte*>(digest.bytes.data()),
                  reinterpret_cast<const std::byte*>(digest.bytes.data()) + digest.bytes.size());

  std::string error;
  if (!write_atomic(snapshot_path, snapshot_temp, snapshot, error)) {
    return fail(ErrorCode::io_failure, describe_error("cannot replace the durable snapshot", error));
  }

  // The snapshot now covers every record written so far. The journal is reset
  // to start after it; replaying the old journal would be harmless because every
  // record in it is already covered, but a compact journal keeps recovery cheap.
  impl_->journal.close();
  const std::vector<std::byte> header = make_journal_header(impl_->last_sequence + 1);
  if (!write_atomic(journal_path, journal_temp, header, error)) {
    return fail(ErrorCode::io_failure, describe_error("cannot reset the durable journal", error));
  }
  if (!impl_->journal.open_append(journal_path)) {
    return fail(ErrorCode::io_failure, "cannot reopen the durable journal after compaction");
  }
  impl_->records_since_compaction = 0;
  ++impl_->counters.compactions;
  return Status::success();
}

Status PlanRepository::flush() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->closed) return fail(ErrorCode::shutdown, "the durable repository is closed");
  if (!impl_->journal.flush()) return fail(ErrorCode::io_failure, "the durable journal flush failed");
  return Status::success();
}

Status PlanRepository::compact() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->closed) return fail(ErrorCode::shutdown, "the durable repository is closed");
  return compact_locked();
}

std::vector<DurableRecord> PlanRepository::records() const {
  std::unique_lock lock(impl_->mutex);
  return impl_->records;
}

std::size_t PlanRepository::record_count() const {
  std::unique_lock lock(impl_->mutex);
  return impl_->records.size();
}

std::uint64_t PlanRepository::last_sequence() const {
  std::unique_lock lock(impl_->mutex);
  return impl_->last_sequence;
}

PlanRepository::RecoveryReport PlanRepository::recovery() const {
  std::unique_lock lock(impl_->mutex);
  return impl_->recovery;
}

const std::string& PlanRepository::directory() const { return impl_->options.directory; }

std::uint64_t PlanRepository::coordinator_epoch() const {
  std::unique_lock lock(impl_->mutex);
  return impl_->coordinator_epoch_value;
}

Status PlanRepository::set_coordinator_epoch(std::uint64_t epoch, const CoordinatorEpochRecord& record) {
  if (epoch == 0) return fail(ErrorCode::invalid_generation, "the coordinator epoch must be positive");
  CoordinatorEpochRecord stored = record;
  stored.epoch = epoch;
  return append(RecordType::coordinator_epoch, encode_coordinator_epoch(stored));
}

Status PlanRepository::add_fence(const FenceRecord& fence) {
  if (!fence.publisher.valid() || !fence.boot.valid() || !fence.incarnation.valid()) {
    return fail(ErrorCode::invalid_argument, "the fence record is incomplete");
  }
  return append(RecordType::fence, encode_fence(fence));
}

std::vector<FenceRecord> PlanRepository::fences() const {
  std::unique_lock lock(impl_->mutex);
  return impl_->fences;
}

PlanRepository::Counters PlanRepository::counters() const {
  std::unique_lock lock(impl_->mutex);
  return impl_->counters;
}

Status PlanRepository::close() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->closed) return Status::success();
  impl_->journal.close();
  impl_->closed = true;
  return Status::success();
}

}  // namespace tef
