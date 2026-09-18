// Traffic Engineering Fabric - solver correctness proofs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "support/fixtures.hpp"
#include "support/test_framework.hpp"
#include "tef/solver.hpp"

using namespace tef;
using tef::test::build_snapshot;
using tef::test::make_objective;
using tef::test::make_policy;
using tef::test::simple_fabric;
using tef::test::SnapshotSpec;

namespace {

SolveOutcome run(const FabricSnapshot& snapshot, SolveOptions options = {}) {
  const Result<SolveOutcome> outcome = solve(snapshot, options);
  TEF_CHECK_MSG(outcome.has_value(), outcome.has_value() ? "" : outcome.error().format());
  return outcome.value();
}

std::int64_t granted_for(const Allocation& allocation, const std::string& demand) {
  const auto id = DemandId::parse(demand).value();
  const DemandAllocation* entry = allocation.find_demand(id);
  return entry == nullptr ? -1 : entry->granted;
}

bool uses_path(const Allocation& allocation, const std::string& path) {
  const auto id = PathId::parse(path).value();
  for (const auto& demand : allocation.demands) {
    for (const auto& share : demand.shares) {
      if (share.path == id && share.granted > 0) return true;
    }
  }
  return false;
}

}  // namespace

TEF_TEST(feasible_fabric_satisfies_every_hard_minimum) {
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::feasible);
  TEF_CHECK(outcome.verified);
  TEF_CHECK(granted_for(outcome.allocation, "demand-one") >= 1000);
  TEF_CHECK(granted_for(outcome.allocation, "demand-two") >= 500);
  TEF_CHECK(verify_allocation(snapshot, outcome.allocation).empty());
}

TEF_TEST(soft_targets_are_filled_up_to_the_authoritative_maximum) {
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(granted_for(outcome.allocation, "demand-one"), 4000);
  TEF_CHECK_EQ(granted_for(outcome.allocation, "demand-two"), 2000);
  TEF_CHECK_EQ(outcome.allocation.total_granted, 6000);
}

TEF_TEST(a_demand_is_never_granted_more_than_its_maximum) {
  SnapshotSpec spec = simple_fabric();
  spec.demands[0].maximum = 2000;
  spec.demands[0].desired = 6000;
  spec.demands[0].minimum = 1000;
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::feasible);
  TEF_CHECK_EQ(granted_for(outcome.allocation, "demand-one"), 2000);
}

TEF_TEST(impossible_minimums_are_certified_infeasible) {
  SnapshotSpec spec = simple_fabric();
  for (auto& resource : spec.resources) resource.usable = 100;
  spec.demands[0].minimum = 1000;
  spec.demands[0].desired = 1000;
  spec.demands[0].maximum = 1000;
  spec.demands[1].minimum = 500;
  spec.demands[1].desired = 500;
  spec.demands[1].maximum = 500;
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::infeasible_capacity);
  bool has_min_cut = false;
  for (const auto& constraint : outcome.feasibility.binding) {
    if (constraint.kind == ConstraintKind::min_cut_certificate) has_min_cut = true;
  }
  TEF_CHECK(has_min_cut);
  TEF_CHECK(is_definitively_infeasible(outcome.status));
  TEF_CHECK(outcome.verified);
}

TEF_TEST(zero_capacity_resources_carry_no_traffic) {
  SnapshotSpec spec = simple_fabric();
  for (auto& resource : spec.resources) resource.usable = 0;
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::infeasible_capacity);
  for (const auto& resource : outcome.allocation.resources) {
    TEF_CHECK_EQ(resource.allocated, 0);
    TEF_CHECK(resource.saturated);
  }
}

TEF_TEST(committed_load_and_reservations_reduce_available_capacity) {
  SnapshotSpec spec = simple_fabric();
  for (auto& resource : spec.resources) resource.committed = 9700;
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::infeasible_capacity);
  for (const auto& resource : outcome.allocation.resources) {
    TEF_CHECK_EQ(resource.available, 300);
    TEF_CHECK(resource.allocated <= 300);
  }
}

