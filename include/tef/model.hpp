// Traffic Engineering Fabric - authoritative input model.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The model below describes *inputs owned by adjacent systems* plus the
// traffic-engineering intent this runtime owns. Traffic Engineering Fabric never
// discovers topology, never decides path legality, and never installs routes.
#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tef/diagnostic.hpp"
#include "tef/digest.hpp"
#include "tef/identity.hpp"
#include "tef/limits.hpp"

namespace tef {

// ---------------------------------------------------------------------------
// Identity tags
// ---------------------------------------------------------------------------

struct PlanIdTag { static constexpr std::string_view kind() noexcept { return "plan"; } };
struct DemandIdTag { static constexpr std::string_view kind() noexcept { return "demand"; } };
struct CandidateSetIdTag { static constexpr std::string_view kind() noexcept { return "candidate-set"; } };
struct PathIdTag { static constexpr std::string_view kind() noexcept { return "path"; } };
struct ResourceIdTag { static constexpr std::string_view kind() noexcept { return "resource"; } };
struct CapacitySnapshotIdTag { static constexpr std::string_view kind() noexcept { return "capacity-snapshot"; } };
struct ReservationSnapshotIdTag { static constexpr std::string_view kind() noexcept { return "reservation-snapshot"; } };
struct ReservationIdTag { static constexpr std::string_view kind() noexcept { return "reservation"; } };
struct PolicyIdTag { static constexpr std::string_view kind() noexcept { return "policy"; } };
struct ObjectiveProfileIdTag { static constexpr std::string_view kind() noexcept { return "objective-profile"; } };
struct FailureDomainIdTag { static constexpr std::string_view kind() noexcept { return "failure-domain"; } };
struct TenantIdTag { static constexpr std::string_view kind() noexcept { return "tenant"; } };
struct ServiceClassIdTag { static constexpr std::string_view kind() noexcept { return "service-class"; } };
struct ExplanationIdTag { static constexpr std::string_view kind() noexcept { return "explanation"; } };
struct PublisherIdTag { static constexpr std::string_view kind() noexcept { return "publisher"; } };
struct WorkerIdTag { static constexpr std::string_view kind() noexcept { return "worker"; } };
struct AttemptIdTag { static constexpr std::string_view kind() noexcept { return "attempt"; } };
struct CommitIdTag { static constexpr std::string_view kind() noexcept { return "commit"; } };
struct SourceSystemIdTag { static constexpr std::string_view kind() noexcept { return "source-system"; } };
struct CoordinatorIdTag { static constexpr std::string_view kind() noexcept { return "coordinator"; } };

using PlanId = Id<PlanIdTag>;
using DemandId = Id<DemandIdTag>;
using CandidateSetId = Id<CandidateSetIdTag>;
using PathId = Id<PathIdTag>;
using ResourceId = Id<ResourceIdTag>;
using CapacitySnapshotId = Id<CapacitySnapshotIdTag>;
using ReservationSnapshotId = Id<ReservationSnapshotIdTag>;
using ReservationId = Id<ReservationIdTag>;
using PolicyId = Id<PolicyIdTag>;
using ObjectiveProfileId = Id<ObjectiveProfileIdTag>;
using FailureDomainId = Id<FailureDomainIdTag>;
using TenantId = Id<TenantIdTag>;
using ServiceClassId = Id<ServiceClassIdTag>;
using ExplanationId = Id<ExplanationIdTag>;
using PublisherId = Id<PublisherIdTag>;
using WorkerId = Id<WorkerIdTag>;
using AttemptId = Id<AttemptIdTag>;
using CommitId = Id<CommitIdTag>;
using SourceSystemId = Id<SourceSystemIdTag>;
using CoordinatorId = Id<CoordinatorIdTag>;

struct PlanGenerationTag {};
struct DemandGenerationTag {};
struct CandidateSetGenerationTag {};
struct PathGenerationTag {};
struct CapacitySnapshotGenerationTag {};
struct ReservationSnapshotGenerationTag {};
struct ReservationGenerationTag {};
struct PolicyGenerationTag {};
struct ObjectiveProfileGenerationTag {};
struct FailureDomainGenerationTag {};
struct ResourceGenerationTag {};
struct CommitGenerationTag {};
struct AttemptGenerationTag {};
struct TopologyGenerationTag {};
struct LinkStateGenerationTag {};
struct PathAuthorityGenerationTag {};
struct FabricEpochTag {};
struct CoordinatorIncarnationTag {};

using PlanGeneration = Gen<PlanGenerationTag>;
using DemandGeneration = Gen<DemandGenerationTag>;
using CandidateSetGeneration = Gen<CandidateSetGenerationTag>;
using PathGeneration = Gen<PathGenerationTag>;
using CapacitySnapshotGeneration = Gen<CapacitySnapshotGenerationTag>;
using ReservationSnapshotGeneration = Gen<ReservationSnapshotGenerationTag>;
using ReservationGeneration = Gen<ReservationGenerationTag>;
using PolicyGeneration = Gen<PolicyGenerationTag>;
using ObjectiveProfileGeneration = Gen<ObjectiveProfileGenerationTag>;
using FailureDomainGeneration = Gen<FailureDomainGenerationTag>;
using ResourceGeneration = Gen<ResourceGenerationTag>;
using CommitGeneration = Gen<CommitGenerationTag>;
using AttemptGeneration = Gen<AttemptGenerationTag>;
using TopologyGeneration = Gen<TopologyGenerationTag>;
using LinkStateGeneration = Gen<LinkStateGenerationTag>;
using PathAuthorityGeneration = Gen<PathAuthorityGenerationTag>;
using FabricEpoch = Gen<FabricEpochTag>;
using CoordinatorIncarnation = Gen<CoordinatorIncarnationTag>;

// ---------------------------------------------------------------------------
// Composite references
// ---------------------------------------------------------------------------

template <class IdT, class GenT>
struct Ref {
  IdT id;
  GenT generation;

