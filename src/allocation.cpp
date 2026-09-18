// Traffic Engineering Fabric - allocation verification and churn control.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "tef/allocation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "tef/binary.hpp"
#include "tef/numeric.hpp"

namespace tef {
namespace {

BindingConstraint violation(ConstraintKind kind, std::string subject, std::string detail) {
  BindingConstraint constraint;
  constraint.kind = kind;
  constraint.subject = std::move(subject);
  constraint.detail = std::move(detail);
  return constraint;
}

}  // namespace

const DemandAllocation* Allocation::find_demand(const DemandId& id) const noexcept {
  for (const auto& entry : demands) {
    if (entry.demand == id) return &entry;
  }
  return nullptr;
}

Digest Allocation::digest() const {
  Writer w;
  w.u32(static_cast<std::uint32_t>(demands.size()));
  for (const auto& demand : demands) {
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
      w.i64(share.minimum);
      w.i64(share.desired);
      w.i64(share.granted);
      w.i64(share.reserved);
    }
  }
  w.u32(static_cast<std::uint32_t>(resources.size()));
  for (const auto& resource : resources) {
    w.str(resource.resource.str());
    w.u64(resource.generation.value());
    w.i64(resource.allocated);
  }
  w.boolean(degraded);
  return w.finish_digest("tef.allocation.v1");
}

// ---------------------------------------------------------------------------
// Independent verification
// ---------------------------------------------------------------------------

std::vector<BindingConstraint> verify_allocation(const FabricSnapshot& snapshot,
                                                 const Allocation& allocation) {
  std::vector<BindingConstraint> violations;

  const Result<Derivation> derivation_result = derive(snapshot);
  if (!derivation_result.has_value()) {
    violations.push_back(violation(ConstraintKind::structural, "snapshot",
                                   "derivation failed: " + derivation_result.error().format()));
    return violations;
  }
  const Derivation& derivation = derivation_result.value();

  std::map<DemandId, const DemandDerivation*> expected;
  for (const auto& entry : derivation.demands) expected.emplace(entry.demand, &entry);

  // No unknown demand may appear in the allocation.
  for (const auto& entry : allocation.demands) {
    if (expected.find(entry.demand) == expected.end()) {
      violations.push_back(violation(ConstraintKind::unknown_demand_reference,
                                     "demand/" + entry.demand.str(),
                                     "the allocation contains a demand that is not in the snapshot"));
    }
  }

  std::map<ResourceId, std::int64_t> per_resource_allocated;

  for (const auto& entry : allocation.demands) {
    auto it = expected.find(entry.demand);
    if (it == expected.end()) continue;
    const DemandDerivation& demand = *it->second;

    if (!(entry.generation == demand.generation)) {
      violations.push_back(violation(ConstraintKind::stale_generation, "demand/" + entry.demand.str(),
                                     "the allocation binds a demand generation that is not current"));
      continue;
    }

    std::int64_t share_total = 0;
    std::int64_t share_reserved = 0;
    std::set<PathId> used;
    for (const auto& share : entry.shares) {
      if (share.granted < 0) {
        violations.push_back(violation(ConstraintKind::structural, "demand/" + entry.demand.str(),
                                       "negative granted bandwidth on path " + share.path.str()));
      }
      if (!used.insert(share.path).second) {
        violations.push_back(violation(ConstraintKind::duplicate_identity,
                                       "demand/" + entry.demand.str(),
                                       "duplicate path share for " + share.path.str()));
      }
      share_total = sat_add(share_total, share.granted);
      share_reserved = sat_add(share_reserved, share.reserved);

      if (share.granted == 0) continue;

      const auto eligible = std::find(demand.eligible_paths.begin(), demand.eligible_paths.end(),
                                      share.path);
      if (eligible == demand.eligible_paths.end()) {
        violations.push_back(violation(ConstraintKind::path_eligibility,
                                       "demand/" + entry.demand.str() + "/path/" + share.path.str(),
                                       "the allocation uses a path that is not eligible for the demand"));
        continue;
      }
      const CandidatePath* path = nullptr;
      for (const auto& candidate : snapshot.paths) {
        if (candidate.id == share.path) {
          path = &candidate;
          break;
        }
      }
      if (path == nullptr) {
        violations.push_back(violation(ConstraintKind::path_eligibility,
                                       "demand/" + entry.demand.str() + "/path/" + share.path.str(),
                                       "the allocation uses a path absent from the candidate set"));
        continue;
      }
      if (!(share.generation == path->generation)) {
        violations.push_back(violation(
            ConstraintKind::path_authority_generation,
            "demand/" + entry.demand.str() + "/path/" + share.path.str(),
            "the allocation binds a path generation that is not the current candidate generation"));
        continue;
      }
      if (!(path->authority.generation == snapshot.path_authority_generation)) {
        violations.push_back(violation(
            ConstraintKind::path_authority_generation, "path/" + share.path.str(),
            "the candidate path was not authorized under the current path authority generation"));
      }
      for (const auto& resource : path->resources) {
        per_resource_allocated[resource] = sat_add(per_resource_allocated[resource], share.granted);
      }
    }

    if (share_total != entry.granted) {
      violations.push_back(violation(
          ConstraintKind::structural, "demand/" + entry.demand.str(),
          "the demand granted total does not equal the sum of its path shares"));
    }
    if (share_reserved > entry.granted) {
      violations.push_back(violation(ConstraintKind::structural, "demand/" + entry.demand.str(),
                                     "reserved bandwidth exceeds granted bandwidth"));
    }
    if (entry.granted < 0) {
      violations.push_back(violation(ConstraintKind::structural, "demand/" + entry.demand.str(),
                                     "granted bandwidth is negative"));
    }
    if (entry.granted > demand.maximum) {
      violations.push_back(violation(ConstraintKind::demand_maximum, "demand/" + entry.demand.str(),
                                     "granted bandwidth exceeds the demand maximum"));
    }
    if (demand.active && !allocation.degraded && entry.granted < demand.floor) {
      violations.push_back(violation(ConstraintKind::demand_minimum, "demand/" + entry.demand.str(),
                                     "granted bandwidth is below the demand hard floor"));
    }
  }

  for (const auto& resource : derivation.residuals.resources) {
    const auto it = per_resource_allocated.find(resource.resource);
    const std::int64_t allocated = it == per_resource_allocated.end() ? 0 : it->second;
    if (allocated > resource.available) {
      BindingConstraint constraint = violation(
          ConstraintKind::resource_capacity, "resource/" + resource.resource.str(),
          "allocated bandwidth exceeds the authoritative available capacity");
      constraint.resource = resource.resource;
      constraint.required = allocated;
      constraint.available = resource.available;
      constraint.slack = resource.available - allocated;
      violations.push_back(std::move(constraint));
    }
  }

  std::sort(violations.begin(), violations.end());
  violations.erase(std::unique(violations.begin(), violations.end()), violations.end());
  return violations;
}