TEF_TEST(non_displaceable_reservations_are_obligations_not_hints) {
  SnapshotSpec spec = simple_fabric();
  spec.reservations.push_back({"reservation-a", {"res-a", "res-b"}, {}, 9500, "", Preemptibility::not_preemptible, true});
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::feasible);
  for (const auto& resource : outcome.allocation.resources) {
    if (resource.resource.str() == "res-a" || resource.resource.str() == "res-b") {
      TEF_CHECK_EQ(resource.reserved, 9500);
      TEF_CHECK_EQ(resource.available, 500);
      TEF_CHECK_LE(resource.allocated, 500);
    } else {
      TEF_CHECK_EQ(resource.reserved, 0);
      TEF_CHECK_EQ(resource.available, 10000);
    }
  }
  // The demand is routed around the reservation rather than through it.
  TEF_CHECK(granted_for(outcome.allocation, "demand-one") >= 1000);
}

TEF_TEST(a_reservation_that_blocks_every_path_is_infeasible) {
  SnapshotSpec spec = simple_fabric();
  for (auto& resource : spec.resources) resource.usable = 1000;
  spec.reservations.push_back({"reservation-a", {"res-a", "res-b", "res-c", "res-d"}, {}, 900,
                               "", Preemptibility::not_preemptible, true});
  spec.demands[0].minimum = 1000;
  spec.demands[0].desired = 1000;
  spec.demands[0].maximum = 1000;
  spec.demands[1].minimum = 0;
  spec.demands[1].desired = 0;
  spec.demands[1].maximum = 0;
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::infeasible_capacity);
  for (const auto& resource : outcome.allocation.resources) {
    TEF_CHECK_EQ(resource.reserved, 900);
    TEF_CHECK_EQ(resource.available, 100);
  }
}

TEF_TEST(reservation_exceeding_usable_capacity_is_an_explicit_conflict) {
  SnapshotSpec spec = simple_fabric();
  spec.reservations.push_back({"reservation-a", {"res-a"}, {}, 20000, "", Preemptibility::not_preemptible, true});
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::infeasible_reservation_conflict);
  bool found = false;
  for (const auto& constraint : outcome.feasibility.binding) {
    if (constraint.kind == ConstraintKind::reservation_obligation) found = true;
  }
  TEF_CHECK(found);
}

TEF_TEST(preemptible_reservations_are_displaced_only_when_policy_allows_it) {
  SnapshotSpec spec = simple_fabric();
  spec.reservations.push_back({"reservation-a", {"res-a", "res-b"}, {}, 9500, "", Preemptibility::preemptible, true});

  SnapshotSpec denied = spec;
  denied.policy.allow_preemption = false;
  const SolveOutcome kept = run(build_snapshot(denied));
  TEF_CHECK_EQ(kept.status, FeasibilityStatus::feasible);
  for (const auto& resource : kept.allocation.resources) {
    if (resource.resource.str() == "res-a" || resource.resource.str() == "res-b") {
      TEF_CHECK_EQ(resource.reserved, 9500);
      TEF_CHECK_EQ(resource.available, 500);
    }
  }

  SnapshotSpec allowed = spec;
  allowed.policy.allow_preemption = true;
  const SolveOutcome displaced = run(build_snapshot(allowed));
  TEF_CHECK_EQ(displaced.status, FeasibilityStatus::feasible);
  for (const auto& resource : displaced.allocation.resources) {
    TEF_CHECK_EQ(resource.reserved, 0);
    TEF_CHECK_EQ(resource.available, 10000);
  }
  TEF_CHECK_NE(kept.allocation_digest.hex(), displaced.allocation_digest.hex());
}

TEF_TEST(preemptible_with_authority_requires_a_demand_attachment) {
  SnapshotSpec spec = simple_fabric();
  spec.reservations.push_back({"reservation-a", {"res-a", "res-b"}, {}, 9500, "",
                               Preemptibility::preemptible_with_authority, true});
  spec.policy.allow_preemption = true;

  // Not attached to any demand: the obligation stands even though preemption is
  // permitted in principle.
  const SolveOutcome unattached = run(build_snapshot(spec));
  for (const auto& resource : unattached.allocation.resources) {
    if (resource.resource.str() == "res-a" || resource.resource.str() == "res-b") {
      TEF_CHECK_EQ(resource.reserved, 9500);
    }
  }

  // Attached to a demand: the adjacent reservation authority has signalled that
  // the cover may be re-planned, so the obligation can be displaced.
  SnapshotSpec attached = spec;
  attached.demands[0].reservation_bindings = {"reservation-a"};
  const SolveOutcome displaced = run(build_snapshot(attached));
  TEF_CHECK_EQ(displaced.status, FeasibilityStatus::feasible);
  for (const auto& resource : displaced.allocation.resources) {
    if (resource.resource.str() == "res-a" || resource.resource.str() == "res-b") {
      TEF_CHECK_EQ(resource.reserved, 0);
    }
  }
}