  bool valid() const noexcept { return id.valid() && generation.valid(); }

  friend bool operator==(const Ref&, const Ref&) noexcept = default;
  friend std::strong_ordering operator<=>(const Ref&, const Ref&) noexcept = default;
};

using CandidateSetRef = Ref<CandidateSetId, CandidateSetGeneration>;
using PathAuthorityRef = Ref<PathId, PathAuthorityGeneration>;
using CapacitySnapshotRef = Ref<CapacitySnapshotId, CapacitySnapshotGeneration>;
using ReservationSnapshotRef = Ref<ReservationSnapshotId, ReservationSnapshotGeneration>;
using PolicyRef = Ref<PolicyId, PolicyGeneration>;
using ObjectiveProfileRef = Ref<ObjectiveProfileId, ObjectiveProfileGeneration>;
using PlanRef = Ref<PlanId, PlanGeneration>;

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------

// Every authoritative input carries provenance. An input without provenance is
// rejected: the runtime never promotes unlabelled evidence into authority.
struct Provenance {
  SourceSystemId source;
  std::uint64_t source_generation = 0;
  std::uint64_t observed_tick = 0;
  Digest evidence_digest;
  std::string detail;

  bool valid() const noexcept { return source.valid(); }

  friend bool operator==(const Provenance&, const Provenance&) noexcept = default;
  friend std::strong_ordering operator<=>(const Provenance&, const Provenance&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

// Half-open integer tick interval [start, end). The runtime never reads a wall
// clock for authoritative decisions: the evaluation tick is part of the input.
struct TimeInterval {
  std::uint64_t start_tick = 0;
  std::uint64_t end_tick = 0;
  bool open_ended = true;

  bool covers(std::uint64_t tick) const noexcept {
    if (tick < start_tick) return false;
    if (open_ended) return true;
    return tick < end_tick;
  }

  friend bool operator==(const TimeInterval&, const TimeInterval&) noexcept = default;
  friend std::strong_ordering operator<=>(const TimeInterval&, const TimeInterval&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

enum class Preemptibility : std::uint8_t {
  not_preemptible = 0,
  preemptible_with_authority = 1,
  preemptible = 2,
};

std::string_view to_string(Preemptibility value) noexcept;
std::optional<Preemptibility> preemptibility_from_string(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Demands
// ---------------------------------------------------------------------------

// A traffic demand is an admitted, authoritative statement of what must be
// carried. Admission itself belongs to Network Admission Fabric; this runtime
// only consumes the admitted result.
struct Demand {
  DemandId id;
  DemandGeneration generation;
  Provenance provenance;

  TenantId tenant;                  // fairness / accountability group
  ServiceClassId service_class;     // opaque service-class binding

  std::int64_t minimum_bandwidth = 0;   // hard floor (hard constraint)
  std::int64_t desired_bandwidth = 0;   // soft target
  std::int64_t maximum_bandwidth = 0;   // hard ceiling
  std::uint8_t priority = 0;            // higher value == more important

  std::optional<std::int64_t> latency_bound_micros;
  std::optional<std::int64_t> path_cost_ceiling;

  CandidateSetRef candidate_set;
  std::vector<PathId> allowed_paths;     // empty == all paths in the candidate set
  std::vector<PathId> forbidden_paths;

  std::vector<DemandId> affinity;
  std::vector<DemandId> anti_affinity;
  std::vector<FailureDomainId> required_failure_domains;
  std::uint32_t min_distinct_failure_domains = 0;

  std::vector<ReservationId> reservation_bindings;

  TimeInterval effective;
  Preemptibility preemptibility = Preemptibility::not_preemptible;

  // Canonical free-form policy metadata (sorted by key at canonicalization).
  std::vector<std::pair<std::string, std::string>> labels;

  friend bool operator==(const Demand&, const Demand&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Paths and resources
// ---------------------------------------------------------------------------

enum class EligibilityScope : std::uint8_t {
  fabric_wide = 0,
  tenant = 1,
  service_class = 2,
};

std::string_view to_string(EligibilityScope value) noexcept;

struct CandidatePath {
  PathId id;
  PathGeneration generation;
  PathAuthorityRef authority;      // exact Path Authority generation
  CandidateSetRef candidate_set;
  Provenance provenance;

  std::vector<ResourceId> resources;          // canonical sorted, unique
  std::vector<FailureDomainId> failure_domains;
  std::vector<std::string> policy_labels;     // canonical sorted, unique

  std::int64_t cost = 0;
  std::optional<std::int64_t> latency_micros;
  EligibilityScope scope = EligibilityScope::fabric_wide;
  TenantId scope_tenant;
  ServiceClassId scope_service_class;

  friend bool operator==(const CandidatePath&, const CandidatePath&) noexcept = default;
};

struct FabricResource {
  ResourceId id;
  ResourceGeneration generation;
  Provenance provenance;

  std::int64_t usable_capacity = 0;   // authoritative usable capacity
  std::int64_t committed_load = 0;    // already-committed load under this snapshot
  std::vector<FailureDomainId> failure_domains;

  friend bool operator==(const FabricResource&, const FabricResource&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

struct CapacitySnapshot {
  CapacitySnapshotId id;
  CapacitySnapshotGeneration generation;
  FabricEpoch fabric_epoch;
  Provenance provenance;
  std::vector<FabricResource> resources;
};

struct Reservation {
  ReservationId id;
  ReservationGeneration generation;
  Provenance provenance;

  std::vector<PathId> paths;          // authoritative path bindings (may be empty)
  std::vector<ResourceId> resources;  // authoritative resource bindings
  std::int64_t bandwidth = 0;

  Preemptibility preemptibility = Preemptibility::not_preemptible;
  std::uint8_t priority = 0;
  DemandId owner;
  TimeInterval effective;

  friend bool operator==(const Reservation&, const Reservation&) noexcept = default;
};

struct ReservationSnapshot {
  ReservationSnapshotId id;
  ReservationSnapshotGeneration generation;
  FabricEpoch fabric_epoch;
  Provenance provenance;
  std::vector<Reservation> reservations;
};

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

// Policy is owned by this runtime: it is durable configuration, not evidence.
struct Policy {
  PolicyId id;
  PolicyGeneration generation;
  Provenance provenance;

  bool allow_preemption = false;
  bool require_minimums = true;              // if false, degraded minimums are permitted
  bool allow_degraded_commit = false;        // may DEGRADED plans be authorized?
  bool require_failure_domain_diversity = false;

  std::int64_t max_utilization_permille = 1000;   // policy cap on resource utilization
  std::int64_t churn_improvement_threshold_permille = 0;
  std::int64_t churn_max_moved_bandwidth_permille = 1000;
  std::int64_t churn_max_operational_risk_permille = 1000;
  std::uint32_t churn_max_affected_demands = 0;   // 0 == unbounded
  std::uint32_t max_paths_per_demand = 0;         // 0 == unbounded

  std::vector<TenantId> allowed_tenants;          // empty == all
  std::vector<ServiceClassId> allowed_service_classes;

  friend bool operator==(const Policy&, const Policy&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Objective profile
// ---------------------------------------------------------------------------

enum class ObjectiveTerm : std::uint16_t {
  satisfy_minimums = 0,
  minimize_max_utilization = 1,
  minimize_congestion_exposure = 2,
  minimize_total_path_cost = 3,
  minimize_churn = 4,
  preserve_reservations = 5,
  preserve_priority = 6,
  maximize_desired_bandwidth = 7,
  fairness_across_groups = 8,
  minimize_failure_domain_concentration = 9,
  minimize_path_count = 10,
};

std::string_view to_string(ObjectiveTerm term) noexcept;
std::optional<ObjectiveTerm> objective_term_from_string(std::string_view text) noexcept;

struct ObjectiveWeight {
  ObjectiveTerm term = ObjectiveTerm::satisfy_minimums;
  std::int64_t weight = 1;

  friend bool operator==(const ObjectiveWeight&, const ObjectiveWeight&) noexcept = default;
  friend std::strong_ordering operator<=>(const ObjectiveWeight& a, const ObjectiveWeight& b) noexcept {
    return static_cast<std::uint16_t>(a.term) <=> static_cast<std::uint16_t>(b.term);
  }
};

// Term order in the vector is lexicographic priority: earlier terms dominate.
struct ObjectiveProfile {
  ObjectiveProfileId id;
  ObjectiveProfileGeneration generation;
  Provenance provenance;
  std::vector<ObjectiveWeight> terms;

  std::int64_t weight_of(ObjectiveTerm term) const noexcept;
  bool has(ObjectiveTerm term) const noexcept { return weight_of(term) != 0; }

  friend bool operator==(const ObjectiveProfile&, const ObjectiveProfile&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Fabric snapshot: the complete authoritative input bundle
// ---------------------------------------------------------------------------

struct FabricSnapshot {
  FabricEpoch fabric_epoch;
  TopologyGeneration topology_generation;
  LinkStateGeneration link_state_generation;
  PathAuthorityGeneration path_authority_generation;
  FailureDomainGeneration failure_domain_generation;

  Provenance provenance;

  CapacitySnapshot capacity;
  ReservationSnapshot reservations;
  Policy policy;
  ObjectiveProfile objective;

  CandidateSetRef candidate_set;
  std::vector<Demand> demands;
  std::vector<CandidatePath> paths;

  // Deterministic evaluation instant for time-dependent inputs.
  std::uint64_t evaluation_tick = 0;
};

// ---------------------------------------------------------------------------
// Canonicalization / structural validation
// ---------------------------------------------------------------------------

// Canonicalization is idempotent: canonicalize(canonicalize(x)) == canonicalize(x).
// It sorts every collection into its canonical order and removes exact
// duplicates. It never reorders semantics-bearing sequences (objective term
// priority and affinity lists are canonicalized by their own key order).
struct CanonicalizeReport {
  std::size_t demands_sorted = 0;
  std::size_t paths_sorted = 0;
  std::size_t resources_sorted = 0;
  std::size_t reservations_sorted = 0;
  std::size_t duplicate_entries_removed = 0;
  bool changed = false;
};

CanonicalizeReport canonicalize(FabricSnapshot& snapshot);

// Structural validation performed before any planning. Returns the first
// structural defect, if any. Semantic feasibility is a solver concern.
Status validate_structure(const FabricSnapshot& snapshot);

// Digest of the canonical demand set (used by the authority vector).
Digest demand_set_digest(const std::vector<Demand>& demands);
Digest candidate_set_digest(const std::vector<CandidatePath>& paths);
Digest resource_catalog_digest(const std::vector<FabricResource>& resources);
Digest reservation_set_digest(const std::vector<Reservation>& reservations);
Digest policy_digest(const Policy& policy);
Digest objective_profile_digest(const ObjectiveProfile& profile);

// Canonical encoding of the whole snapshot (used for wire transport and for
// reproducible fixtures). Decoding re-validates everything.
std::vector<std::byte> encode_snapshot(const FabricSnapshot& snapshot);
Result<FabricSnapshot> decode_snapshot(std::span<const std::byte> payload);

}  // namespace tef
