// Traffic Engineering Fabric - durable, versioned, integrity-checked storage.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Only state that belongs to this runtime is persisted: policies, objective
// profiles, committed plans, supersession lineage, provenance, audit history,
// fencing records and coordinator epoch state. Dynamic evidence (liveness,
// leases, capacity snapshots, path authority results) is never restored as
// fresh; it must be resubmitted by its owner.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "tef/allocation.hpp"
#include "tef/authority.hpp"
#include "tef/diagnostic.hpp"
#include "tef/digest.hpp"
#include "tef/model.hpp"
#include "tef/plan.hpp"

namespace tef {

enum class RecordType : std::uint16_t {
  plan = 1,
  policy = 2,
  objective_profile = 3,
  fence = 4,
  coordinator_epoch = 5,
  audit = 6,
};

std::string_view to_string(RecordType type) noexcept;

struct FenceRecord {
  PublisherId publisher;
  BootId boot;
  CoordinatorIncarnation incarnation;
  std::string reason;
  std::uint64_t tick = 0;
};

struct AuditRecord {
  std::uint64_t tick = 0;
  std::string event;
  std::string detail;
  Digest subject;
};

struct CoordinatorEpochRecord {
  CoordinatorId coordinator;
  CoordinatorIncarnation incarnation;
  BootId boot;
  std::uint64_t epoch = 0;
};

struct DurableRecord {
  RecordType type = RecordType::audit;
  std::uint64_t sequence = 0;
  Digest previous_digest;
  Digest record_digest;
  std::vector<std::byte> payload;
};

// Canonical payload encoders/decoders. Every decoder re-validates structure and
// rejects trailing bytes.
std::vector<std::byte> encode_plan(const Plan& plan);
Result<Plan> decode_plan(std::span<const std::byte> payload);

std::vector<std::byte> encode_policy(const Policy& policy);
Result<Policy> decode_policy(std::span<const std::byte> payload);

std::vector<std::byte> encode_objective_profile(const ObjectiveProfile& profile);
Result<ObjectiveProfile> decode_objective_profile(std::span<const std::byte> payload);

std::vector<std::byte> encode_fence(const FenceRecord& fence);
Result<FenceRecord> decode_fence(std::span<const std::byte> payload);

std::vector<std::byte> encode_coordinator_epoch(const CoordinatorEpochRecord& record);
Result<CoordinatorEpochRecord> decode_coordinator_epoch(std::span<const std::byte> payload);

std::vector<std::byte> encode_audit(const AuditRecord& record);
Result<AuditRecord> decode_audit(std::span<const std::byte> payload);

// ---------------------------------------------------------------------------
// Crash-safe record file
// ---------------------------------------------------------------------------
//
// Layout: manifest + rotating snapshot + append-only journal. Every record is
// framed with a magic, format version, sequence number, chained previous digest
// and a CRC-32C over the payload. A torn or corrupted tail is detected, the
// valid prefix is preserved, and the file is truncated at the last valid record
// rather than silently reinterpreting damaged bytes.
class PlanRepository {
 public:
  struct Options {
    std::string directory;
    std::size_t max_records = Limits::max_journal_records;
    bool fsync = true;
    std::size_t compact_after_records = 4096;
  };

  struct RecoveryReport {
    bool manifest_present = false;
    bool snapshot_loaded = false;
    std::uint64_t snapshot_sequence = 0;
    std::uint64_t records_recovered = 0;
    std::uint64_t records_discarded = 0;
    std::uint64_t journal_bytes_truncated = 0;
    std::uint64_t coordinator_epoch = 0;
    std::vector<std::string> diagnostics;
  };

  static Result<std::unique_ptr<PlanRepository>> open(const Options& options);

  ~PlanRepository();
  PlanRepository(const PlanRepository&) = delete;
  PlanRepository& operator=(const PlanRepository&) = delete;

  // Appends a record durably. Returns only after the record is on stable
  // storage (or after an explicit failure); a caller must never observe a
  // successful return for a record that would not survive the failure boundary.
  Status append(RecordType type, std::vector<std::byte> payload);

  Status flush();
  Status compact();

  std::vector<DurableRecord> records() const;
  std::size_t record_count() const;
  std::uint64_t last_sequence() const;
  RecoveryReport recovery() const;
  const std::string& directory() const;

  // Coordinator epoch: persisted, and advanced on every open so that frames
  // issued by a previous coordinator incarnation are rejected.
  std::uint64_t coordinator_epoch() const;
  Status set_coordinator_epoch(std::uint64_t epoch, const CoordinatorEpochRecord& record);

  // Fences are durable facts: a dead publisher boot identity stays fenced
  // forever, across restarts.
  Status add_fence(const FenceRecord& fence);
  std::vector<FenceRecord> fences() const;

  Status close();

  struct Counters {
    std::uint64_t appends = 0;
    std::uint64_t compactions = 0;
    std::uint64_t recoveries = 0;
    std::uint64_t truncated_bytes = 0;
  };
  Counters counters() const;

 private:
  PlanRepository();
  Status compact_locked();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tef
