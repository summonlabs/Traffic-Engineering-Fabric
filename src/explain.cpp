// Traffic Engineering Fabric - deterministic bounded explanations.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "tef/explain.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "tef/binary.hpp"
#include "tef/derived.hpp"
#include "tef/numeric.hpp"
#include "tef/solver.hpp"

namespace tef {
namespace {

void append_escaped(std::string& out, std::string_view text) {
  out.push_back('"');
  for (char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    switch (u) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (u < 0x20) {
          static const char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[(u >> 4) & 0x0F]);
          out.push_back(kHex[u & 0x0F]);
        } else {
          out.push_back(static_cast<char>(u));
        }
    }
  }
  out.push_back('"');
}

void append_key(std::string& out, std::string_view key, bool& first) {
  if (!first) out.push_back(',');
  first = false;
  append_escaped(out, key);
  out.push_back(':');
}

}  // namespace

Digest Explanation::digest() const {
  Writer w;
  w.str(id.str());
  w.str(plan.id.str());
  w.u64(plan.generation.value());
  w.u16(static_cast<std::uint16_t>(state));
  w.u16(static_cast<std::uint16_t>(applicability));
  w.digest(authority.digest());
  w.str(objective.id.str());
  w.u64(objective.generation.value());
  w.u16(static_cast<std::uint16_t>(feasibility.status));
  w.str(feasibility.summary);
  w.i64(total_score);
  w.u32(static_cast<std::uint32_t>(components.size()));
  for (const auto& component : components) {
    w.u16(static_cast<std::uint16_t>(component.term));
    w.i64(component.weight);
    w.i64(component.raw);
    w.i64(component.weighted);
  }
  w.u32(static_cast<std::uint32_t>(binding_constraints.size()));
  for (const auto& constraint : binding_constraints) {
    w.u16(static_cast<std::uint16_t>(constraint.kind));
    w.str(constraint.subject);
    w.i64(constraint.required);
    w.i64(constraint.available);
  }
  w.u32(static_cast<std::uint32_t>(saturated_resources.size()));
  for (const auto& resource : saturated_resources) {
    w.str(resource.resource.str());
    w.i64(resource.utilization_permille);
    w.i64(resource.allocated);
  }
  w.u32(static_cast<std::uint32_t>(allocations.size()));
  for (const auto& allocation : allocations) {
    w.str(allocation.demand.str());
    w.i64(allocation.minimum);
    w.i64(allocation.desired);
    w.i64(allocation.granted);
    w.i64(allocation.reserved);
    w.u32(static_cast<std::uint32_t>(allocation.shares.size()));
    for (const auto& share : allocation.shares) {
      w.str(share.path.str());
      w.i64(share.granted);
    }
  }
  w.u32(static_cast<std::uint32_t>(policy_exclusions.size()));
  for (const auto& exclusion : policy_exclusions) w.str(exclusion);
  w.u32(static_cast<std::uint32_t>(alternatives.size()));
  for (const auto& alternative : alternatives) {
    w.str(alternative.subject);
    w.str(alternative.reason);
    w.i64(alternative.delta);
  }
  w.u8(static_cast<std::uint8_t>(churn.decision));
  w.str(churn.rationale);
  w.u32(static_cast<std::uint32_t>(notes.size()));
  for (const auto& note : notes) w.str(note);
  w.boolean(truncated);
  return w.finish_digest("tef.explanation.v1");
}

