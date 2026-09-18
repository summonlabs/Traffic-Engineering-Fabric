// Traffic Engineering Fabric - typed feasibility outcomes.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tef/limits.hpp"
#include "tef/model.hpp"

namespace tef {

// Hard constraints and soft objectives are represented by different types so
// that an infeasible state can never be reported as merely suboptimal.
enum class FeasibilityStatus : std::uint16_t {
  feasible = 0,
  feasible_degraded = 1,
  infeasible_capacity = 2,
  infeasible_policy = 3,
  infeasible_path_set = 4,
  infeasible_reservation_conflict = 5,
  stale_input = 6,
  conflicting_input = 7,
  unsupported_objective = 8,
  // The deterministic solver exhausted its bounded search without constructing
  // a feasible allocation and without producing a sound infeasibility
  // certificate. This is an explicit "not proven either way" outcome; it is
  // never reported as success and never silently downgraded to a default path.
  solver_limit_reached = 9,
};

std::string_view to_string(FeasibilityStatus status) noexcept;

// True when the status carries a constructed, verifiable allocation.
bool carries_allocation(FeasibilityStatus status) noexcept;
// True when the status proves the request cannot be satisfied.
bool is_definitively_infeasible(FeasibilityStatus status) noexcept;

enum class ConstraintKind : std::uint16_t {
  resource_capacity = 0,
  policy_utilization_cap = 1,
  demand_minimum = 2,
  demand_maximum = 3,
  path_eligibility = 4,
  forbidden_path = 5,
  path_authority_generation = 6,
  reservation_obligation = 7,
  reservation_preemption_denied = 8,
  failure_domain_diversity = 9,
  affinity = 10,
  anti_affinity = 11,
  tenant_policy = 12,
  service_class_policy = 13,
  latency_bound = 14,
  path_cost_ceiling = 15,
  path_count_limit = 16,
  stale_generation = 17,
  unknown_demand_reference = 18,
  duplicate_identity = 19,
  missing_provenance = 20,
  structural = 21,
  min_cut_certificate = 22,
};

std::string_view to_string(ConstraintKind kind) noexcept;

// A single binding reason. required vs available are exact integer quantities in
// fabric bandwidth units so the explanation is checkable by a third party.
struct BindingConstraint {
  ConstraintKind kind = ConstraintKind::structural;
  std::string subject;          // canonical textual subject
  DemandId demand;              // set when the constraint is demand-scoped
  ResourceId resource;          // set when resource-scoped
  PathId path;                  // set when path-scoped
  std::int64_t required = 0;
  std::int64_t available = 0;
  std::int64_t slack = 0;       // available - required (may be negative)
  std::string detail;

  friend bool operator==(const BindingConstraint&, const BindingConstraint&) noexcept = default;
  friend std::strong_ordering operator<=>(const BindingConstraint& a, const BindingConstraint& b) noexcept {
    if (auto c = a.kind <=> b.kind; c != 0) return c;
    if (auto c = a.subject <=> b.subject; c != 0) return c;
    if (auto c = a.demand.str() <=> b.demand.str(); c != 0) return c;
    if (auto c = a.resource.str() <=> b.resource.str(); c != 0) return c;
    if (auto c = a.path.str() <=> b.path.str(); c != 0) return c;
    if (auto c = a.required <=> b.required; c != 0) return c;
    return a.available <=> b.available;
  }
};

// Deterministically minimized explanation of infeasibility: the smallest set of
// constraints the solver could reduce the failure to, in canonical order.
struct FeasibilityResult {
  FeasibilityStatus status = FeasibilityStatus::feasible;
  std::string summary;
  std::vector<BindingConstraint> binding;
  bool binding_truncated = false;

  bool ok() const noexcept {
    return status == FeasibilityStatus::feasible || status == FeasibilityStatus::feasible_degraded;
  }

  void add(BindingConstraint constraint);
};

// Canonical ordering + duplicate removal + size bounding of a binding set.
void canonicalize_bindings(std::vector<BindingConstraint>& bindings, bool& truncated);

}  // namespace tef