// ---------------------------------------------------------------------------
// Churn
// ---------------------------------------------------------------------------

std::string_view to_string(ChurnDecision decision) noexcept {
  switch (decision) {
    case ChurnDecision::no_incumbent: return "NO_INCUMBENT";
    case ChurnDecision::accept: return "ACCEPT";
    case ChurnDecision::hold_incumbent: return "HOLD_INCUMBENT";
    case ChurnDecision::reject_regression: return "REJECT_REGRESSION";
  }
  return "UNKNOWN";
}

ChurnReport compare_churn(const Allocation& incumbent, const Allocation& proposal,
                          const FabricSnapshot& snapshot, const Policy& policy,
                          std::int64_t incumbent_score, std::int64_t proposal_score) {
  ChurnReport report;
  report.incumbent_score = incumbent_score;
  report.proposal_score = proposal_score;
  report.required_improvement_permille = policy.churn_improvement_threshold_permille;
  report.max_moved_permille = policy.churn_max_moved_bandwidth_permille;
  report.max_affected_demands = policy.churn_max_affected_demands;

  using Key = std::pair<std::string, std::string>;
  std::map<Key, std::int64_t> incumbent_shares;
  std::map<Key, std::int64_t> proposal_shares;
  std::map<DemandId, bool> changed;

  for (const auto& demand : incumbent.demands) {
    for (const auto& share : demand.shares) {
      incumbent_shares[{demand.demand.str(), share.path.str()}] = share.granted;
    }
  }
  for (const auto& demand : proposal.demands) {
    for (const auto& share : demand.shares) {
      proposal_shares[{demand.demand.str(), share.path.str()}] = share.granted;
    }
  }

  std::set<Key> keys;
  for (const auto& entry : incumbent_shares) keys.insert(entry.first);
  for (const auto& entry : proposal_shares) keys.insert(entry.first);

  std::int64_t moved = 0;
  for (const auto& key : keys) {
    const auto a = incumbent_shares.find(key);
    const auto b = proposal_shares.find(key);
    const std::int64_t before = a == incumbent_shares.end() ? 0 : a->second;
    const std::int64_t after = b == proposal_shares.end() ? 0 : b->second;
    if (before == after) continue;
    moved = sat_add(moved, before > after ? before - after : after - before);
    if (before > 0 && after == 0) ++report.paths_removed;
    if (before == 0 && after > 0) ++report.paths_added;
    const DemandId parsed = *DemandId::parse(key.first);
    changed[parsed] = true;
  }
  report.moved_bandwidth = moved;
  report.total_bandwidth = std::max(incumbent.total_granted, proposal.total_granted);
  report.moved_permille = permille(moved, report.total_bandwidth);
  report.demands_changed = static_cast<std::uint32_t>(changed.size());

  // Failure-domain concentration change, derived from the snapshot's path to
  // failure-domain mapping.
  std::map<std::string, std::int64_t> incumbent_domains;
  std::map<std::string, std::int64_t> proposal_domains;
  const auto accumulate = [&snapshot](const Allocation& allocation,
                                      std::map<std::string, std::int64_t>& out) {
    for (const auto& demand : allocation.demands) {
      for (const auto& share : demand.shares) {
        if (share.granted == 0) continue;
        for (const auto& path : snapshot.paths) {
          if (!(path.id == share.path)) continue;
          for (const auto& domain : path.failure_domains) {
            out[domain.str()] = sat_add(out[domain.str()], share.granted);
          }
          break;
        }
      }
    }
  };
  accumulate(incumbent, incumbent_domains);
  accumulate(proposal, proposal_domains);
  std::set<std::string> all_domains;
  for (const auto& entry : incumbent_domains) all_domains.insert(entry.first);
  for (const auto& entry : proposal_domains) all_domains.insert(entry.first);
  for (const auto& domain : all_domains) {
    const auto a = incumbent_domains.find(domain);
    const auto b = proposal_domains.find(domain);
    const std::int64_t before = a == incumbent_domains.end() ? 0 : a->second;
    const std::int64_t after = b == proposal_domains.end() ? 0 : b->second;
    if (before != after) ++report.failure_domains_changed;
  }

  // Improvement is expressed in permille of the incumbent score. Scores are
  // lower-is-better and non-negative by construction.
  if (incumbent_score > 0) {
    const std::int64_t delta = incumbent_score - proposal_score;
    report.improvement_permille = permille(delta, incumbent_score);
  } else {
    report.improvement_permille = proposal_score <= 0 ? 0 : -1000;
  }

  // Deterministic operational risk composite. Fixed weights, documented in
  // docs/algorithm.md; it is informational and is a policy gate only when the
  // policy sets a bound below 1000.
  const std::int64_t demand_share = permille(static_cast<std::int64_t>(report.demands_changed),
                                             static_cast<std::int64_t>(incumbent.demands.size()));
  const std::int64_t domain_share =
      permille(static_cast<std::int64_t>(report.failure_domains_changed),
               static_cast<std::int64_t>(all_domains.size()));
  report.operational_risk_permille =
      std::min<std::int64_t>(1000, (report.moved_permille * 60 + demand_share * 25 + domain_share * 15) / 100);

  const bool has_incumbent = !incumbent.demands.empty() || incumbent.total_granted > 0;
  if (!has_incumbent) {
    report.decision = ChurnDecision::no_incumbent;
    report.rationale = "no incumbent allocation was supplied; the proposal is not compared against one";
    return report;
  }

  if (proposal_score > incumbent_score) {
    report.decision = ChurnDecision::reject_regression;
    report.rationale = "the proposal scores worse than the incumbent";
    return report;
  }

  std::vector<std::string> failures;
  if (report.improvement_permille < policy.churn_improvement_threshold_permille) {
    failures.push_back("improvement " + std::to_string(report.improvement_permille) +
                       " permille is below the required " +
                       std::to_string(policy.churn_improvement_threshold_permille) + " permille");
  }
  if (report.moved_permille > policy.churn_max_moved_bandwidth_permille) {
    failures.push_back("moved bandwidth " + std::to_string(report.moved_permille) +
                       " permille exceeds the bound " +
                       std::to_string(policy.churn_max_moved_bandwidth_permille) + " permille");
  }
  if (policy.churn_max_affected_demands != 0 && report.demands_changed > policy.churn_max_affected_demands) {
    failures.push_back("affected demands " + std::to_string(report.demands_changed) +
                       " exceed the bound " + std::to_string(policy.churn_max_affected_demands));
  }
  if (report.operational_risk_permille > policy.churn_max_operational_risk_permille) {
    failures.push_back("operational risk " + std::to_string(report.operational_risk_permille) +
                       " permille exceeds the bound " +
                       std::to_string(policy.churn_max_operational_risk_permille) + " permille");
  }

  if (failures.empty()) {
    report.decision = ChurnDecision::accept;
    report.rationale = "the proposal improves the objective and stays inside every churn bound";
  } else {
    report.decision = ChurnDecision::hold_incumbent;
    report.rationale = "the incumbent is retained: ";
    for (std::size_t i = 0; i < failures.size(); ++i) {
      if (i != 0) report.rationale += "; ";
      report.rationale += failures[i];
    }
  }
  return report;
}

}  // namespace tef