Explanation build_explanation(const Plan& plan, const FabricSnapshot& snapshot) {
  Explanation explanation;
  explanation.plan = plan.ref();
  explanation.state = plan.state;
  explanation.applicability = plan.applicability;
  explanation.authority = plan.authority;
  explanation.objective = plan.authority.objective;
  explanation.feasibility = plan.feasibility;
  explanation.churn = plan.churn;
  explanation.total_score = plan.objective_score;

  std::vector<ObjectiveComponent> components;
  (void)score_allocation(snapshot, plan.allocation, &components);
  explanation.components = std::move(components);

  const std::size_t entry_budget = Limits::max_explanation_entries;
  const std::size_t alternative_budget = Limits::max_alternatives;

  std::size_t used = 0;
  for (const auto& constraint : plan.feasibility.binding) {
    if (used >= entry_budget) {
      explanation.truncated = true;
      ++explanation.omitted_binding_constraints;
      continue;
    }
    explanation.binding_constraints.push_back(constraint);
    ++used;
  }

  for (const auto& resource : plan.allocation.resources) {
    if (!resource.saturated) continue;
    explanation.saturated_resources.push_back(resource);
  }

  for (const auto& allocation : plan.allocation.demands) {
    if (explanation.allocations.size() >= entry_budget) {
      explanation.truncated = true;
      ++explanation.omitted_allocations;
      continue;
    }
    explanation.allocations.push_back(allocation);
  }

  const Result<Derivation> derivation = derive(snapshot);
  if (derivation.has_value()) {
    explanation.policy_exclusions = derivation.value().policy_exclusions;
    for (const auto& demand : derivation.value().demands) {
      for (const auto& note : demand.notes) {
        if (explanation.binding_constraints.size() >= entry_budget) {
          explanation.truncated = true;
          ++explanation.omitted_binding_constraints;
          break;
        }
        explanation.binding_constraints.push_back(note);
      }
    }
    std::sort(explanation.binding_constraints.begin(), explanation.binding_constraints.end());
    explanation.binding_constraints.erase(
        std::unique(explanation.binding_constraints.begin(), explanation.binding_constraints.end()),
        explanation.binding_constraints.end());
    if (explanation.binding_constraints.size() > entry_budget) {
      explanation.binding_constraints.resize(entry_budget);
      explanation.truncated = true;
    }
  }

  for (const auto& alternative : plan.alternatives) {
    if (explanation.alternatives.size() >= alternative_budget) {
      explanation.truncated = true;
      ++explanation.omitted_alternatives;
      continue;
    }
    explanation.alternatives.push_back(alternative);
  }

  if (plan.churn.decision != ChurnDecision::no_incumbent) {
    explanation.notes.push_back(std::string("churn decision: ") +
                                std::string(to_string(plan.churn.decision)));
    if (!plan.churn.rationale.empty()) explanation.notes.push_back(plan.churn.rationale);
  }
  if (plan.supersedes.id.valid()) {
    explanation.notes.push_back("supersedes plan " + plan.supersedes.id.str() + " generation " +
                                std::to_string(plan.supersedes.generation.value()));
  }
  if (plan.superseded_by.id.valid()) {
    explanation.notes.push_back("superseded by plan " + plan.superseded_by.id.str() + " generation " +
                                std::to_string(plan.superseded_by.generation.value()));
  }
  if (plan.state == PlanState::stale) {
    explanation.notes.push_back("the plan is stale: a material bound input advanced after commit");
  }
  if (plan.applicability == PlanApplicability::revalidation_required) {
    explanation.notes.push_back(
        "the plan was restored from durable storage and must be revalidated against the live "
        "authority before it is treated as current");
  }
  for (const auto& note : explanation.policy_exclusions) explanation.notes.push_back(note);

  if (explanation.notes.size() > entry_budget) {
    explanation.notes.resize(entry_budget);
    explanation.truncated = true;
  }
  explanation.id = ExplanationId::parse("explain-" + plan.id.str() + "-g" +
                                        std::to_string(plan.generation.value()))
                       .value_or(ExplanationId{});
  return explanation;
}

