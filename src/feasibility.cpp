// Traffic Engineering Fabric - feasibility outcome helpers.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "tef/feasibility.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace tef {

std::string_view to_string(FeasibilityStatus status) noexcept {
  switch (status) {
    case FeasibilityStatus::feasible: return "FEASIBLE";
    case FeasibilityStatus::feasible_degraded: return "FEASIBLE_DEGRADED";
    case FeasibilityStatus::infeasible_capacity: return "INFEASIBLE_CAPACITY";
    case FeasibilityStatus::infeasible_policy: return "INFEASIBLE_POLICY";
    case FeasibilityStatus::infeasible_path_set: return "INFEASIBLE_PATH_SET";
    case FeasibilityStatus::infeasible_reservation_conflict: return "INFEASIBLE_RESERVATION_CONFLICT";
    case FeasibilityStatus::stale_input: return "STALE_INPUT";
    case FeasibilityStatus::conflicting_input: return "CONFLICTING_INPUT";
    case FeasibilityStatus::unsupported_objective: return "UNSUPPORTED_OBJECTIVE";
    case FeasibilityStatus::solver_limit_reached: return "SOLVER_LIMIT_REACHED";
  }
  return "UNKNOWN";
}

bool carries_allocation(FeasibilityStatus status) noexcept {
  return status == FeasibilityStatus::feasible || status == FeasibilityStatus::feasible_degraded ||
         status == FeasibilityStatus::infeasible_capacity ||
         status == FeasibilityStatus::infeasible_reservation_conflict;
}

bool is_definitively_infeasible(FeasibilityStatus status) noexcept {
  switch (status) {
    case FeasibilityStatus::infeasible_capacity:
    case FeasibilityStatus::infeasible_policy:
    case FeasibilityStatus::infeasible_path_set:
    case FeasibilityStatus::infeasible_reservation_conflict:
    case FeasibilityStatus::stale_input:
    case FeasibilityStatus::conflicting_input:
    case FeasibilityStatus::unsupported_objective:
      return true;
    case FeasibilityStatus::feasible:
    case FeasibilityStatus::feasible_degraded:
    case FeasibilityStatus::solver_limit_reached:
      return false;
  }
  return false;
}

std::string_view to_string(ConstraintKind kind) noexcept {
  switch (kind) {
    case ConstraintKind::resource_capacity: return "resource_capacity";
    case ConstraintKind::policy_utilization_cap: return "policy_utilization_cap";
    case ConstraintKind::demand_minimum: return "demand_minimum";
    case ConstraintKind::demand_maximum: return "demand_maximum";
    case ConstraintKind::path_eligibility: return "path_eligibility";
    case ConstraintKind::forbidden_path: return "forbidden_path";
    case ConstraintKind::path_authority_generation: return "path_authority_generation";
    case ConstraintKind::reservation_obligation: return "reservation_obligation";
    case ConstraintKind::reservation_preemption_denied: return "reservation_preemption_denied";
    case ConstraintKind::failure_domain_diversity: return "failure_domain_diversity";
    case ConstraintKind::affinity: return "affinity";
    case ConstraintKind::anti_affinity: return "anti_affinity";
    case ConstraintKind::tenant_policy: return "tenant_policy";
    case ConstraintKind::service_class_policy: return "service_class_policy";
    case ConstraintKind::latency_bound: return "latency_bound";
    case ConstraintKind::path_cost_ceiling: return "path_cost_ceiling";
    case ConstraintKind::path_count_limit: return "path_count_limit";
    case ConstraintKind::stale_generation: return "stale_generation";
    case ConstraintKind::unknown_demand_reference: return "unknown_demand_reference";
    case ConstraintKind::duplicate_identity: return "duplicate_identity";
    case ConstraintKind::missing_provenance: return "missing_provenance";
    case ConstraintKind::structural: return "structural";
    case ConstraintKind::min_cut_certificate: return "min_cut_certificate";
  }
  return "unknown";
}

void FeasibilityResult::add(BindingConstraint constraint) {
  binding.push_back(std::move(constraint));
}

void canonicalize_bindings(std::vector<BindingConstraint>& bindings, bool& truncated) {
  std::sort(bindings.begin(), bindings.end());
  bindings.erase(std::unique(bindings.begin(), bindings.end()), bindings.end());
  if (bindings.size() > Limits::max_explanation_entries) {
    bindings.resize(Limits::max_explanation_entries);
    truncated = true;
  }
}

}  // namespace tef
