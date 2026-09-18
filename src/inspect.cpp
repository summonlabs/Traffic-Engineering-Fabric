// Traffic Engineering Fabric - inspection helpers.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "tef/inspect.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tef {

std::string render_authority(const AuthorityVector& authority) {
  std::string out;
  out += "fabric_epoch=" + std::to_string(authority.fabric_epoch.value()) + "\n";
  out += "topology_generation=" + std::to_string(authority.topology_generation.value()) + "\n";
  out += "link_state_generation=" + std::to_string(authority.link_state_generation.value()) + "\n";
  out += "path_authority_generation=" + std::to_string(authority.path_authority_generation.value()) + "\n";
  out += "failure_domain_generation=" + std::to_string(authority.failure_domain_generation.value()) + "\n";
  out += "capacity_snapshot=" + authority.capacity.id.str() + "@" +
         std::to_string(authority.capacity.generation.value()) + "\n";
  out += "reservation_snapshot=" + authority.reservations.id.str() + "@" +
         std::to_string(authority.reservations.generation.value()) + "\n";
  out += "policy=" + authority.policy.id.str() + "@" +
         std::to_string(authority.policy.generation.value()) + "\n";
  out += "objective_profile=" + authority.objective.id.str() + "@" +
         std::to_string(authority.objective.generation.value()) + "\n";
  out += "candidate_set=" + authority.candidate_set.id.str() + "@" +
         std::to_string(authority.candidate_set.generation.value()) + "\n";
  out += "demand_set_digest=" + authority.demand_set_digest.hex() + "\n";
  out += "candidate_set_digest=" + authority.candidate_set_digest.hex() + "\n";
  out += "resource_catalog_digest=" + authority.resource_catalog_digest.hex() + "\n";
  out += "reservation_set_digest=" + authority.reservation_set_digest.hex() + "\n";
  out += "policy_digest=" + authority.policy_digest.hex() + "\n";
  out += "objective_digest=" + authority.objective_digest.hex() + "\n";
  out += "authority_digest=" + authority.digest().hex() + "\n";
  return out;
}

std::string render_snapshot_summary(const FabricSnapshot& snapshot) {
  std::string out;
  out += "demands=" + std::to_string(snapshot.demands.size()) + "\n";
  out += "candidate_paths=" + std::to_string(snapshot.paths.size()) + "\n";
  out += "resources=" + std::to_string(snapshot.capacity.resources.size()) + "\n";
  out += "reservations=" + std::to_string(snapshot.reservations.reservations.size()) + "\n";
  out += "evaluation_tick=" + std::to_string(snapshot.evaluation_tick) + "\n";
  out += "policy_max_utilization_permille=" +
         std::to_string(snapshot.policy.max_utilization_permille) + "\n";
  out += "objective_terms=" + std::to_string(snapshot.objective.terms.size()) + "\n";
  for (const auto& term : snapshot.objective.terms) {
    out += "  term " + std::string(to_string(term.term)) + " weight=" + std::to_string(term.weight) +
           "\n";
  }
  return out;
}

std::string render_feasibility(const FeasibilityResult& feasibility) {
  std::string out;
  out += std::string("status=") + std::string(to_string(feasibility.status)) + "\n";
  if (!feasibility.summary.empty()) out += "summary=" + feasibility.summary + "\n";
  if (feasibility.binding_truncated) out += "binding_truncated=true\n";
  for (const auto& constraint : feasibility.binding) {
    out += "  " + std::string(to_string(constraint.kind)) + " " + constraint.subject +
           " required=" + std::to_string(constraint.required) +
           " available=" + std::to_string(constraint.available) +
           " slack=" + std::to_string(constraint.slack) + "\n";
  }
  return out;
}

std::string render_allocation(const Allocation& allocation) {
  std::string out;
  out += "total_granted=" + std::to_string(allocation.total_granted) + "\n";
  out += "total_reserved=" + std::to_string(allocation.total_reserved) + "\n";
  out += allocation.degraded ? "degraded=true\n" : "degraded=false\n";
  for (const auto& demand : allocation.demands) {
    out += "  " + demand.demand.str() + " min=" + std::to_string(demand.minimum) +
           " desired=" + std::to_string(demand.desired) +
           " granted=" + std::to_string(demand.granted) +
           " reserved=" + std::to_string(demand.reserved) + "\n";
    for (const auto& share : demand.shares) {
      out += "    " + share.path.str() + " = " + std::to_string(share.granted) + "\n";
    }
  }
  for (const auto& resource : allocation.resources) {
    if (!resource.saturated) continue;
    out += "  saturated " + resource.resource.str() + " allocated=" +
           std::to_string(resource.allocated) + " available=" +
           std::to_string(resource.available) + " utilization=" +
           std::to_string(resource.utilization_permille) + " permille\n";
  }
  return out;
}

std::string render_churn(const ChurnReport& churn) {
  std::string out;
  out += std::string("decision=") + std::string(to_string(churn.decision)) + "\n";
  out += "improvement_permille=" + std::to_string(churn.improvement_permille) + "\n";
  out += "required_improvement_permille=" + std::to_string(churn.required_improvement_permille) + "\n";
  out += "moved_bandwidth=" + std::to_string(churn.moved_bandwidth) + "\n";
  out += "moved_permille=" + std::to_string(churn.moved_permille) + "\n";
  out += "demands_changed=" + std::to_string(churn.demands_changed) + "\n";
  out += "paths_added=" + std::to_string(churn.paths_added) + "\n";
  out += "paths_removed=" + std::to_string(churn.paths_removed) + "\n";
  out += "failure_domains_changed=" + std::to_string(churn.failure_domains_changed) + "\n";
  out += "operational_risk_permille=" + std::to_string(churn.operational_risk_permille) + "\n";
  if (!churn.rationale.empty()) out += "rationale=" + churn.rationale + "\n";
  return out;
}

std::string render_plan_summary(const Plan& plan) {
  std::string out;
  out += "plan=" + plan.id.str() + "\n";
  out += "generation=" + std::to_string(plan.generation.value()) + "\n";
  out += std::string("state=") + std::string(to_string(plan.state)) + "\n";
  out += std::string("applicability=") + std::string(to_string(plan.applicability)) + "\n";
  out += "objective_score=" + std::to_string(plan.objective_score) + "\n";
  if (plan.explanation.valid()) out += "explanation=" + plan.explanation.str() + "\n";
  if (!plan.explanation_digest.is_zero()) out += "explanation_digest=" + plan.explanation_digest.hex() + "\n";
  if (plan.commit.valid()) {
    out += "commit=" + plan.commit.str() + "@" + std::to_string(plan.commit_generation.value()) + "\n";
  }
  if (plan.supersedes.id.valid()) out += "supersedes=" + plan.supersedes.id.str() + "\n";
  if (plan.superseded_by.id.valid()) out += "superseded_by=" + plan.superseded_by.id.str() + "\n";
  out += "content_digest=" + plan.content_digest().hex() + "\n";
  out += render_feasibility(plan.feasibility);
  out += render_allocation(plan.allocation);
  out += render_churn(plan.churn);
  return out;
}

}  // namespace tef