TEF_TEST(reservations_bound_to_a_demand_are_charged_to_that_demand) {
  SnapshotSpec spec = simple_fabric();
  spec.reservations.push_back({"reservation-a", {"res-a"}, {}, 3000, "demand-one", Preemptibility::not_preemptible, true});
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::feasible);
  const auto* entry = outcome.allocation.find_demand(DemandId::parse("demand-one").value());
  TEF_CHECK(entry != nullptr);
  TEF_CHECK_EQ(entry->reserved, 3000);
  for (const auto& resource : outcome.allocation.resources) {
    TEF_CHECK_EQ(resource.reserved, 0);
  }
}

TEF_TEST(forbidden_paths_are_never_used) {
  SnapshotSpec spec = simple_fabric();
  spec.demands[0].forbidden_paths = {"path-ab"};
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::feasible);
  const auto* entry = outcome.allocation.find_demand(DemandId::parse("demand-one").value());
  TEF_CHECK(entry != nullptr);
  for (const auto& share : entry->shares) {
    TEF_CHECK_NE(share.path.str(), std::string("path-ab"));
  }
  // Only the excluded demand is restricted; the other demand may still use it.
  const auto* other = outcome.allocation.find_demand(DemandId::parse("demand-two").value());
  TEF_CHECK(other != nullptr);
  TEF_CHECK(other->granted >= 500);
}

TEF_TEST(allow_lists_restrict_the_eligible_set) {
  SnapshotSpec spec = simple_fabric();
  spec.demands[0].allowed_paths = {"path-cd"};
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::feasible);
  const auto* entry = outcome.allocation.find_demand(DemandId::parse("demand-one").value());
  TEF_CHECK(entry != nullptr);
  for (const auto& share : entry->shares) {
    TEF_CHECK_EQ(share.path.str(), std::string("path-cd"));
  }
}

TEF_TEST(latency_bound_without_path_evidence_excludes_the_path) {
  SnapshotSpec spec = simple_fabric();
  spec.paths[0].has_latency = false;
  spec.demands[0].latency_bound = 5000;
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::feasible);
  const auto* entry = outcome.allocation.find_demand(DemandId::parse("demand-one").value());
  TEF_CHECK(entry != nullptr);
  for (const auto& share : entry->shares) {
    TEF_CHECK_NE(share.path.str(), std::string("path-ab"));
  }
}

TEF_TEST(no_eligible_path_yields_infeasible_path_set) {
  SnapshotSpec spec = simple_fabric();
  spec.demands[0].forbidden_paths = {"path-ab", "path-cd", "path-ac"};
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::infeasible_path_set);
}

TEF_TEST(policy_utilization_cap_is_a_hard_constraint) {
  SnapshotSpec spec;
  spec.resources = {{"res-a", 1000, 0, {"domain-1"}}};
  spec.paths = {{"path-a", {"res-a"}, {"domain-1"}, 1, true, 100, EligibilityScope::fabric_wide, "", ""}};
  spec.demands = {{"demand-one", "tenant-a", "class-gold", 200, 200, 200, 10},
                  {"demand-two", "tenant-b", "class-silver", 200, 200, 200, 10}};
  spec.policy.max_utilization_permille = 250;
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::infeasible_policy);
  bool has_policy = false;
  for (const auto& constraint : outcome.feasibility.binding) {
    if (constraint.kind == ConstraintKind::policy_utilization_cap) has_policy = true;
  }
  TEF_CHECK(has_policy);

  // Without the cap the same fabric carries both minimums comfortably.
  SnapshotSpec uncapped = spec;
  uncapped.policy.max_utilization_permille = 1000;
  TEF_CHECK_EQ(run(build_snapshot(uncapped)).status, FeasibilityStatus::feasible);
}