std::string Explanation::to_json() const {
  std::string out = "{";
  bool first = true;
  append_key(out, "explanation_id", first);
  append_escaped(out, id.str());
  append_key(out, "plan_id", first);
  append_escaped(out, plan.id.str());
  append_key(out, "plan_generation", first);
  out += std::to_string(plan.generation.value());
  append_key(out, "state", first);
  append_escaped(out, to_string(state));
  append_key(out, "applicability", first);
  append_escaped(out, to_string(applicability));
  append_key(out, "authority_digest", first);
  append_escaped(out, authority.digest().hex());
  append_key(out, "authority", first);
  out += "{";
  bool authority_first = true;
  append_key(out, "fabric_epoch", authority_first);
  out += std::to_string(authority.fabric_epoch.value());
  append_key(out, "topology_generation", authority_first);
  out += std::to_string(authority.topology_generation.value());
  append_key(out, "link_state_generation", authority_first);
  out += std::to_string(authority.link_state_generation.value());
  append_key(out, "path_authority_generation", authority_first);
  out += std::to_string(authority.path_authority_generation.value());
  append_key(out, "failure_domain_generation", authority_first);
  out += std::to_string(authority.failure_domain_generation.value());
  append_key(out, "capacity_snapshot", authority_first);
  out += "{";
  bool ref_first = true;
  append_key(out, "id", ref_first);
  append_escaped(out, authority.capacity.id.str());
  append_key(out, "generation", ref_first);
  out += std::to_string(authority.capacity.generation.value());
  out += "}";
  append_key(out, "reservation_snapshot", authority_first);
  out += "{";
  ref_first = true;
  append_key(out, "id", ref_first);
  append_escaped(out, authority.reservations.id.str());
  append_key(out, "generation", ref_first);
  out += std::to_string(authority.reservations.generation.value());
  out += "}";
  append_key(out, "policy", authority_first);
  out += "{";
  ref_first = true;
  append_key(out, "id", ref_first);
  append_escaped(out, authority.policy.id.str());
  append_key(out, "generation", ref_first);
  out += std::to_string(authority.policy.generation.value());
  out += "}";
  append_key(out, "objective_profile", authority_first);
  out += "{";
  ref_first = true;
  append_key(out, "id", ref_first);
  append_escaped(out, authority.objective.id.str());
  append_key(out, "generation", ref_first);
  out += std::to_string(authority.objective.generation.value());
  out += "}";
  append_key(out, "candidate_set", authority_first);
  out += "{";
  ref_first = true;
  append_key(out, "id", ref_first);
  append_escaped(out, authority.candidate_set.id.str());
  append_key(out, "generation", ref_first);
  out += std::to_string(authority.candidate_set.generation.value());
  out += "}";
  append_key(out, "digests", authority_first);
  out += "{";
  ref_first = true;
  append_key(out, "demand_set", ref_first);
  append_escaped(out, authority.demand_set_digest.hex());
  append_key(out, "candidate_set", ref_first);
  append_escaped(out, authority.candidate_set_digest.hex());
  append_key(out, "resource_catalog", ref_first);
  append_escaped(out, authority.resource_catalog_digest.hex());
  append_key(out, "reservation_set", ref_first);
  append_escaped(out, authority.reservation_set_digest.hex());
  append_key(out, "policy", ref_first);
  append_escaped(out, authority.policy_digest.hex());
  append_key(out, "objective", ref_first);
  append_escaped(out, authority.objective_digest.hex());
  out += "}";
  out += "}";

  append_key(out, "feasibility", first);
  out += "{";
  bool feasibility_first = true;
  append_key(out, "status", feasibility_first);
  append_escaped(out, to_string(feasibility.status));
  append_key(out, "summary", feasibility_first);
  append_escaped(out, feasibility.summary);
  append_key(out, "verified", feasibility_first);
  out += "false";
  append_key(out, "binding", feasibility_first);
  out += "[";
  for (std::size_t i = 0; i < binding_constraints.size(); ++i) {
    if (i != 0) out.push_back(',');
    const auto& constraint = binding_constraints[i];
    out += "{";
    bool constraint_first = true;
    append_key(out, "kind", constraint_first);
    append_escaped(out, to_string(constraint.kind));
    append_key(out, "subject", constraint_first);
    append_escaped(out, constraint.subject);
    append_key(out, "required", constraint_first);
    out += std::to_string(constraint.required);
    append_key(out, "available", constraint_first);
    out += std::to_string(constraint.available);
    append_key(out, "slack", constraint_first);
    out += std::to_string(constraint.slack);
    append_key(out, "detail", constraint_first);
    append_escaped(out, constraint.detail);
    out += "}";
  }
  out += "]}";

  append_key(out, "objective", first);
  out += "{";
  bool objective_first = true;
  append_key(out, "profile_id", objective_first);
  append_escaped(out, objective.id.str());
  append_key(out, "profile_generation", objective_first);
  out += std::to_string(objective.generation.value());
  append_key(out, "total_score", objective_first);
  out += std::to_string(total_score);
  append_key(out, "components", objective_first);
  out += "[";
  for (std::size_t i = 0; i < components.size(); ++i) {
    if (i != 0) out.push_back(',');
    const auto& component = components[i];
    out += "{";
    bool component_first = true;
    append_key(out, "term", component_first);
    append_escaped(out, to_string(component.term));
    append_key(out, "weight", component_first);
    out += std::to_string(component.weight);
    append_key(out, "raw", component_first);
    out += std::to_string(component.raw);
    append_key(out, "weighted", component_first);
    out += std::to_string(component.weighted);
    append_key(out, "unit", component_first);
    append_escaped(out, component.unit);
    out += "}";
  }
  out += "]}";

  append_key(out, "allocations", first);
  out += "[";
  for (std::size_t i = 0; i < allocations.size(); ++i) {
    if (i != 0) out.push_back(',');
    const auto& allocation = allocations[i];
    out += "{";
    bool allocation_first = true;
    append_key(out, "demand", allocation_first);
    append_escaped(out, allocation.demand.str());
    append_key(out, "minimum", allocation_first);
    out += std::to_string(allocation.minimum);
    append_key(out, "desired", allocation_first);
    out += std::to_string(allocation.desired);
    append_key(out, "maximum", allocation_first);
    out += std::to_string(allocation.maximum);
    append_key(out, "granted", allocation_first);
    out += std::to_string(allocation.granted);
    append_key(out, "reserved", allocation_first);
    out += std::to_string(allocation.reserved);
    append_key(out, "effective", allocation_first);
    out += std::to_string(allocation.effective);
    append_key(out, "shortfall_minimum", allocation_first);
    out += std::to_string(allocation.shortfall_against_minimum);
    append_key(out, "shortfall_desired", allocation_first);
    out += std::to_string(allocation.shortfall_against_desired);
    append_key(out, "shares", allocation_first);
    out += "[";
    for (std::size_t j = 0; j < allocation.shares.size(); ++j) {
      if (j != 0) out.push_back(',');
      const auto& share = allocation.shares[j];
      out += "{";
      bool share_first = true;
      append_key(out, "path", share_first);
      append_escaped(out, share.path.str());
      append_key(out, "generation", share_first);
      out += std::to_string(share.generation.value());
      append_key(out, "granted", share_first);
      out += std::to_string(share.granted);
      append_key(out, "reserved", share_first);
      out += std::to_string(share.reserved);
      append_key(out, "delta", share_first);
      out += std::to_string(share.delta_from_incumbent);
      out += "}";
    }
    out += "]}";
  }
  out += "]";

  append_key(out, "saturated_resources", first);
  out += "[";
  for (std::size_t i = 0; i < saturated_resources.size(); ++i) {
    if (i != 0) out.push_back(',');
    const auto& resource = saturated_resources[i];
    out += "{";
    bool resource_first = true;
    append_key(out, "resource", resource_first);
    append_escaped(out, resource.resource.str());
    append_key(out, "usable", resource_first);
    out += std::to_string(resource.usable_capacity);
    append_key(out, "committed", resource_first);
    out += std::to_string(resource.committed_load);
    append_key(out, "reserved", resource_first);
    out += std::to_string(resource.reserved);
    append_key(out, "allocated", resource_first);
    out += std::to_string(resource.allocated);
    append_key(out, "headroom", resource_first);
    out += std::to_string(resource.headroom);
    append_key(out, "utilization_permille", resource_first);
    out += std::to_string(resource.utilization_permille);
    out += "}";
  }
  out += "]";

  append_key(out, "churn", first);
  out += "{";
  bool churn_first = true;
  append_key(out, "decision", churn_first);
  append_escaped(out, to_string(churn.decision));
  append_key(out, "incumbent_score", churn_first);
  out += std::to_string(churn.incumbent_score);
  append_key(out, "proposal_score", churn_first);
  out += std::to_string(churn.proposal_score);
  append_key(out, "improvement_permille", churn_first);
  out += std::to_string(churn.improvement_permille);
  append_key(out, "required_improvement_permille", churn_first);
  out += std::to_string(churn.required_improvement_permille);
  append_key(out, "moved_bandwidth", churn_first);
  out += std::to_string(churn.moved_bandwidth);
  append_key(out, "moved_permille", churn_first);
  out += std::to_string(churn.moved_permille);
  append_key(out, "demands_changed", churn_first);
  out += std::to_string(churn.demands_changed);
  append_key(out, "paths_added", churn_first);
  out += std::to_string(churn.paths_added);
  append_key(out, "paths_removed", churn_first);
  out += std::to_string(churn.paths_removed);
  append_key(out, "failure_domains_changed", churn_first);
  out += std::to_string(churn.failure_domains_changed);
  append_key(out, "operational_risk_permille", churn_first);
  out += std::to_string(churn.operational_risk_permille);
  append_key(out, "rationale", churn_first);
  append_escaped(out, churn.rationale);
  out += "}";

  append_key(out, "policy_exclusions", first);
  out += "[";
  for (std::size_t i = 0; i < policy_exclusions.size(); ++i) {
    if (i != 0) out.push_back(',');
    append_escaped(out, policy_exclusions[i]);
  }
  out += "]";

  append_key(out, "alternatives", first);
  out += "[";
  for (std::size_t i = 0; i < alternatives.size(); ++i) {
    if (i != 0) out.push_back(',');
    const auto& alternative = alternatives[i];
    out += "{";
    bool alternative_scope = true;
    append_key(out, "subject", alternative_scope);
    append_escaped(out, alternative.subject);
    append_key(out, "reason", alternative_scope);
    append_escaped(out, alternative.reason);
    append_key(out, "delta", alternative_scope);
    out += std::to_string(alternative.delta);
    out += "}";
  }
  out += "]";

  append_key(out, "notes", first);
  out += "[";
  for (std::size_t i = 0; i < notes.size(); ++i) {
    if (i != 0) out.push_back(',');
    append_escaped(out, notes[i]);
  }
  out += "]";

  append_key(out, "truncated", first);
  out += truncated ? "true" : "false";
  out += "}";
  return out;
}

