// Traffic Engineering Fabric - deterministic derivation of planning inputs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "tef/derived.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "tef/numeric.hpp"

namespace tef {

const ResourceResidual* ResidualSet::find(const ResourceId& id) const noexcept {
  for (const auto& entry : resources) {
    if (entry.resource == id) return &entry;
  }
  return nullptr;
}

std::int64_t ResidualSet::total_available() const noexcept {
  std::int64_t total = 0;
  for (const auto& entry : resources) total = sat_add(total, entry.available);
  return total;
}

const CandidatePath* DemandDerivation::path(const std::vector<CandidatePath>& catalog,
                                            const PathId& id) const noexcept {
  for (const auto& entry : catalog) {
    if (entry.id == id) return &entry;
  }
  return nullptr;
}

namespace {

BindingConstraint make_constraint(ConstraintKind kind, std::string subject) {
  BindingConstraint constraint;
  constraint.kind = kind;
  constraint.subject = std::move(subject);
  constraint.detail = std::string(to_string(kind));
  return constraint;
}

// Canonical eligibility order: cost ascending, then latency (paths without
// latency evidence sort last), then identity. Deterministic for any input.
bool path_less(const CandidatePath& a, const CandidatePath& b) {
  if (a.cost != b.cost) return a.cost < b.cost;
  const std::int64_t a_latency = a.latency_micros.value_or(Limits::max_latency_micros + 1);
  const std::int64_t b_latency = b.latency_micros.value_or(Limits::max_latency_micros + 1);
  if (a_latency != b_latency) return a_latency < b_latency;
  return a.id < b.id;
}

template <class IdT>
bool contains_id(const std::vector<IdT>& ids, const IdT& id) {
  return std::binary_search(ids.begin(), ids.end(), id);
}

bool contains_domain(const std::vector<FailureDomainId>& ids, const FailureDomainId& id) {
  return std::binary_search(ids.begin(), ids.end(), id);
}

// A reservation may be displaced only when the active policy permits preemption
// AND the reserving authority has marked it preemptible. The stronger
// 'preemptible_with_authority' class additionally requires that the reservation
// has been attached to a demand in this snapshot, which is how the adjacent
// reservation authority signals that the cover may be re-planned.
bool policy_permits_preemption(const Policy& policy, Preemptibility preemptibility,
                               bool attached_to_demand) {
  if (!policy.allow_preemption) return false;
  switch (preemptibility) {
    case Preemptibility::not_preemptible:
      return false;
    case Preemptibility::preemptible:
      return true;
    case Preemptibility::preemptible_with_authority:
      return attached_to_demand;
  }
  return false;
}

}  // namespace

Result<Derivation> derive(const FabricSnapshot& snapshot) {
  Derivation derivation;

  // ---- 1. Base residuals -------------------------------------------------
  std::map<ResourceId, ResourceResidual> residuals;
  for (const auto& resource : snapshot.capacity.resources) {
    ResourceResidual entry;
    entry.resource = resource.id;
    entry.generation = resource.generation;
    entry.usable_capacity = resource.usable_capacity;
    entry.committed_load = resource.committed_load;
    entry.reserved = 0;
    entry.available = 0;
    residuals.emplace(resource.id, entry);
  }

  // ---- 2. Which demand covers which reservation ---------------------------
  std::map<DemandId, bool> demand_present;
  for (const auto& demand : snapshot.demands) demand_present.emplace(demand.id, true);

  std::map<ReservationId, DemandId> binding_owner;
  for (const auto& demand : snapshot.demands) {
    for (const auto& binding : demand.reservation_bindings) {
      auto it = binding_owner.find(binding);
      if (it == binding_owner.end()) {
        binding_owner.emplace(binding, demand.id);
      } else if (demand.id < it->second) {
        it->second = demand.id;  // deterministic: lexicographically smallest claimant
      }
    }
  }

  std::map<DemandId, std::int64_t> obligation;
  for (const auto& demand : snapshot.demands) obligation.emplace(demand.id, 0);

  // ---- 3. Reservation accounting -----------------------------------------
  for (const auto& reservation : snapshot.reservations.reservations) {
    ReservationAccounting accounting;
    accounting.reservation = reservation.id;
    accounting.generation = reservation.generation;
    accounting.bandwidth = reservation.bandwidth;
    accounting.active = reservation.effective.covers(snapshot.evaluation_tick);

    bool bound = false;
    auto binding_it = binding_owner.find(reservation.id);
    if (binding_it != binding_owner.end()) bound = true;

    const bool owned = reservation.owner.valid() && demand_present.count(reservation.owner) != 0;
    const bool authority_permits =
        policy_permits_preemption(snapshot.policy, reservation.preemptibility, bound || owned);

    accounting.displaceable = snapshot.policy.allow_preemption && authority_permits;

    DemandId charged_to;
    if (owned) {
      charged_to = reservation.owner;
    } else if (bound) {
      charged_to = binding_it->second;
    }

    if (!accounting.active) {
      accounting.disposition = "inactive: reservation interval does not cover the evaluation tick";
      derivation.policy_exclusions.push_back("reservation " + reservation.id.str() +
                                              " is not effective at the evaluation tick");
    } else if (accounting.displaceable) {
      // Displacement is checked before demand cover: when the active policy
      // permits preemption and the reserving authority has marked the
      // reservation preemptible, the obligation is not charged as a floor. The
      // plan may then re-cover it subject to the covering demand's own bounds.
      accounting.disposition =
          "displaceable: policy allows preemption and the reserving authority marked it preemptible";
      derivation.policy_exclusions.push_back("reservation " + reservation.id.str() +
                                              " is displaceable under the active policy");
    } else if (charged_to.valid()) {
      accounting.charged_to = charged_to;
      accounting.disposition = "charged to demand " + charged_to.str();
      obligation[charged_to] = sat_add(obligation[charged_to], reservation.bandwidth);
    } else {
      accounting.disposition = "reserved: charged to the resource residual";
      for (const auto& resource : reservation.resources) {
        auto it = residuals.find(resource);
        if (it == residuals.end()) {
          // validate_structure already rejects unknown references; defend anyway.
          return fail_as<Derivation>(ErrorCode::unknown_reference,
                                     "reservation references unknown resource " + resource.str());
        }
        it->second.reserved = sat_add(it->second.reserved, reservation.bandwidth);
      }
    }
    derivation.reservations.push_back(std::move(accounting));
  }

  // ---- 4. Availability ----------------------------------------------------
  for (auto& kv : residuals) {
    ResourceResidual& entry = kv.second;
    const auto claimed = add_checked(entry.committed_load, entry.reserved);
    if (!claimed) {
      return fail_as<Derivation>(ErrorCode::numeric_overflow,
                                 "resource " + entry.resource.str() + " accounting overflows");
    }
    if (*claimed > entry.usable_capacity) {
      BindingConstraint constraint = make_constraint(
          ConstraintKind::reservation_obligation, "resource/" + entry.resource.str());
      constraint.resource = entry.resource;
      constraint.required = *claimed;
      constraint.available = entry.usable_capacity;
      constraint.slack = entry.usable_capacity - *claimed;
      constraint.detail = "committed load plus non-displaceable reservations exceed usable capacity";
      derivation.constraints.push_back(std::move(constraint));
      entry.available = 0;
    } else {
      entry.available = entry.usable_capacity - *claimed;
    }
    derivation.residuals.resources.push_back(entry);
  }

  // ---- 5. Per-demand derivation ------------------------------------------
  for (const auto& demand : snapshot.demands) {
    DemandDerivation out;
    out.demand = demand.id;
    out.generation = demand.generation;
    out.minimum = demand.minimum_bandwidth;
    out.reservation_obligation = obligation[demand.id];
    out.floor = std::max(demand.minimum_bandwidth, out.reservation_obligation);
    out.desired = std::max(demand.desired_bandwidth, out.floor);
    out.maximum = demand.maximum_bandwidth;
    out.priority = demand.priority;
    out.tenant = demand.tenant;
    out.service_class = demand.service_class;
    out.active = demand.effective.covers(snapshot.evaluation_tick);

    if (!out.active) {
      derivation.policy_exclusions.push_back("demand " + demand.id.str() +
                                              " is not effective at the evaluation tick");
      derivation.demands.push_back(std::move(out));
      continue;
    }
    derivation.any_demand_active = true;

    if (out.floor > out.maximum) {
      BindingConstraint constraint =
          make_constraint(ConstraintKind::demand_maximum, "demand/" + demand.id.str());
      constraint.demand = demand.id;
      constraint.required = out.floor;
      constraint.available = out.maximum;
      constraint.slack = out.maximum - out.floor;
      constraint.detail =
          "the demand hard floor (minimum or reservation obligation) exceeds its maximum bandwidth";
      out.notes.push_back(std::move(constraint));
    }

    if (!snapshot.policy.allowed_tenants.empty() &&
        !contains_id<TenantId>(snapshot.policy.allowed_tenants, demand.tenant)) {
      BindingConstraint constraint =
          make_constraint(ConstraintKind::tenant_policy, "demand/" + demand.id.str());
      constraint.demand = demand.id;
      constraint.detail = "the demand tenant is not permitted by the active policy";
      out.notes.push_back(std::move(constraint));
      derivation.policy_exclusions.push_back("demand " + demand.id.str() +
                                             " is excluded: its tenant is not permitted by policy");
    }
    if (!snapshot.policy.allowed_service_classes.empty() &&
        !contains_id<ServiceClassId>(snapshot.policy.allowed_service_classes, demand.service_class)) {
      BindingConstraint constraint =
          make_constraint(ConstraintKind::service_class_policy, "demand/" + demand.id.str());
      constraint.demand = demand.id;
      constraint.detail = "the demand service class is not permitted by the active policy";
      out.notes.push_back(std::move(constraint));
      derivation.policy_exclusions.push_back(
          "demand " + demand.id.str() +
          " is excluded: its service class is not permitted by policy");
    }

    std::vector<const CandidatePath*> eligible;
    for (const auto& path : snapshot.paths) {
      if (!(path.candidate_set == demand.candidate_set)) {
        out.excluded_paths.push_back(ExcludedPath{path.id, ConstraintKind::path_eligibility,
                                                  "candidate set generation does not match the demand"});
        continue;
      }
      if (!demand.allowed_paths.empty() && !contains_id(demand.allowed_paths, path.id)) {
        out.excluded_paths.push_back(ExcludedPath{path.id, ConstraintKind::path_eligibility,
                                                  "path is not in the demand allow list"});
        continue;
      }
      if (!demand.forbidden_paths.empty() && contains_id(demand.forbidden_paths, path.id)) {
        out.excluded_paths.push_back(ExcludedPath{path.id, ConstraintKind::forbidden_path,
                                                  "path is explicitly forbidden for the demand"});
        continue;
      }
      if (path.scope == EligibilityScope::tenant && !(path.scope_tenant == demand.tenant)) {
        out.excluded_paths.push_back(ExcludedPath{path.id, ConstraintKind::path_eligibility,
                                                  "path tenant scope excludes the demand tenant"});
        continue;
      }
      if (path.scope == EligibilityScope::service_class &&
          !(path.scope_service_class == demand.service_class)) {
        out.excluded_paths.push_back(ExcludedPath{path.id, ConstraintKind::path_eligibility,
                                                  "path service-class scope excludes the demand"});
        continue;
      }
      if (demand.latency_bound_micros.has_value()) {
        if (!path.latency_micros.has_value()) {
          out.excluded_paths.push_back(ExcludedPath{
              path.id, ConstraintKind::latency_bound,
              "demand declares a latency bound but the path carries no latency evidence"});
          continue;
        }
        if (*path.latency_micros > *demand.latency_bound_micros) {
          out.excluded_paths.push_back(ExcludedPath{path.id, ConstraintKind::latency_bound,
                                                    "path latency exceeds the demand latency bound"});
          continue;
        }
      }
      if (demand.path_cost_ceiling.has_value() && path.cost > *demand.path_cost_ceiling) {
        out.excluded_paths.push_back(ExcludedPath{path.id, ConstraintKind::path_cost_ceiling,
                                                  "path cost exceeds the demand cost ceiling"});
        continue;
      }
      bool domains_ok = true;
      for (const auto& domain : demand.required_failure_domains) {
        if (!contains_domain(path.failure_domains, domain)) {
          domains_ok = false;
          break;
        }
      }
      if (!domains_ok) {
        out.excluded_paths.push_back(ExcludedPath{
            path.id, ConstraintKind::failure_domain_diversity,
            "path does not traverse every failure domain the demand requires"});
        continue;
      }
      eligible.push_back(&path);
    }

    std::sort(eligible.begin(), eligible.end(),
              [](const CandidatePath* a, const CandidatePath* b) { return path_less(*a, *b); });

    const std::size_t limit = snapshot.policy.max_paths_per_demand == 0
                                  ? eligible.size()
                                  : std::min<std::size_t>(eligible.size(), snapshot.policy.max_paths_per_demand);
    if (limit < eligible.size()) {
      BindingConstraint constraint =
          make_constraint(ConstraintKind::path_count_limit, "demand/" + demand.id.str());
      constraint.demand = demand.id;
      constraint.required = static_cast<std::int64_t>(eligible.size());
      constraint.available = static_cast<std::int64_t>(limit);
      constraint.slack = constraint.available - constraint.required;
      constraint.detail = "policy path-count bound truncated the eligible path list";
      out.notes.push_back(std::move(constraint));
      for (std::size_t i = limit; i < eligible.size(); ++i) {
        out.excluded_paths.push_back(ExcludedPath{eligible[i]->id, ConstraintKind::path_count_limit,
                                                  "excluded by the policy path-count bound"});
      }
    }
    for (std::size_t i = 0; i < limit; ++i) out.eligible_paths.push_back(eligible[i]->id);

    std::sort(out.excluded_paths.begin(), out.excluded_paths.end());
    out.excluded_paths.erase(
        std::unique(out.excluded_paths.begin(), out.excluded_paths.end()),
        out.excluded_paths.end());

    if (out.eligible_paths.empty()) {
      BindingConstraint constraint =
          make_constraint(ConstraintKind::path_eligibility, "demand/" + demand.id.str());
      constraint.demand = demand.id;
      constraint.required = out.floor;
      constraint.available = 0;
      constraint.slack = -out.floor;
      constraint.detail = "the demand has no eligible candidate path under the current policy";
      out.notes.push_back(std::move(constraint));
    }

    derivation.demands.push_back(std::move(out));
  }

  std::sort(derivation.constraints.begin(), derivation.constraints.end());
  std::sort(derivation.policy_exclusions.begin(), derivation.policy_exclusions.end());
  derivation.policy_exclusions.erase(
      std::unique(derivation.policy_exclusions.begin(), derivation.policy_exclusions.end()),
      derivation.policy_exclusions.end());
  derivation.reservations.shrink_to_fit();
  return derivation;
}

}  // namespace tef