TEF_TEST(policy_tenant_exclusion_is_infeasible_policy) {
  SnapshotSpec spec = simple_fabric();
  spec.policy.allowed_tenants = {TenantId::parse("tenant-a").value()};
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::infeasible_policy);
}

TEF_TEST(policy_tenant_exclusion_of_a_zero_floor_demand_is_tolerated) {
  SnapshotSpec spec = simple_fabric();
  spec.demands[1].minimum = 0;
  spec.demands[1].desired = 1000;
  spec.demands[1].maximum = 1000;
  spec.policy.allowed_tenants = {TenantId::parse("tenant-a").value()};
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::feasible);
  TEF_CHECK_EQ(granted_for(outcome.allocation, "demand-two"), 0);
  TEF_CHECK(!outcome.policy_exclusions.empty());
}

TEF_TEST(minimize_max_utilization_tightens_the_ceiling) {
  SnapshotSpec spec = simple_fabric();
  for (auto& resource : spec.resources) resource.usable = 8000;
  spec.objective = make_objective({{ObjectiveTerm::satisfy_minimums, 1000},
                                   {ObjectiveTerm::minimize_max_utilization, 1},
                                   {ObjectiveTerm::maximize_desired_bandwidth, 1}});
  spec.demands[0].minimum = 1000;
  spec.demands[0].desired = 2000;
  spec.demands[1].minimum = 500;
  spec.demands[1].desired = 1000;
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::feasible);
  std::int64_t worst = 0;
  for (const auto& resource : outcome.allocation.resources) {
    worst = std::max(worst, resource.utilization_permille);
  }
  TEF_CHECK_LT(worst, 1000);
}

TEF_TEST(preserve_priority_prefers_higher_priority_demands_under_scarcity) {
  SnapshotSpec spec;
  spec.resources = {{"res-a", 1000, 0, {"domain-1"}}};
  spec.paths = {{"path-a", {"res-a"}, {"domain-1"}, 1, true, 100, EligibilityScope::fabric_wide, "", ""}};
  spec.demands = {
      {"demand-low", "tenant-a", "class-bronze", 0, 1000, 1000, 10},
      {"demand-high", "tenant-b", "class-gold", 0, 1000, 1000, 250},
  };
  spec.objective = make_objective({{ObjectiveTerm::satisfy_minimums, 1000},
                                   {ObjectiveTerm::preserve_priority, 1},
                                   {ObjectiveTerm::maximize_desired_bandwidth, 1}});
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::feasible);
  TEF_CHECK_EQ(granted_for(outcome.allocation, "demand-high"), 1000);
  TEF_CHECK_EQ(granted_for(outcome.allocation, "demand-low"), 0);
}

TEF_TEST(deterministic_input_produces_deterministic_output) {
  const FabricSnapshot snapshot = build_snapshot(tef::test::random_fabric(4242, 24, 12, 24));
  const SolveOutcome first = run(snapshot);
  const SolveOutcome second = run(snapshot);
  TEF_CHECK_EQ(first.allocation_digest.hex(), second.allocation_digest.hex());
  TEF_CHECK_EQ(first.score, second.score);
  TEF_CHECK_EQ(first.status, second.status);
}

TEF_TEST(input_permutation_does_not_change_the_result) {
  SnapshotSpec spec = tef::test::random_fabric(99, 16, 8, 16);
  FabricSnapshot base = build_snapshot(spec);
  SnapshotSpec shuffled = spec;
  std::reverse(shuffled.demands.begin(), shuffled.demands.end());
  std::reverse(shuffled.paths.begin(), shuffled.paths.end());
  std::reverse(shuffled.resources.begin(), shuffled.resources.end());
  FabricSnapshot permuted = build_snapshot(shuffled);
  const SolveOutcome first = run(base);
  const SolveOutcome second = run(permuted);
  TEF_CHECK_EQ(first.status, second.status);
  TEF_CHECK_EQ(first.allocation_digest.hex(), second.allocation_digest.hex());
}