std::string Explanation::to_text() const {
  std::string out;
  out += "Traffic Engineering Fabric explanation\n";
  out += "  plan:            " + plan.id.str() + " generation " +
         std::to_string(plan.generation.value()) + "\n";
  out += "  state:           " + std::string(to_string(state)) + "\n";
  out += "  applicability:   " + std::string(to_string(applicability)) + "\n";
  out += "  feasibility:     " + std::string(to_string(feasibility.status)) + "\n";
  if (!feasibility.summary.empty()) out += "  summary:         " + feasibility.summary + "\n";
  out += "  authority:       epoch=" + std::to_string(authority.fabric_epoch.value()) +
         " topology=" + std::to_string(authority.topology_generation.value()) +
         " link_state=" + std::to_string(authority.link_state_generation.value()) +
         " path_authority=" + std::to_string(authority.path_authority_generation.value()) +
         " failure_domains=" + std::to_string(authority.failure_domain_generation.value()) + "\n";
  out += "  capacity:        " + authority.capacity.id.str() + "@" +
         std::to_string(authority.capacity.generation.value()) + "\n";
  out += "  reservations:    " + authority.reservations.id.str() + "@" +
         std::to_string(authority.reservations.generation.value()) + "\n";
  out += "  policy:          " + authority.policy.id.str() + "@" +
         std::to_string(authority.policy.generation.value()) + "\n";
  out += "  objective:       " + authority.objective.id.str() + "@" +
         std::to_string(authority.objective.generation.value()) + "\n";
  out += "  candidate set:   " + authority.candidate_set.id.str() + "@" +
         std::to_string(authority.candidate_set.generation.value()) + "\n";
  out += "  authority digest:" + authority.digest().hex() + "\n";
  out += "  objective score: " + std::to_string(total_score) + "\n";
  for (const auto& component : components) {
    out += "    - " + std::string(to_string(component.term)) + " weight=" +
           std::to_string(component.weight) + " raw=" + std::to_string(component.raw) +
           " weighted=" + std::to_string(component.weighted) + " " + component.unit + "\n";
  }

  out += "  allocations:\n";
  for (const auto& allocation : allocations) {
    out += "    - " + allocation.demand.str() + " min=" + std::to_string(allocation.minimum) +
           " desired=" + std::to_string(allocation.desired) +
           " granted=" + std::to_string(allocation.granted) +
           " reserved=" + std::to_string(allocation.reserved);
    if (allocation.shortfall_against_minimum > 0) {
      out += " SHORTFALL(min)=" + std::to_string(allocation.shortfall_against_minimum);
    }
    out += "\n";
    for (const auto& share : allocation.shares) {
      out += "        " + share.path.str() + " = " + std::to_string(share.granted);
      if (share.delta_from_incumbent != 0) {
        out += " (delta " + std::to_string(share.delta_from_incumbent) + ")";
      }
      out += "\n";
    }
  }

  if (!binding_constraints.empty()) {
    out += "  binding constraints:\n";
    for (const auto& constraint : binding_constraints) {
      out += "    - [" + std::string(to_string(constraint.kind)) + "] " + constraint.subject +
             " required=" + std::to_string(constraint.required) +
             " available=" + std::to_string(constraint.available) +
             " slack=" + std::to_string(constraint.slack) + " " + constraint.detail + "\n";
    }
  }
  if (!saturated_resources.empty()) {
    out += "  saturated resources:\n";
    for (const auto& resource : saturated_resources) {
      out += "    - " + resource.resource.str() + " usable=" +
             std::to_string(resource.usable_capacity) +
             " committed=" + std::to_string(resource.committed_load) +
             " reserved=" + std::to_string(resource.reserved) +
             " allocated=" + std::to_string(resource.allocated) +
             " utilization=" + std::to_string(resource.utilization_permille) + " permille\n";
    }
  }
  if (churn.decision != ChurnDecision::no_incumbent) {
    out += "  churn:           " + std::string(to_string(churn.decision)) +
           " improvement=" + std::to_string(churn.improvement_permille) +
           " permille required=" + std::to_string(churn.required_improvement_permille) +
           " moved=" + std::to_string(churn.moved_bandwidth) + " fbu (" +
           std::to_string(churn.moved_permille) + " permille) risk=" +
           std::to_string(churn.operational_risk_permille) + " permille\n";
    if (!churn.rationale.empty()) out += "                   " + churn.rationale + "\n";
  }
  if (!policy_exclusions.empty()) {
    out += "  policy exclusions:\n";
    for (const auto& exclusion : policy_exclusions) out += "    - " + exclusion + "\n";
  }
  if (!alternatives.empty()) {
    out += "  alternatives rejected:\n";
    for (const auto& alternative : alternatives) {
      out += "    - " + alternative.subject + ": " + alternative.reason + "\n";
    }
  }
  if (!notes.empty()) {
    out += "  notes:\n";
    for (const auto& note : notes) out += "    - " + note + "\n";
  }
  if (truncated) {
    out += "  (output truncated: omitted " + std::to_string(omitted_binding_constraints) +
           " binding constraints, " + std::to_string(omitted_alternatives) + " alternatives, " +
           std::to_string(omitted_allocations) + " allocations)\n";
  }
  return out;
}

}  // namespace tef
