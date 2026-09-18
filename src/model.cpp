// Traffic Engineering Fabric - canonicalization, validation and encoding.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "tef/model.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tef/binary.hpp"
#include "tef/numeric.hpp"
#include "tef/version.hpp"

namespace tef {
namespace {

template <class T, class Key>
void sort_unique_in_place(std::vector<T>& items, Key key, std::size_t& removed) {
  std::stable_sort(items.begin(), items.end(),
                   [&](const T& a, const T& b) { return key(a) < key(b); });
  const auto duplicates = static_cast<std::size_t>(
      std::unique(items.begin(), items.end(),
                  [&](const T& a, const T& b) { return key(a) == key(b); }) -
      items.begin());
  removed += items.size() - duplicates;
  items.resize(duplicates);
}

template <class T, class Key>
bool is_sorted_unique(const std::vector<T>& items, Key key) {
  for (std::size_t i = 1; i < items.size(); ++i) {
    if (!(key(items[i - 1]) < key(items[i]))) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Encoding helpers
// ---------------------------------------------------------------------------

void write_provenance(Writer& w, const Provenance& p) {
  w.str(p.source.str());
  w.u64(p.source_generation);
  w.u64(p.observed_tick);
  w.digest(p.evidence_digest);
  w.str(p.detail);
}

bool read_provenance(Reader& r, Provenance& p) {
  std::string source;
  if (!r.str(source, Limits::max_name_length)) return false;
  const auto parsed = SourceSystemId::parse(source);
  if (!parsed) return false;
  p.source = *parsed;
  if (!r.u64(p.source_generation)) return false;
  if (!r.u64(p.observed_tick)) return false;
  if (!r.digest(p.evidence_digest)) return false;
  if (!r.str(p.detail)) return false;
  return true;
}

void write_interval(Writer& w, const TimeInterval& t) {
  w.u64(t.start_tick);
  w.u64(t.end_tick);
  w.boolean(t.open_ended);
}

bool read_interval(Reader& r, TimeInterval& t) {
  if (!r.u64(t.start_tick)) return false;
  if (!r.u64(t.end_tick)) return false;
  if (!r.boolean(t.open_ended)) return false;
  return true;
}

template <class IdT>
void write_ids(Writer& w, const std::vector<IdT>& ids) {
  w.u32(static_cast<std::uint32_t>(ids.size()));
  for (const auto& id : ids) w.str(id.str());
}

template <class IdT>
bool read_ids(Reader& r, std::vector<IdT>& ids, std::size_t max_count) {
  std::uint32_t count = 0;
  if (!r.u32(count)) return false;
  if (count > max_count) {
    r.fail();
    return false;
  }
  ids.clear();
  ids.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string text;
    if (!r.str(text, Limits::max_name_length)) return false;
    const auto parsed = IdT::parse(text);
    if (!parsed) {
      r.fail();
      return false;
    }
    ids.push_back(*parsed);
  }
  return true;
}

void write_labels(Writer& w, const std::vector<std::pair<std::string, std::string>>& labels) {
  w.u32(static_cast<std::uint32_t>(labels.size()));
  for (const auto& [key, value] : labels) {
    w.str(key);
    w.str(value);
  }
}

bool read_labels(Reader& r, std::vector<std::pair<std::string, std::string>>& labels) {
  std::uint32_t count = 0;
  if (!r.u32(count)) return false;
  if (count > Limits::max_constraint_refs * 4u) {
    r.fail();
    return false;
  }
  labels.clear();
  labels.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string key;
    std::string value;
    if (!r.str(key, Limits::max_name_length)) return false;
    if (!r.str(value)) return false;
    labels.emplace_back(std::move(key), std::move(value));
  }
  return true;
}

void write_strings(Writer& w, const std::vector<std::string>& values) {
  w.u32(static_cast<std::uint32_t>(values.size()));
  for (const auto& value : values) w.str(value);
}

bool read_strings(Reader& r, std::vector<std::string>& values, std::size_t max_count) {
  std::uint32_t count = 0;
  if (!r.u32(count)) return false;
  if (count > max_count) {
    r.fail();
    return false;
  }
  values.clear();
  values.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string value;
    if (!r.str(value)) return false;
    values.push_back(std::move(value));
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Enum rendering
// ---------------------------------------------------------------------------

std::string_view to_string(Preemptibility value) noexcept {
  switch (value) {
    case Preemptibility::not_preemptible: return "not_preemptible";
    case Preemptibility::preemptible_with_authority: return "preemptible_with_authority";
    case Preemptibility::preemptible: return "preemptible";
  }
  return "unknown";
}

std::optional<Preemptibility> preemptibility_from_string(std::string_view text) noexcept {
  if (text == "not_preemptible") return Preemptibility::not_preemptible;
  if (text == "preemptible_with_authority") return Preemptibility::preemptible_with_authority;
  if (text == "preemptible") return Preemptibility::preemptible;
  return std::nullopt;
}

std::string_view to_string(EligibilityScope value) noexcept {
  switch (value) {
    case EligibilityScope::fabric_wide: return "fabric_wide";
    case EligibilityScope::tenant: return "tenant";
    case EligibilityScope::service_class: return "service_class";
  }
  return "unknown";
}

std::string_view to_string(ObjectiveTerm term) noexcept {
  switch (term) {
    case ObjectiveTerm::satisfy_minimums: return "satisfy_minimums";
    case ObjectiveTerm::minimize_max_utilization: return "minimize_max_utilization";
    case ObjectiveTerm::minimize_congestion_exposure: return "minimize_congestion_exposure";
    case ObjectiveTerm::minimize_total_path_cost: return "minimize_total_path_cost";
    case ObjectiveTerm::minimize_churn: return "minimize_churn";
    case ObjectiveTerm::preserve_reservations: return "preserve_reservations";
    case ObjectiveTerm::preserve_priority: return "preserve_priority";
    case ObjectiveTerm::maximize_desired_bandwidth: return "maximize_desired_bandwidth";
    case ObjectiveTerm::fairness_across_groups: return "fairness_across_groups";
    case ObjectiveTerm::minimize_failure_domain_concentration:
      return "minimize_failure_domain_concentration";
    case ObjectiveTerm::minimize_path_count: return "minimize_path_count";
  }
  return "unknown";
}

std::optional<ObjectiveTerm> objective_term_from_string(std::string_view text) noexcept {
  for (std::uint16_t i = 0; i <= static_cast<std::uint16_t>(ObjectiveTerm::minimize_path_count); ++i) {
    const auto term = static_cast<ObjectiveTerm>(i);
    if (to_string(term) == text) return term;
  }
  return std::nullopt;
}

std::int64_t ObjectiveProfile::weight_of(ObjectiveTerm term) const noexcept {
  for (const auto& entry : terms) {
    if (entry.term == term) return entry.weight;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Canonicalization
// ---------------------------------------------------------------------------

CanonicalizeReport canonicalize(FabricSnapshot& snapshot) {
  CanonicalizeReport report;
  const std::size_t before = 0;
  (void)before;

  auto& demands = snapshot.demands;
  for (auto& demand : demands) {
    sort_unique_in_place(demand.allowed_paths, [](const PathId& id) { return id; },
                         report.duplicate_entries_removed);
    sort_unique_in_place(demand.forbidden_paths, [](const PathId& id) { return id; },
                         report.duplicate_entries_removed);
    sort_unique_in_place(demand.affinity, [](const DemandId& id) { return id; },
                         report.duplicate_entries_removed);
    sort_unique_in_place(demand.anti_affinity, [](const DemandId& id) { return id; },
                         report.duplicate_entries_removed);
    sort_unique_in_place(demand.required_failure_domains,
                         [](const FailureDomainId& id) { return id; },
                         report.duplicate_entries_removed);
    sort_unique_in_place(demand.reservation_bindings, [](const ReservationId& id) { return id; },
                         report.duplicate_entries_removed);
    sort_unique_in_place(demand.labels, [](const std::pair<std::string, std::string>& kv) {
      return kv.first;
    }, report.duplicate_entries_removed);
  }
  std::stable_sort(demands.begin(), demands.end(),
                   [](const Demand& a, const Demand& b) { return a.id < b.id; });

  auto& paths = snapshot.paths;
  for (auto& path : paths) {
    sort_unique_in_place(path.resources, [](const ResourceId& id) { return id; },
                         report.duplicate_entries_removed);
    sort_unique_in_place(path.failure_domains, [](const FailureDomainId& id) { return id; },
                         report.duplicate_entries_removed);
    sort_unique_in_place(path.policy_labels, [](const std::string& value) { return value; },
                         report.duplicate_entries_removed);
  }
  std::stable_sort(paths.begin(), paths.end(),
                   [](const CandidatePath& a, const CandidatePath& b) { return a.id < b.id; });

  auto& resources = snapshot.capacity.resources;
  for (auto& resource : resources) {
    sort_unique_in_place(resource.failure_domains, [](const FailureDomainId& id) { return id; },
                         report.duplicate_entries_removed);
  }
  std::stable_sort(resources.begin(), resources.end(),
                   [](const FabricResource& a, const FabricResource& b) { return a.id < b.id; });

  auto& reservations = snapshot.reservations.reservations;
  for (auto& reservation : reservations) {
    sort_unique_in_place(reservation.paths, [](const PathId& id) { return id; },
                         report.duplicate_entries_removed);
    sort_unique_in_place(reservation.resources, [](const ResourceId& id) { return id; },
                         report.duplicate_entries_removed);
  }
  std::stable_sort(reservations.begin(), reservations.end(),
                   [](const Reservation& a, const Reservation& b) { return a.id < b.id; });

  // Objective term order is semantic (lexicographic priority) and is preserved
  // exactly. Duplicate terms are an input defect, not something to repair
  // silently, so canonicalization leaves them for validate_structure to reject.

  sort_unique_in_place(snapshot.policy.allowed_tenants, [](const TenantId& id) { return id; },
                       report.duplicate_entries_removed);
  sort_unique_in_place(snapshot.policy.allowed_service_classes,
                       [](const ServiceClassId& id) { return id; },
                       report.duplicate_entries_removed);

  // Detect whether the caller's ordering differed from canonical order.
  const bool needs_sort =
      !std::is_sorted(snapshot.demands.begin(), snapshot.demands.end(),
                      [](const Demand& a, const Demand& b) { return a.id < b.id; });
  (void)needs_sort;
  report.changed = report.changed || report.duplicate_entries_removed > 0;
  report.demands_sorted = snapshot.demands.size();
  report.paths_sorted = snapshot.paths.size();
  report.resources_sorted = snapshot.capacity.resources.size();
  report.reservations_sorted = snapshot.reservations.reservations.size();
  return report;
}

// ---------------------------------------------------------------------------
// Structural validation
// ---------------------------------------------------------------------------

namespace {

Status require(bool condition, ErrorCode code, std::string detail) {
  if (condition) return Status::success();
  return Status(Error(code, std::move(detail)));
}

template <class IdT>
bool has_duplicate_ids(const std::vector<IdT>& ids) {
  for (std::size_t i = 1; i < ids.size(); ++i) {
    if (!(ids[i - 1] < ids[i])) return true;
  }
  return false;
}

}  // namespace

Status validate_structure(const FabricSnapshot& snapshot) {
  if (!snapshot.fabric_epoch.valid()) {
    return Status(Error(ErrorCode::invalid_generation, "fabric epoch is absent"));
  }
  if (!snapshot.topology_generation.valid()) {
    return Status(Error(ErrorCode::invalid_generation, "topology generation is absent"));
  }
  if (!snapshot.link_state_generation.valid()) {
    return Status(Error(ErrorCode::invalid_generation, "link-state generation is absent"));
  }
  if (!snapshot.path_authority_generation.valid()) {
    return Status(Error(ErrorCode::invalid_generation, "path authority generation is absent"));
  }
  if (!snapshot.failure_domain_generation.valid()) {
    return Status(Error(ErrorCode::invalid_generation, "failure-domain generation is absent"));
  }
  if (!snapshot.provenance.valid()) {
    return Status(Error(ErrorCode::missing_provenance, "snapshot provenance is absent"));
  }
  if (!snapshot.candidate_set.valid()) {
    return Status(Error(ErrorCode::invalid_generation, "candidate set reference is incomplete"));
  }
  if (snapshot.demands.size() > Limits::max_demands) {
    return Status(Error(ErrorCode::limit_exceeded, "demand count exceeds the supported bound"));
  }
  if (snapshot.paths.size() > Limits::max_total_candidate_paths) {
    return Status(Error(ErrorCode::limit_exceeded, "candidate path count exceeds the supported bound"));
  }

  // Capacity snapshot.
  const auto& capacity = snapshot.capacity;
  if (!capacity.id.valid() || !capacity.generation.valid()) {
    return Status(Error(ErrorCode::invalid_generation, "capacity snapshot reference is incomplete"));
  }
  if (!capacity.provenance.valid()) {
    return Status(Error(ErrorCode::missing_provenance, "capacity snapshot provenance is absent"));
  }
  if (!(capacity.fabric_epoch == snapshot.fabric_epoch)) {
    return Status(Error(ErrorCode::conflicting_authority,
                        "capacity snapshot fabric epoch does not match the snapshot epoch"));
  }
  if (capacity.resources.size() > Limits::max_resources) {
    return Status(Error(ErrorCode::limit_exceeded, "resource count exceeds the supported bound"));
  }
  {
    std::vector<ResourceId> ids;
    ids.reserve(capacity.resources.size());
    for (const auto& resource : capacity.resources) {
      if (!resource.id.valid()) return Status(Error(ErrorCode::invalid_argument, "resource id is absent"));
      if (!resource.generation.valid()) {
        return Status(Error(ErrorCode::invalid_generation, "resource generation is absent for " + resource.id.str()));
      }
      if (!resource.provenance.valid()) {
        return Status(Error(ErrorCode::missing_provenance, "resource provenance is absent for " + resource.id.str()));
      }
      if (resource.usable_capacity < 0 || resource.usable_capacity > Limits::max_bandwidth) {
        return Status(Error(ErrorCode::numeric_invalid,
                            "resource usable capacity is out of range for " + resource.id.str()));
      }
      if (resource.committed_load < 0 || resource.committed_load > Limits::max_bandwidth) {
        return Status(Error(ErrorCode::numeric_invalid,
                            "resource committed load is out of range for " + resource.id.str()));
      }
      if (resource.failure_domains.size() > Limits::max_domains_per_path) {
        return Status(Error(ErrorCode::limit_exceeded,
                            "resource failure-domain membership exceeds the supported bound for " +
                                resource.id.str()));
      }
      ids.push_back(resource.id);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
      return Status(Error(ErrorCode::duplicate_identity, "duplicate resource identity in the capacity snapshot"));
    }
  }

  std::set<ResourceId> resource_ids;
  for (const auto& resource : capacity.resources) resource_ids.insert(resource.id);

  // Reservations snapshot.
  const auto& reservations = snapshot.reservations;
  if (!reservations.id.valid() || !reservations.generation.valid()) {
    return Status(Error(ErrorCode::invalid_generation, "reservation snapshot reference is incomplete"));
  }
  if (!reservations.provenance.valid()) {
    return Status(Error(ErrorCode::missing_provenance, "reservation snapshot provenance is absent"));
  }
  if (!(reservations.fabric_epoch == snapshot.fabric_epoch)) {
    return Status(Error(ErrorCode::conflicting_authority,
                        "reservation snapshot fabric epoch does not match the snapshot epoch"));
  }
  if (reservations.reservations.size() > Limits::max_reservations) {
    return Status(Error(ErrorCode::limit_exceeded, "reservation count exceeds the supported bound"));
  }
  {
    std::vector<ReservationId> ids;
    ids.reserve(reservations.reservations.size());
    for (const auto& reservation : reservations.reservations) {
      if (!reservation.id.valid()) return Status(Error(ErrorCode::invalid_argument, "reservation id is absent"));
      if (!reservation.generation.valid()) {
        return Status(Error(ErrorCode::invalid_generation,
                            "reservation generation is absent for " + reservation.id.str()));
      }
      if (!reservation.provenance.valid()) {
        return Status(Error(ErrorCode::missing_provenance,
                            "reservation provenance is absent for " + reservation.id.str()));
      }
      if (reservation.bandwidth < 0 || reservation.bandwidth > Limits::max_bandwidth) {
        return Status(Error(ErrorCode::numeric_invalid,
                            "reservation bandwidth is out of range for " + reservation.id.str()));
      }
      for (const auto& resource : reservation.resources) {
        if (resource_ids.find(resource) == resource_ids.end()) {
          return Status(Error(ErrorCode::unknown_reference,
                              "reservation " + reservation.id.str() + " references unknown resource " +
                                  resource.str()));
        }
      }
      ids.push_back(reservation.id);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
      return Status(Error(ErrorCode::duplicate_identity, "duplicate reservation identity"));
    }
  }

  // Policy.
  const auto& policy = snapshot.policy;
  if (!policy.id.valid() || !policy.generation.valid()) {
    return Status(Error(ErrorCode::invalid_generation, "policy reference is incomplete"));
  }
  if (!policy.provenance.valid()) {
    return Status(Error(ErrorCode::missing_provenance, "policy provenance is absent"));
  }
  if (policy.max_utilization_permille <= 0 || policy.max_utilization_permille > 1000) {
    return Status(Error(ErrorCode::invalid_argument, "policy max utilization must be within (0, 1000] permille"));
  }
  if (policy.churn_improvement_threshold_permille < 0 ||
      policy.churn_improvement_threshold_permille > 1000) {
    return Status(Error(ErrorCode::invalid_argument, "policy churn improvement threshold is out of range"));
  }
  if (policy.churn_max_moved_bandwidth_permille < 0 ||
      policy.churn_max_moved_bandwidth_permille > 1000) {
    return Status(Error(ErrorCode::invalid_argument, "policy churn moved-bandwidth bound is out of range"));
  }
  if (policy.churn_max_operational_risk_permille < 0 ||
      policy.churn_max_operational_risk_permille > 1000) {
    return Status(Error(ErrorCode::invalid_argument, "policy churn risk bound is out of range"));
  }
  if (policy.max_paths_per_demand > Limits::max_paths_per_demand) {
    return Status(Error(ErrorCode::limit_exceeded, "policy path-count bound exceeds the supported bound"));
  }

  // Objective profile.
  const auto& objective = snapshot.objective;
  if (!objective.id.valid() || !objective.generation.valid()) {
    return Status(Error(ErrorCode::invalid_generation, "objective profile reference is incomplete"));
  }
  if (!objective.provenance.valid()) {
    return Status(Error(ErrorCode::missing_provenance, "objective profile provenance is absent"));
  }
  if (objective.terms.empty()) {
    return Status(Error(ErrorCode::unsupported_objective, "objective profile declares no terms"));
  }
  if (objective.terms.size() > 16) {
    return Status(Error(ErrorCode::limit_exceeded, "objective profile declares too many terms"));
  }
  {
    std::set<std::uint16_t> seen;
    for (const auto& term : objective.terms) {
      const auto ordinal = static_cast<std::uint16_t>(term.term);
      if (ordinal > static_cast<std::uint16_t>(ObjectiveTerm::minimize_path_count)) {
        return Status(Error(ErrorCode::unsupported_objective, "objective profile declares an unknown term"));
      }
      if (term.weight == 0) {
        return Status(Error(ErrorCode::unsupported_objective,
                            std::string("objective term ") + std::string(to_string(term.term)) +
                                " declares a zero weight"));
      }
      if (!seen.insert(ordinal).second) {
        return Status(Error(ErrorCode::duplicate_identity,
                            std::string("objective term ") + std::string(to_string(term.term)) +
                                " is declared twice"));
      }
    }
  }

  // Candidate paths.
  {
    std::vector<PathId> ids;
    ids.reserve(snapshot.paths.size());
    std::map<std::string, std::size_t> paths_per_set;
    for (const auto& path : snapshot.paths) {
      if (!path.id.valid()) return Status(Error(ErrorCode::invalid_argument, "candidate path id is absent"));
      if (!path.generation.valid()) {
        return Status(Error(ErrorCode::invalid_generation,
                            "candidate path generation is absent for " + path.id.str()));
      }
      if (!path.authority.valid()) {
        return Status(Error(ErrorCode::invalid_generation,
                            "path authority binding is incomplete for " + path.id.str()));
      }
      if (!(path.authority.id == path.id)) {
        return Status(Error(ErrorCode::conflicting_authority,
                            "path " + path.id.str() + " binds a path authority identity that is not its own"));
      }
      if (!(path.candidate_set == snapshot.candidate_set)) {
        return Status(Error(ErrorCode::conflicting_authority,
                            "path " + path.id.str() + " belongs to a different candidate set generation"));
      }
      if (!path.provenance.valid()) {
        return Status(Error(ErrorCode::missing_provenance,
                            "candidate path provenance is absent for " + path.id.str()));
      }
      if (path.resources.empty()) {
        return Status(Error(ErrorCode::invalid_argument,
                            "candidate path " + path.id.str() + " declares no resources"));
      }
      if (path.resources.size() > Limits::max_resources_per_path) {
        return Status(Error(ErrorCode::limit_exceeded,
                            "candidate path " + path.id.str() + " exceeds the resource bound"));
      }
      for (const auto& resource : path.resources) {
        if (resource_ids.find(resource) == resource_ids.end()) {
          return Status(Error(ErrorCode::unknown_reference,
                              "candidate path " + path.id.str() + " references unknown resource " +
                                  resource.str()));
        }
      }
      if (path.failure_domains.size() > Limits::max_domains_per_path) {
        return Status(Error(ErrorCode::limit_exceeded,
                            "candidate path " + path.id.str() + " exceeds the failure-domain bound"));
      }
      if (path.cost < 0 || path.cost > Limits::max_cost) {
        return Status(Error(ErrorCode::numeric_invalid,
                            "candidate path cost is out of range for " + path.id.str()));
      }
      if (path.latency_micros.has_value() &&
          (*path.latency_micros < 0 || *path.latency_micros > Limits::max_latency_micros)) {
        return Status(Error(ErrorCode::numeric_invalid,
                            "candidate path latency is out of range for " + path.id.str()));
      }
      if (path.scope == EligibilityScope::tenant && !path.scope_tenant.valid()) {
        return Status(Error(ErrorCode::invalid_argument,
                            "candidate path " + path.id.str() + " has tenant scope without a tenant"));
      }
      if (path.scope == EligibilityScope::service_class && !path.scope_service_class.valid()) {
        return Status(Error(ErrorCode::invalid_argument,
                            "candidate path " + path.id.str() +
                                " has service-class scope without a service class"));
      }
      ++paths_per_set[path.candidate_set.id.str()];
      ids.push_back(path.id);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
      return Status(Error(ErrorCode::duplicate_identity, "duplicate candidate path identity"));
    }
    for (const auto& [key, count] : paths_per_set) {
      if (count > Limits::max_paths_per_demand * Limits::max_demands) {
        return Status(Error(ErrorCode::limit_exceeded,
                            "candidate set " + key + " exceeds the supported bound"));
      }
    }
  }

  // Demands.
  {
    std::vector<DemandId> ids;
    ids.reserve(snapshot.demands.size());
    std::set<DemandId> demand_ids;
    for (const auto& demand : snapshot.demands) demand_ids.insert(demand.id);

    for (const auto& demand : snapshot.demands) {
      if (!demand.id.valid()) return Status(Error(ErrorCode::invalid_argument, "demand id is absent"));
      if (!demand.generation.valid()) {
        return Status(Error(ErrorCode::invalid_generation,
                            "demand generation is absent for " + demand.id.str()));
      }
      if (!demand.provenance.valid()) {
        return Status(Error(ErrorCode::missing_provenance,
                            "demand provenance is absent for " + demand.id.str()));
      }
      if (!(demand.candidate_set == snapshot.candidate_set)) {
        return Status(Error(ErrorCode::conflicting_authority,
                            "demand " + demand.id.str() + " binds a different candidate set generation"));
      }
      if (demand.minimum_bandwidth < 0 || demand.minimum_bandwidth > Limits::max_bandwidth) {
        return Status(Error(ErrorCode::numeric_invalid,
                            "demand minimum bandwidth is out of range for " + demand.id.str()));
      }
      if (demand.desired_bandwidth < 0 || demand.desired_bandwidth > Limits::max_bandwidth) {
        return Status(Error(ErrorCode::numeric_invalid,
                            "demand desired bandwidth is out of range for " + demand.id.str()));
      }
      if (demand.maximum_bandwidth < 0 || demand.maximum_bandwidth > Limits::max_bandwidth) {
        return Status(Error(ErrorCode::numeric_invalid,
                            "demand maximum bandwidth is out of range for " + demand.id.str()));
      }
      if (demand.minimum_bandwidth > demand.desired_bandwidth) {
        return Status(Error(ErrorCode::numeric_invalid,
                            "demand " + demand.id.str() + " has a minimum above its desired bandwidth"));
      }
      if (demand.desired_bandwidth > demand.maximum_bandwidth) {
        return Status(Error(ErrorCode::numeric_invalid,
                            "demand " + demand.id.str() + " has a desired above its maximum bandwidth"));
      }
      if (demand.latency_bound_micros.has_value() &&
          (*demand.latency_bound_micros < 0 || *demand.latency_bound_micros > Limits::max_latency_micros)) {
        return Status(Error(ErrorCode::numeric_invalid,
                            "demand latency bound is out of range for " + demand.id.str()));
      }
      if (demand.path_cost_ceiling.has_value() &&
          (*demand.path_cost_ceiling < 0 || *demand.path_cost_ceiling > Limits::max_cost)) {
        return Status(Error(ErrorCode::numeric_invalid,
                            "demand path cost ceiling is out of range for " + demand.id.str()));
      }
      if (demand.min_distinct_failure_domains > Limits::max_domains_per_path) {
        return Status(Error(ErrorCode::limit_exceeded,
                            "demand failure-domain diversity bound exceeds the supported bound for " +
                                demand.id.str()));
      }
      if (demand.allowed_paths.size() > Limits::max_constraint_refs ||
          demand.forbidden_paths.size() > Limits::max_constraint_refs ||
          demand.affinity.size() > Limits::max_constraint_refs ||
          demand.anti_affinity.size() > Limits::max_constraint_refs) {
        return Status(Error(ErrorCode::limit_exceeded,
                            "demand constraint list exceeds the supported bound for " + demand.id.str()));
      }
      for (const auto& other : demand.affinity) {
        if (other == demand.id) {
          return Status(Error(ErrorCode::cyclic_reference,
                              "demand " + demand.id.str() + " declares affinity with itself"));
        }
        if (demand_ids.find(other) == demand_ids.end()) {
          return Status(Error(ErrorCode::unknown_reference,
                              "demand " + demand.id.str() + " declares affinity with unknown demand " +
                                  other.str()));
        }
      }
      for (const auto& other : demand.anti_affinity) {
        if (other == demand.id) {
          return Status(Error(ErrorCode::cyclic_reference,
                              "demand " + demand.id.str() + " declares anti-affinity with itself"));
        }
        if (demand_ids.find(other) == demand_ids.end()) {
          return Status(Error(ErrorCode::unknown_reference,
                              "demand " + demand.id.str() + " declares anti-affinity with unknown demand " +
                                  other.str()));
        }
      }
      for (const auto& binding : demand.reservation_bindings) {
        bool found = false;
        for (const auto& reservation : reservations.reservations) {
          if (reservation.id == binding) {
            found = true;
            break;
          }
        }
        if (!found) {
          return Status(Error(ErrorCode::unknown_reference,
                              "demand " + demand.id.str() + " binds unknown reservation " + binding.str()));
        }
      }
      for (const auto& label : demand.labels) {
        if (label.first.empty() || label.first.size() > Limits::max_name_length ||
            label.second.size() > Limits::max_text_length) {
          return Status(Error(ErrorCode::invalid_argument,
                              "demand " + demand.id.str() + " carries a malformed label"));
        }
      }
      ids.push_back(demand.id);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
      return Status(Error(ErrorCode::duplicate_identity, "duplicate demand identity"));
    }
  }

  return Status::success();
}

// ---------------------------------------------------------------------------
// Collection digests
// ---------------------------------------------------------------------------

Digest demand_set_digest(const std::vector<Demand>& demands) {
  Writer w;
  w.u32(static_cast<std::uint32_t>(demands.size()));
  for (const auto& demand : demands) {
    w.str(demand.id.str());
    w.u64(demand.generation.value());
    w.str(demand.tenant.str());
    w.str(demand.service_class.str());
    w.i64(demand.minimum_bandwidth);
    w.i64(demand.desired_bandwidth);
    w.i64(demand.maximum_bandwidth);
    w.u8(demand.priority);
    w.boolean(demand.latency_bound_micros.has_value());
    if (demand.latency_bound_micros) w.i64(*demand.latency_bound_micros);
    w.boolean(demand.path_cost_ceiling.has_value());
    if (demand.path_cost_ceiling) w.i64(*demand.path_cost_ceiling);
    w.str(demand.candidate_set.id.str());
    w.u64(demand.candidate_set.generation.value());
    write_ids(w, demand.allowed_paths);
    write_ids(w, demand.forbidden_paths);
    write_ids(w, demand.affinity);
    write_ids(w, demand.anti_affinity);
    write_ids(w, demand.required_failure_domains);
    w.u32(demand.min_distinct_failure_domains);
    write_ids(w, demand.reservation_bindings);
    write_interval(w, demand.effective);
    w.u8(static_cast<std::uint8_t>(demand.preemptibility));
    write_labels(w, demand.labels);
    w.digest(demand.provenance.evidence_digest);
    w.str(demand.provenance.source.str());
    w.u64(demand.provenance.source_generation);
    w.u64(demand.provenance.observed_tick);
  }
  return w.finish_digest("tef.demands.v1");
}

Digest candidate_set_digest(const std::vector<CandidatePath>& paths) {
  Writer w;
  w.u32(static_cast<std::uint32_t>(paths.size()));
  for (const auto& path : paths) {
    w.str(path.id.str());
    w.u64(path.generation.value());
    w.str(path.authority.id.str());
    w.u64(path.authority.generation.value());
    w.str(path.candidate_set.id.str());
    w.u64(path.candidate_set.generation.value());
    write_ids(w, path.resources);
    write_ids(w, path.failure_domains);
    write_strings(w, path.policy_labels);
    w.i64(path.cost);
    w.boolean(path.latency_micros.has_value());
    if (path.latency_micros) w.i64(*path.latency_micros);
    w.u8(static_cast<std::uint8_t>(path.scope));
    w.str(path.scope_tenant.str());
    w.str(path.scope_service_class.str());
    w.digest(path.provenance.evidence_digest);
    w.str(path.provenance.source.str());
    w.u64(path.provenance.source_generation);
    w.u64(path.provenance.observed_tick);
  }
  return w.finish_digest("tef.candidate-set.v1");
}

Digest resource_catalog_digest(const std::vector<FabricResource>& resources) {
  Writer w;
  w.u32(static_cast<std::uint32_t>(resources.size()));
  for (const auto& resource : resources) {
    w.str(resource.id.str());
    w.u64(resource.generation.value());
    w.i64(resource.usable_capacity);
    w.i64(resource.committed_load);
    write_ids(w, resource.failure_domains);
    w.digest(resource.provenance.evidence_digest);
    w.str(resource.provenance.source.str());
    w.u64(resource.provenance.source_generation);
    w.u64(resource.provenance.observed_tick);
  }
  return w.finish_digest("tef.resources.v1");
}

Digest reservation_set_digest(const std::vector<Reservation>& reservations) {
  Writer w;
  w.u32(static_cast<std::uint32_t>(reservations.size()));
  for (const auto& reservation : reservations) {
    w.str(reservation.id.str());
    w.u64(reservation.generation.value());
    write_ids(w, reservation.paths);
    write_ids(w, reservation.resources);
    w.i64(reservation.bandwidth);
    w.u8(static_cast<std::uint8_t>(reservation.preemptibility));
    w.u8(reservation.priority);
    w.str(reservation.owner.str());
    write_interval(w, reservation.effective);
    w.digest(reservation.provenance.evidence_digest);
    w.str(reservation.provenance.source.str());
    w.u64(reservation.provenance.source_generation);
    w.u64(reservation.provenance.observed_tick);
  }
  return w.finish_digest("tef.reservations.v1");
}

Digest policy_digest(const Policy& policy) {
  Writer w;
  w.str(policy.id.str());
  w.u64(policy.generation.value());
  w.boolean(policy.allow_preemption);
  w.boolean(policy.require_minimums);
  w.boolean(policy.allow_degraded_commit);
  w.boolean(policy.require_failure_domain_diversity);
  w.i64(policy.max_utilization_permille);
  w.i64(policy.churn_improvement_threshold_permille);
  w.i64(policy.churn_max_moved_bandwidth_permille);
  w.i64(policy.churn_max_operational_risk_permille);
  w.u32(policy.churn_max_affected_demands);
  w.u32(policy.max_paths_per_demand);
  write_ids(w, policy.allowed_tenants);
  write_ids(w, policy.allowed_service_classes);
  w.digest(policy.provenance.evidence_digest);
  w.str(policy.provenance.source.str());
  w.u64(policy.provenance.source_generation);
  return w.finish_digest("tef.policy.v1");
}

Digest objective_profile_digest(const ObjectiveProfile& profile) {
  Writer w;
  w.str(profile.id.str());
  w.u64(profile.generation.value());
  w.u32(static_cast<std::uint32_t>(profile.terms.size()));
  for (const auto& term : profile.terms) {
    w.u16(static_cast<std::uint16_t>(term.term));
    w.i64(term.weight);
  }
  w.digest(profile.provenance.evidence_digest);
  w.str(profile.provenance.source.str());
  w.u64(profile.provenance.source_generation);
  return w.finish_digest("tef.objective.v1");
}

// ---------------------------------------------------------------------------
// Snapshot encoding
// ---------------------------------------------------------------------------

namespace {

void write_resource(Writer& w, const FabricResource& resource) {
  w.str(resource.id.str());
  w.u64(resource.generation.value());
  w.i64(resource.usable_capacity);
  w.i64(resource.committed_load);
  write_ids(w, resource.failure_domains);
  write_provenance(w, resource.provenance);
}

bool read_resource(Reader& r, FabricResource& resource) {
  std::string id;
  if (!r.str(id, Limits::max_name_length)) return false;
  const auto parsed = ResourceId::parse(id);
  if (!parsed) return false;
  resource.id = *parsed;
  std::uint64_t generation = 0;
  if (!r.u64(generation)) return false;
  const auto gen = ResourceGeneration::parse(generation);
  if (!gen) return false;
  resource.generation = *gen;
  if (!r.i64(resource.usable_capacity)) return false;
  if (!r.i64(resource.committed_load)) return false;
  if (!read_ids(r, resource.failure_domains, Limits::max_domains_per_path)) return false;
  return read_provenance(r, resource.provenance);
}

void write_reservation(Writer& w, const Reservation& reservation) {
  w.str(reservation.id.str());
  w.u64(reservation.generation.value());
  write_ids(w, reservation.paths);
  write_ids(w, reservation.resources);
  w.i64(reservation.bandwidth);
  w.u8(static_cast<std::uint8_t>(reservation.preemptibility));
  w.u8(reservation.priority);
  w.str(reservation.owner.str());
  write_interval(w, reservation.effective);
  write_provenance(w, reservation.provenance);
}

bool read_reservation(Reader& r, Reservation& reservation) {
  std::string id;
  if (!r.str(id, Limits::max_name_length)) return false;
  const auto parsed = ReservationId::parse(id);
  if (!parsed) return false;
  reservation.id = *parsed;
  std::uint64_t generation = 0;
  if (!r.u64(generation)) return false;
  const auto gen = ReservationGeneration::parse(generation);
  if (!gen) return false;
  reservation.generation = *gen;
  if (!read_ids(r, reservation.paths, Limits::max_constraint_refs)) return false;
  if (!read_ids(r, reservation.resources, Limits::max_resources_per_path)) return false;
  if (!r.i64(reservation.bandwidth)) return false;
  std::uint8_t preemptibility = 0;
  if (!r.u8(preemptibility)) return false;
  if (preemptibility > static_cast<std::uint8_t>(Preemptibility::preemptible)) return false;
  reservation.preemptibility = static_cast<Preemptibility>(preemptibility);
  if (!r.u8(reservation.priority)) return false;
  std::string owner;
  if (!r.str(owner, Limits::max_name_length)) return false;
  if (!owner.empty()) {
    const auto owner_id = DemandId::parse(owner);
    if (!owner_id) return false;
    reservation.owner = *owner_id;
  }
  if (!read_interval(r, reservation.effective)) return false;
  return read_provenance(r, reservation.provenance);
}

void write_path(Writer& w, const CandidatePath& path) {
  w.str(path.id.str());
  w.u64(path.generation.value());
  w.str(path.authority.id.str());
  w.u64(path.authority.generation.value());
  w.str(path.candidate_set.id.str());
  w.u64(path.candidate_set.generation.value());
  write_ids(w, path.resources);
  write_ids(w, path.failure_domains);
  write_strings(w, path.policy_labels);
  w.i64(path.cost);
  w.boolean(path.latency_micros.has_value());
  if (path.latency_micros) w.i64(*path.latency_micros);
  w.u8(static_cast<std::uint8_t>(path.scope));
  w.str(path.scope_tenant.str());
  w.str(path.scope_service_class.str());
  write_provenance(w, path.provenance);
}

bool read_path(Reader& r, CandidatePath& path) {
  std::string id;
  if (!r.str(id, Limits::max_name_length)) return false;
  const auto parsed = PathId::parse(id);
  if (!parsed) return false;
  path.id = *parsed;
  std::uint64_t generation = 0;
  if (!r.u64(generation)) return false;
  const auto gen = PathGeneration::parse(generation);
  if (!gen) return false;
  path.generation = *gen;

  std::string authority_id;
  if (!r.str(authority_id, Limits::max_name_length)) return false;
  const auto authority_parsed = PathId::parse(authority_id);
  if (!authority_parsed) return false;
  path.authority.id = *authority_parsed;
  std::uint64_t authority_generation = 0;
  if (!r.u64(authority_generation)) return false;
  const auto authority_gen = PathAuthorityGeneration::parse(authority_generation);
  if (!authority_gen) return false;
  path.authority.generation = *authority_gen;

  std::string set_id;
  if (!r.str(set_id, Limits::max_name_length)) return false;
  const auto set_parsed = CandidateSetId::parse(set_id);
  if (!set_parsed) return false;
  path.candidate_set.id = *set_parsed;
  std::uint64_t set_generation = 0;
  if (!r.u64(set_generation)) return false;
  const auto set_gen = CandidateSetGeneration::parse(set_generation);
  if (!set_gen) return false;
  path.candidate_set.generation = *set_gen;

  if (!read_ids(r, path.resources, Limits::max_resources_per_path)) return false;
  if (!read_ids(r, path.failure_domains, Limits::max_domains_per_path)) return false;
  if (!read_strings(r, path.policy_labels, Limits::max_constraint_refs * 2u)) return false;
  if (!r.i64(path.cost)) return false;
  bool has_latency = false;
  if (!r.boolean(has_latency)) return false;
  if (has_latency) {
    std::int64_t latency = 0;
    if (!r.i64(latency)) return false;
    path.latency_micros = latency;
  }
  std::uint8_t scope = 0;
  if (!r.u8(scope)) return false;
  if (scope > static_cast<std::uint8_t>(EligibilityScope::service_class)) return false;
  path.scope = static_cast<EligibilityScope>(scope);
  std::string tenant;
  if (!r.str(tenant, Limits::max_name_length)) return false;
  if (!tenant.empty()) {
    const auto tenant_id = TenantId::parse(tenant);
    if (!tenant_id) return false;
    path.scope_tenant = *tenant_id;
  }
  std::string service_class;
  if (!r.str(service_class, Limits::max_name_length)) return false;
  if (!service_class.empty()) {
    const auto service_class_id = ServiceClassId::parse(service_class);
    if (!service_class_id) return false;
    path.scope_service_class = *service_class_id;
  }
  return read_provenance(r, path.provenance);
}

void write_demand(Writer& w, const Demand& demand) {
  w.str(demand.id.str());
  w.u64(demand.generation.value());
  write_provenance(w, demand.provenance);
  w.str(demand.tenant.str());
  w.str(demand.service_class.str());
  w.i64(demand.minimum_bandwidth);
  w.i64(demand.desired_bandwidth);
  w.i64(demand.maximum_bandwidth);
  w.u8(demand.priority);
  w.boolean(demand.latency_bound_micros.has_value());
  if (demand.latency_bound_micros) w.i64(*demand.latency_bound_micros);
  w.boolean(demand.path_cost_ceiling.has_value());
  if (demand.path_cost_ceiling) w.i64(*demand.path_cost_ceiling);
  w.str(demand.candidate_set.id.str());
  w.u64(demand.candidate_set.generation.value());
  write_ids(w, demand.allowed_paths);
  write_ids(w, demand.forbidden_paths);
  write_ids(w, demand.affinity);
  write_ids(w, demand.anti_affinity);
  write_ids(w, demand.required_failure_domains);
  w.u32(demand.min_distinct_failure_domains);
  write_ids(w, demand.reservation_bindings);
  write_interval(w, demand.effective);
  w.u8(static_cast<std::uint8_t>(demand.preemptibility));
  write_labels(w, demand.labels);
}

bool read_demand(Reader& r, Demand& demand) {
  std::string id;
  if (!r.str(id, Limits::max_name_length)) return false;
  const auto parsed = DemandId::parse(id);
  if (!parsed) return false;
  demand.id = *parsed;
  std::uint64_t generation = 0;
  if (!r.u64(generation)) return false;
  const auto gen = DemandGeneration::parse(generation);
  if (!gen) return false;
  demand.generation = *gen;
  if (!read_provenance(r, demand.provenance)) return false;
  std::string tenant;
  if (!r.str(tenant, Limits::max_name_length)) return false;
  if (!tenant.empty()) {
    const auto tenant_id = TenantId::parse(tenant);
    if (!tenant_id) return false;
    demand.tenant = *tenant_id;
  }
  std::string service_class;
  if (!r.str(service_class, Limits::max_name_length)) return false;
  if (!service_class.empty()) {
    const auto service_class_id = ServiceClassId::parse(service_class);
    if (!service_class_id) return false;
    demand.service_class = *service_class_id;
  }
  if (!r.i64(demand.minimum_bandwidth)) return false;
  if (!r.i64(demand.desired_bandwidth)) return false;
  if (!r.i64(demand.maximum_bandwidth)) return false;
  if (!r.u8(demand.priority)) return false;
  bool has_latency = false;
  if (!r.boolean(has_latency)) return false;
  if (has_latency) {
    std::int64_t latency = 0;
    if (!r.i64(latency)) return false;
    demand.latency_bound_micros = latency;
  }
  bool has_cost = false;
  if (!r.boolean(has_cost)) return false;
  if (has_cost) {
    std::int64_t cost = 0;
    if (!r.i64(cost)) return false;
    demand.path_cost_ceiling = cost;
  }
  std::string set_id;
  if (!r.str(set_id, Limits::max_name_length)) return false;
  const auto set_parsed = CandidateSetId::parse(set_id);
  if (!set_parsed) return false;
  demand.candidate_set.id = *set_parsed;
  std::uint64_t set_generation = 0;
  if (!r.u64(set_generation)) return false;
  const auto set_gen = CandidateSetGeneration::parse(set_generation);
  if (!set_gen) return false;
  demand.candidate_set.generation = *set_gen;

  if (!read_ids(r, demand.allowed_paths, Limits::max_constraint_refs)) return false;
  if (!read_ids(r, demand.forbidden_paths, Limits::max_constraint_refs)) return false;
  if (!read_ids(r, demand.affinity, Limits::max_constraint_refs)) return false;
  if (!read_ids(r, demand.anti_affinity, Limits::max_constraint_refs)) return false;
  if (!read_ids(r, demand.required_failure_domains, Limits::max_domains_per_path)) return false;
  if (!r.u32(demand.min_distinct_failure_domains)) return false;
  if (!read_ids(r, demand.reservation_bindings, Limits::max_constraint_refs)) return false;
  if (!read_interval(r, demand.effective)) return false;
  std::uint8_t preemptibility = 0;
  if (!r.u8(preemptibility)) return false;
  if (preemptibility > static_cast<std::uint8_t>(Preemptibility::preemptible)) return false;
  demand.preemptibility = static_cast<Preemptibility>(preemptibility);
  return read_labels(r, demand.labels);
}

template <class RefT, class IdT, class GenT>
void write_ref(Writer& w, const RefT& ref) {
  w.str(ref.id.str());
  w.u64(ref.generation.value());
}

template <class RefT, class IdT, class GenT>
bool read_ref(Reader& r, RefT& ref) {
  std::string id;
  if (!r.str(id, Limits::max_name_length)) return false;
  const auto parsed = IdT::parse(id);
  if (!parsed) return false;
  ref.id = *parsed;
  std::uint64_t generation = 0;
  if (!r.u64(generation)) return false;
  const auto gen = GenT::parse(generation);
  if (!gen) return false;
  ref.generation = *gen;
  return true;
}

void write_policy(Writer& w, const Policy& policy) {
  w.str(policy.id.str());
  w.u64(policy.generation.value());
  write_provenance(w, policy.provenance);
  w.boolean(policy.allow_preemption);
  w.boolean(policy.require_minimums);
  w.boolean(policy.allow_degraded_commit);
  w.boolean(policy.require_failure_domain_diversity);
  w.i64(policy.max_utilization_permille);
  w.i64(policy.churn_improvement_threshold_permille);
  w.i64(policy.churn_max_moved_bandwidth_permille);
  w.i64(policy.churn_max_operational_risk_permille);
  w.u32(policy.churn_max_affected_demands);
  w.u32(policy.max_paths_per_demand);
  write_ids(w, policy.allowed_tenants);
  write_ids(w, policy.allowed_service_classes);
}

bool read_policy(Reader& r, Policy& policy) {
  std::string id;
  if (!r.str(id, Limits::max_name_length)) return false;
  const auto parsed = PolicyId::parse(id);
  if (!parsed) return false;
  policy.id = *parsed;
  std::uint64_t generation = 0;
  if (!r.u64(generation)) return false;
  const auto gen = PolicyGeneration::parse(generation);
  if (!gen) return false;
  policy.generation = *gen;
  if (!read_provenance(r, policy.provenance)) return false;
  if (!r.boolean(policy.allow_preemption)) return false;
  if (!r.boolean(policy.require_minimums)) return false;
  if (!r.boolean(policy.allow_degraded_commit)) return false;
  if (!r.boolean(policy.require_failure_domain_diversity)) return false;
  if (!r.i64(policy.max_utilization_permille)) return false;
  if (!r.i64(policy.churn_improvement_threshold_permille)) return false;
  if (!r.i64(policy.churn_max_moved_bandwidth_permille)) return false;
  if (!r.i64(policy.churn_max_operational_risk_permille)) return false;
  if (!r.u32(policy.churn_max_affected_demands)) return false;
  if (!r.u32(policy.max_paths_per_demand)) return false;
  if (!read_ids(r, policy.allowed_tenants, Limits::max_constraint_refs * 4u)) return false;
  if (!read_ids(r, policy.allowed_service_classes, Limits::max_constraint_refs * 4u)) return false;
  return true;
}

void write_objective(Writer& w, const ObjectiveProfile& profile) {
  w.str(profile.id.str());
  w.u64(profile.generation.value());
  write_provenance(w, profile.provenance);
  w.u32(static_cast<std::uint32_t>(profile.terms.size()));
  for (const auto& term : profile.terms) {
    w.u16(static_cast<std::uint16_t>(term.term));
    w.i64(term.weight);
  }
}

bool read_objective(Reader& r, ObjectiveProfile& profile) {
  std::string id;
  if (!r.str(id, Limits::max_name_length)) return false;
  const auto parsed = ObjectiveProfileId::parse(id);
  if (!parsed) return false;
  profile.id = *parsed;
  std::uint64_t generation = 0;
  if (!r.u64(generation)) return false;
  const auto gen = ObjectiveProfileGeneration::parse(generation);
  if (!gen) return false;
  profile.generation = *gen;
  if (!read_provenance(r, profile.provenance)) return false;
  std::uint32_t count = 0;
  if (!r.u32(count)) return false;
  if (count > 16) {
    r.fail();
    return false;
  }
  profile.terms.clear();
  profile.terms.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint16_t ordinal = 0;
    std::int64_t weight = 0;
    if (!r.u16(ordinal)) return false;
    if (!r.i64(weight)) return false;
    if (ordinal > static_cast<std::uint16_t>(ObjectiveTerm::minimize_path_count)) return false;
    ObjectiveWeight entry;
    entry.term = static_cast<ObjectiveTerm>(ordinal);
    entry.weight = weight;
    profile.terms.push_back(entry);
  }
  return true;
}

}  // namespace

std::vector<std::byte> encode_snapshot(const FabricSnapshot& snapshot) {
  Writer w;
  w.u16(static_cast<std::uint16_t>(kDurableFormatVersion));
  w.u64(snapshot.fabric_epoch.value());
  w.u64(snapshot.topology_generation.value());
  w.u64(snapshot.link_state_generation.value());
  w.u64(snapshot.path_authority_generation.value());
  w.u64(snapshot.failure_domain_generation.value());
  write_provenance(w, snapshot.provenance);

  w.str(snapshot.capacity.id.str());
  w.u64(snapshot.capacity.generation.value());
  w.u64(snapshot.capacity.fabric_epoch.value());
  write_provenance(w, snapshot.capacity.provenance);
  w.u32(static_cast<std::uint32_t>(snapshot.capacity.resources.size()));
  for (const auto& resource : snapshot.capacity.resources) write_resource(w, resource);

  w.str(snapshot.reservations.id.str());
  w.u64(snapshot.reservations.generation.value());
  w.u64(snapshot.reservations.fabric_epoch.value());
  write_provenance(w, snapshot.reservations.provenance);
  w.u32(static_cast<std::uint32_t>(snapshot.reservations.reservations.size()));
  for (const auto& reservation : snapshot.reservations.reservations) write_reservation(w, reservation);

  write_policy(w, snapshot.policy);
  write_objective(w, snapshot.objective);

  write_ref<CandidateSetRef, CandidateSetId, CandidateSetGeneration>(w, snapshot.candidate_set);

  w.u32(static_cast<std::uint32_t>(snapshot.demands.size()));
  for (const auto& demand : snapshot.demands) write_demand(w, demand);

  w.u32(static_cast<std::uint32_t>(snapshot.paths.size()));
  for (const auto& path : snapshot.paths) write_path(w, path);

  w.u64(snapshot.evaluation_tick);
  return w.bytes();
}

Result<FabricSnapshot> decode_snapshot(std::span<const std::byte> payload) {
  Reader r(payload);
  FabricSnapshot snapshot;

  std::uint16_t version = 0;
  if (!r.u16(version)) return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "snapshot version is truncated");
  if (version != kDurableFormatVersion) {
    return fail_as<FabricSnapshot>(ErrorCode::unsupported_version, "unsupported snapshot format version");
  }

  std::uint64_t value = 0;
  if (!r.u64(value)) return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "fabric epoch is truncated");
  const auto epoch = FabricEpoch::parse(value);
  if (!epoch) return fail_as<FabricSnapshot>(ErrorCode::invalid_generation, "fabric epoch is zero");
  snapshot.fabric_epoch = *epoch;
  if (!r.u64(value)) return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "topology generation is truncated");
  const auto topology = TopologyGeneration::parse(value);
  if (!topology) return fail_as<FabricSnapshot>(ErrorCode::invalid_generation, "topology generation is zero");
  snapshot.topology_generation = *topology;
  if (!r.u64(value)) return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "link-state generation is truncated");
  const auto link_state = LinkStateGeneration::parse(value);
  if (!link_state) return fail_as<FabricSnapshot>(ErrorCode::invalid_generation, "link-state generation is zero");
  snapshot.link_state_generation = *link_state;
  if (!r.u64(value)) return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "path authority generation is truncated");
  const auto path_authority = PathAuthorityGeneration::parse(value);
  if (!path_authority) return fail_as<FabricSnapshot>(ErrorCode::invalid_generation, "path authority generation is zero");
  snapshot.path_authority_generation = *path_authority;
  if (!r.u64(value)) return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "failure-domain generation is truncated");
  const auto failure_domain = FailureDomainGeneration::parse(value);
  if (!failure_domain) return fail_as<FabricSnapshot>(ErrorCode::invalid_generation, "failure-domain generation is zero");
  snapshot.failure_domain_generation = *failure_domain;
  if (!read_provenance(r, snapshot.provenance)) {
    return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "snapshot provenance is malformed");
  }

  std::string id;
  if (!r.str(id, Limits::max_name_length)) return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "capacity snapshot id is malformed");
  const auto capacity_id = CapacitySnapshotId::parse(id);
  if (!capacity_id) return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "capacity snapshot id is invalid");
  snapshot.capacity.id = *capacity_id;
  if (!r.u64(value)) return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "capacity snapshot generation is truncated");
  const auto capacity_gen = CapacitySnapshotGeneration::parse(value);
  if (!capacity_gen) return fail_as<FabricSnapshot>(ErrorCode::invalid_generation, "capacity snapshot generation is zero");
  snapshot.capacity.generation = *capacity_gen;
  if (!r.u64(value)) return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "capacity snapshot epoch is truncated");
  const auto capacity_epoch = FabricEpoch::parse(value);
  if (!capacity_epoch) return fail_as<FabricSnapshot>(ErrorCode::invalid_generation, "capacity snapshot epoch is zero");
  snapshot.capacity.fabric_epoch = *capacity_epoch;
  if (!read_provenance(r, snapshot.capacity.provenance)) {
    return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "capacity snapshot provenance is malformed");
  }
  std::uint32_t count = 0;
  if (!r.u32(count)) return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "resource count is truncated");
  if (count > Limits::max_resources) return fail_as<FabricSnapshot>(ErrorCode::oversized_input, "resource count exceeds the supported bound");
  snapshot.capacity.resources.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!read_resource(r, snapshot.capacity.resources[i])) {
      return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "resource entry is malformed");
    }
  }

  if (!r.str(id, Limits::max_name_length)) return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "reservation snapshot id is malformed");
  const auto reservation_id = ReservationSnapshotId::parse(id);
  if (!reservation_id) return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "reservation snapshot id is invalid");
  snapshot.reservations.id = *reservation_id;
  if (!r.u64(value)) return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "reservation snapshot generation is truncated");
  const auto reservation_gen = ReservationSnapshotGeneration::parse(value);
  if (!reservation_gen) return fail_as<FabricSnapshot>(ErrorCode::invalid_generation, "reservation snapshot generation is zero");
  snapshot.reservations.generation = *reservation_gen;
  if (!r.u64(value)) return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "reservation snapshot epoch is truncated");
  const auto reservation_epoch = FabricEpoch::parse(value);
  if (!reservation_epoch) return fail_as<FabricSnapshot>(ErrorCode::invalid_generation, "reservation snapshot epoch is zero");
  snapshot.reservations.fabric_epoch = *reservation_epoch;
  if (!read_provenance(r, snapshot.reservations.provenance)) {
    return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "reservation snapshot provenance is malformed");
  }
  if (!r.u32(count)) return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "reservation count is truncated");
  if (count > Limits::max_reservations) return fail_as<FabricSnapshot>(ErrorCode::oversized_input, "reservation count exceeds the supported bound");
  snapshot.reservations.reservations.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!read_reservation(r, snapshot.reservations.reservations[i])) {
      return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "reservation entry is malformed");
    }
  }

  if (!read_policy(r, snapshot.policy)) {
    return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "policy is malformed");
  }
  if (!read_objective(r, snapshot.objective)) {
    return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "objective profile is malformed");
  }
  if (!read_ref<CandidateSetRef, CandidateSetId, CandidateSetGeneration>(r, snapshot.candidate_set)) {
    return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "candidate set reference is malformed");
  }

  if (!r.u32(count)) return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "demand count is truncated");
  if (count > Limits::max_demands) return fail_as<FabricSnapshot>(ErrorCode::oversized_input, "demand count exceeds the supported bound");
  snapshot.demands.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!read_demand(r, snapshot.demands[i])) {
      return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "demand entry is malformed");
    }
  }

  if (!r.u32(count)) return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "candidate path count is truncated");
  if (count > Limits::max_total_candidate_paths) return fail_as<FabricSnapshot>(ErrorCode::oversized_input, "candidate path count exceeds the supported bound");
  snapshot.paths.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!read_path(r, snapshot.paths[i])) {
      return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "candidate path entry is malformed");
    }
  }

  if (!r.u64(snapshot.evaluation_tick)) {
    return fail_as<FabricSnapshot>(ErrorCode::truncated_input, "evaluation tick is truncated");
  }
  if (r.failed()) return fail_as<FabricSnapshot>(ErrorCode::malformed_input, "snapshot payload is malformed");
  if (!r.at_end()) return fail_as<FabricSnapshot>(ErrorCode::trailing_input, "snapshot payload has trailing bytes");

  const Status structural = validate_structure(snapshot);
  if (!structural.ok()) {
    return Result<FabricSnapshot>(structural.error());
  }
  canonicalize(snapshot);
  return snapshot;
}

}  // namespace tef