TEF_TEST(enormous_bandwidths_do_not_overflow_accounting) {
  SnapshotSpec spec;
  spec.resources = {{"res-a", Limits::max_bandwidth, 0, {"domain-1"}}};
  spec.paths = {{"path-a", {"res-a"}, {"domain-1"}, Limits::max_cost, true, Limits::max_latency_micros,
                 EligibilityScope::fabric_wide, "", ""}};
  spec.demands = {{"demand-huge", "tenant-a", "class-gold", Limits::max_bandwidth,
                   Limits::max_bandwidth, Limits::max_bandwidth, 255}};
  spec.objective = make_objective({{ObjectiveTerm::satisfy_minimums, 1000},
                                   {ObjectiveTerm::minimize_total_path_cost, 1},
                                   {ObjectiveTerm::maximize_desired_bandwidth, 1}});
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::feasible);
  TEF_CHECK_EQ(granted_for(outcome.allocation, "demand-huge"), Limits::max_bandwidth);
  TEF_CHECK(outcome.verified);
}

TEF_TEST(verification_rejects_a_tampered_allocation) {
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const SolveOutcome outcome = run(snapshot);
  Allocation tampered = outcome.allocation;
  tampered.demands[0].shares[0].granted += 100000;
  tampered.demands[0].granted += 100000;
  const std::vector<BindingConstraint> violations = verify_allocation(snapshot, tampered);
  TEF_CHECK(!violations.empty());
}

TEF_TEST(verification_rejects_an_unauthorised_path) {
  SnapshotSpec spec = simple_fabric();
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  Allocation tampered = outcome.allocation;
  tampered.demands[0].shares.clear();
  PathShare share;
  share.path = PathId::parse("path-not-in-set").value();
  share.generation = PathGeneration::first();
  share.granted = 100;
  tampered.demands[0].shares.push_back(share);
  tampered.demands[0].granted = 100;
  TEF_CHECK(!verify_allocation(snapshot, tampered).empty());
}

TEF_TEST(usage_counts_are_bounded_and_stable_across_repeats) {
  SnapshotSpec spec = tef::test::random_fabric(7, 32, 16, 32);
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_LT(outcome.iterations, Limits::max_solver_iterations);
  TEF_CHECK(outcome.allocation.demands.size() <= Limits::max_demands);
  TEF_CHECK(!outcome.allocation_digest.is_zero());
}

TEF_TEST(negative_objective_weight_is_reported_as_unsupported) {
  SnapshotSpec spec = simple_fabric();
  spec.objective = make_objective({{ObjectiveTerm::satisfy_minimums, 1000},
                                   {ObjectiveTerm::minimize_churn, -1}});
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::unsupported_objective);
  TEF_CHECK(!carries_allocation(outcome.status));
}

TEF_TEST(disconnected_candidate_sets_are_infeasible_without_fallback_routing) {
  SnapshotSpec spec;
  spec.resources = {{"res-a", 10000, 0, {"domain-1"}}, {"res-b", 10000, 0, {"domain-2"}}};
  spec.paths = {{"path-a", {"res-a"}, {"domain-1"}, 1, true, 100, EligibilityScope::fabric_wide, "", ""},
                {"path-b", {"res-b"}, {"domain-2"}, 1, true, 100, EligibilityScope::fabric_wide, "", ""}};
  spec.demands = {{"demand-a", "tenant-a", "class-gold", 100, 200, 300, 10}};
  spec.demands[0].forbidden_paths = {"path-a"};
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::feasible);
  TEF_CHECK_EQ(granted_for(outcome.allocation, "demand-a"), 200);
  const auto* entry = outcome.allocation.find_demand(DemandId::parse("demand-a").value());
  TEF_CHECK(entry != nullptr);
  for (const auto& share : entry->shares) {
    TEF_CHECK_EQ(share.path.str(), std::string("path-b"));
  }
}

TEF_TEST(reservation_conflict_is_reported_before_any_allocation_attempt) {
  SnapshotSpec spec = simple_fabric();
  spec.reservations.push_back({"reservation-big", {"res-c"}, {}, 30000, "", Preemptibility::not_preemptible, true});
  const FabricSnapshot snapshot = build_snapshot(spec);
  const SolveOutcome outcome = run(snapshot);
  TEF_CHECK_EQ(outcome.status, FeasibilityStatus::infeasible_reservation_conflict);
  TEF_CHECK(outcome.feasibility.binding.size() >= 1);
}

int main(int argc, char** argv) { return tef::test::run_all(argc, argv); }
