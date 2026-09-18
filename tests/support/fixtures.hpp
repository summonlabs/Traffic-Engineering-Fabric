// Traffic Engineering Fabric - deterministic test fixtures.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every fixture is a pure function of its parameters, so a failing case can be
// reproduced exactly from the numbers printed in the test output.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tef/model.hpp"

namespace tef::test {

inline SourceSystemId source(std::string_view name) {
  return SourceSystemId::parse(name).value();
}

inline Provenance provenance(std::string_view system, std::uint64_t generation) {
  Provenance value;
  value.source = source(system);
  value.source_generation = generation;
  value.observed_tick = generation;
  value.evidence_digest = Sha256::hash(std::string(system) + "#" + std::to_string(generation));
  value.detail = std::string(system) + " evidence";
  return value;
}

inline Policy make_policy(std::string_view id = "policy-a", std::uint64_t generation = 1) {
  Policy policy;
  policy.id = PolicyId::parse(id).value();
  policy.generation = PolicyGeneration::parse(generation).value();
  policy.provenance = provenance("tef-config", generation);
  policy.allow_preemption = false;
  policy.require_minimums = true;
  policy.allow_degraded_commit = false;
  policy.require_failure_domain_diversity = false;
  policy.max_utilization_permille = 1000;
  policy.churn_improvement_threshold_permille = 0;
  policy.churn_max_moved_bandwidth_permille = 1000;
  policy.churn_max_operational_risk_permille = 1000;
  policy.churn_max_affected_demands = 0;
  policy.max_paths_per_demand = 0;
  return policy;
}

inline ObjectiveProfile make_objective(
    std::initializer_list<ObjectiveWeight> terms = {{ObjectiveTerm::satisfy_minimums, 1000},
                                                    {ObjectiveTerm::maximize_desired_bandwidth, 1}},
    std::string_view id = "objective-default", std::uint64_t generation = 1) {
  ObjectiveProfile profile;
  profile.id = ObjectiveProfileId::parse(id).value();
  profile.generation = ObjectiveProfileGeneration::parse(generation).value();
  profile.provenance = provenance("tef-config", generation);
  for (const auto& term : terms) profile.terms.push_back(term);
  return profile;
}

struct ResourceSpec {
  std::string id;
  std::int64_t usable = 0;
  std::int64_t committed = 0;
  std::vector<std::string> domains;
};

struct PathSpec {
  std::string id;
  std::vector<std::string> resources;
  std::vector<std::string> domains;
  std::int64_t cost = 1;
  bool has_latency = true;
  std::int64_t latency_micros = 1000;
  EligibilityScope scope = EligibilityScope::fabric_wide;
  std::string scope_tenant;
  std::string scope_service_class;
};

struct DemandSpec {
  std::string id;
  std::string tenant = "tenant-a";
  std::string service_class = "class-gold";
  std::int64_t minimum = 0;
  std::int64_t desired = 0;
  std::int64_t maximum = 0;
  std::uint8_t priority = 128;
  std::vector<std::string> allowed_paths;
  std::vector<std::string> forbidden_paths;
  std::optional<std::int64_t> latency_bound;
  std::optional<std::int64_t> cost_ceiling;
  std::vector<std::string> required_domains;
  std::vector<std::string> reservation_bindings;
  Preemptibility preemptibility = Preemptibility::not_preemptible;
};

struct ReservationSpec {
  std::string id;
  std::vector<std::string> resources;
  std::vector<std::string> paths;
  std::int64_t bandwidth = 0;
  std::string owner;
  Preemptibility preemptibility = Preemptibility::not_preemptible;
  bool active = true;
};

struct SnapshotSpec {
  std::string candidate_set = "candidates-a";
  std::uint64_t candidate_set_generation = 1;
  std::uint64_t fabric_epoch = 1;
  std::uint64_t topology_generation = 1;
  std::uint64_t link_state_generation = 1;
  std::uint64_t path_authority_generation = 1;
  std::uint64_t failure_domain_generation = 1;
  std::uint64_t evaluation_tick = 0;
  std::vector<ResourceSpec> resources;
  std::vector<PathSpec> paths;
  std::vector<DemandSpec> demands;
  std::vector<ReservationSpec> reservations;
  Policy policy = make_policy();
  ObjectiveProfile objective = make_objective();
  std::uint64_t demand_generation = 1;
  std::uint64_t path_generation = 1;
  std::uint64_t resource_generation = 1;
  std::uint64_t reservation_generation = 1;
};

// Builds a canonical snapshot from the specification. The result always passes
// validate_structure unless the specification itself is contradictory, which is
// exactly what the adversarial tests exercise.
FabricSnapshot build_snapshot(const SnapshotSpec& spec);

// A small, fully connected three-path, four-resource fabric used by most tests.
SnapshotSpec simple_fabric();

// A randomized population with a deterministic seed. Same seed, same fabric.
SnapshotSpec random_fabric(std::uint64_t seed, std::size_t demand_count, std::size_t path_count,
                           std::size_t resource_count);

std::string temporary_directory(std::string_view tag);

}  // namespace tef::test
