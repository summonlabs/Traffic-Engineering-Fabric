// Traffic Engineering Fabric - authority binding and staleness.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "tef/authority.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "tef/binary.hpp"

namespace tef {
namespace {

template <class GenT>
bool advanced(const GenT& live, const GenT& bound) {
  return live.valid() && bound.valid() && bound.value() < live.value();
}

template <class GenT>
bool regressed(const GenT& live, const GenT& bound) {
  return live.valid() && bound.valid() && live.value() < bound.value();
}

template <class IdT, class GenT>
void compare_ref(const Ref<IdT, GenT>& live, const Ref<IdT, GenT>& bound, AuthorityField id_field,
                 AuthorityField gen_field, AuthorityDelta& delta) {
  if (!bound.valid()) return;
  if (!live.valid()) {
    delta.conflicting.push_back(id_field);
    return;
  }
  if (!(live.id == bound.id)) {
    delta.conflicting.push_back(id_field);
  }
  if (advanced(live.generation, bound.generation)) {
    delta.advanced.push_back(gen_field);
  } else if (regressed(live.generation, bound.generation)) {
    delta.regressed.push_back(gen_field);
  }
}

void compare_digest(const Digest& live, const Digest& bound, AuthorityField field,
                    AuthorityDelta& delta) {
  if (bound.is_zero() && live.is_zero()) return;
  if (!(live == bound)) delta.conflicting.push_back(field);
}

}  // namespace

bool AuthorityVector::valid() const noexcept {
  return fabric_epoch.valid() && topology_generation.valid() && link_state_generation.valid() &&
         path_authority_generation.valid() && failure_domain_generation.valid() && capacity.valid() &&
         reservations.valid() && policy.valid() && objective.valid() && candidate_set.valid();
}

Digest AuthorityVector::digest() const noexcept {
  Writer w;
  w.u64(fabric_epoch.value());
  w.u64(topology_generation.value());
  w.u64(link_state_generation.value());
  w.u64(path_authority_generation.value());
  w.u64(failure_domain_generation.value());
  w.str(capacity.id.str());
  w.u64(capacity.generation.value());
  w.str(reservations.id.str());
  w.u64(reservations.generation.value());
  w.str(policy.id.str());
  w.u64(policy.generation.value());
  w.str(objective.id.str());
  w.u64(objective.generation.value());
  w.str(candidate_set.id.str());
  w.u64(candidate_set.generation.value());
  w.digest(demand_set_digest);
  w.digest(candidate_set_digest);
  w.digest(resource_catalog_digest);
  w.digest(reservation_set_digest);
  w.digest(policy_digest);
  w.digest(objective_digest);
  return w.finish_digest("tef.authority.v1");
}

AuthorityVector authority_of(const FabricSnapshot& snapshot) {
  AuthorityVector authority;
  authority.fabric_epoch = snapshot.fabric_epoch;
  authority.topology_generation = snapshot.topology_generation;
  authority.link_state_generation = snapshot.link_state_generation;
  authority.path_authority_generation = snapshot.path_authority_generation;
  authority.failure_domain_generation = snapshot.failure_domain_generation;
  authority.capacity = CapacitySnapshotRef{snapshot.capacity.id, snapshot.capacity.generation};
  authority.reservations =
      ReservationSnapshotRef{snapshot.reservations.id, snapshot.reservations.generation};
  authority.policy = PolicyRef{snapshot.policy.id, snapshot.policy.generation};
  authority.objective = ObjectiveProfileRef{snapshot.objective.id, snapshot.objective.generation};
  authority.candidate_set = snapshot.candidate_set;
  authority.demand_set_digest = demand_set_digest(snapshot.demands);
  authority.candidate_set_digest = candidate_set_digest(snapshot.paths);
  authority.resource_catalog_digest = resource_catalog_digest(snapshot.capacity.resources);
  authority.reservation_set_digest = reservation_set_digest(snapshot.reservations.reservations);
  authority.policy_digest = policy_digest(snapshot.policy);
  authority.objective_digest = objective_profile_digest(snapshot.objective);
  return authority;
}

std::string_view to_string(AuthorityField field) noexcept {
  switch (field) {
    case AuthorityField::fabric_epoch: return "fabric_epoch";
    case AuthorityField::topology_generation: return "topology_generation";
    case AuthorityField::link_state_generation: return "link_state_generation";
    case AuthorityField::path_authority_generation: return "path_authority_generation";
    case AuthorityField::failure_domain_generation: return "failure_domain_generation";
    case AuthorityField::capacity_snapshot: return "capacity_snapshot_id";
    case AuthorityField::capacity_snapshot_generation: return "capacity_snapshot_generation";
    case AuthorityField::reservation_snapshot: return "reservation_snapshot_id";
    case AuthorityField::reservation_snapshot_generation: return "reservation_snapshot_generation";
    case AuthorityField::policy: return "policy_id";
    case AuthorityField::policy_generation: return "policy_generation";
    case AuthorityField::objective_profile: return "objective_profile_id";
    case AuthorityField::objective_profile_generation: return "objective_profile_generation";
    case AuthorityField::candidate_set: return "candidate_set_id";
    case AuthorityField::candidate_set_generation: return "candidate_set_generation";
    case AuthorityField::demand_set: return "demand_set_content";
    case AuthorityField::candidate_set_content: return "candidate_set_content";
    case AuthorityField::resource_catalog: return "resource_catalog_content";
    case AuthorityField::reservation_set: return "reservation_set_content";
    case AuthorityField::policy_content: return "policy_content";
    case AuthorityField::objective_content: return "objective_content";
  }
  return "unknown";
}

AuthorityDelta compare_authority(const AuthorityVector& bound, const AuthorityVector& live) {
  AuthorityDelta delta;

  if (advanced(live.fabric_epoch, bound.fabric_epoch)) {
    delta.advanced.push_back(AuthorityField::fabric_epoch);
  } else if (regressed(live.fabric_epoch, bound.fabric_epoch)) {
    delta.regressed.push_back(AuthorityField::fabric_epoch);
  }
  if (advanced(live.topology_generation, bound.topology_generation)) {
    delta.advanced.push_back(AuthorityField::topology_generation);
  } else if (regressed(live.topology_generation, bound.topology_generation)) {
    delta.regressed.push_back(AuthorityField::topology_generation);
  }
  if (advanced(live.link_state_generation, bound.link_state_generation)) {
    delta.advanced.push_back(AuthorityField::link_state_generation);
  } else if (regressed(live.link_state_generation, bound.link_state_generation)) {
    delta.regressed.push_back(AuthorityField::link_state_generation);
  }
  if (advanced(live.path_authority_generation, bound.path_authority_generation)) {
    delta.advanced.push_back(AuthorityField::path_authority_generation);
  } else if (regressed(live.path_authority_generation, bound.path_authority_generation)) {
    delta.regressed.push_back(AuthorityField::path_authority_generation);
  }
  if (advanced(live.failure_domain_generation, bound.failure_domain_generation)) {
    delta.advanced.push_back(AuthorityField::failure_domain_generation);
  } else if (regressed(live.failure_domain_generation, bound.failure_domain_generation)) {
    delta.regressed.push_back(AuthorityField::failure_domain_generation);
  }

  compare_ref(live.capacity, bound.capacity, AuthorityField::capacity_snapshot,
              AuthorityField::capacity_snapshot_generation, delta);
  compare_ref(live.reservations, bound.reservations, AuthorityField::reservation_snapshot,
              AuthorityField::reservation_snapshot_generation, delta);
  compare_ref(live.policy, bound.policy, AuthorityField::policy, AuthorityField::policy_generation,
              delta);
  compare_ref(live.objective, bound.objective, AuthorityField::objective_profile,
              AuthorityField::objective_profile_generation, delta);
  compare_ref(live.candidate_set, bound.candidate_set, AuthorityField::candidate_set,
              AuthorityField::candidate_set_generation, delta);

  compare_digest(live.demand_set_digest, bound.demand_set_digest, AuthorityField::demand_set, delta);
  compare_digest(live.candidate_set_digest, bound.candidate_set_digest,
                 AuthorityField::candidate_set_content, delta);
  compare_digest(live.resource_catalog_digest, bound.resource_catalog_digest,
                 AuthorityField::resource_catalog, delta);
  compare_digest(live.reservation_set_digest, bound.reservation_set_digest,
                 AuthorityField::reservation_set, delta);
  compare_digest(live.policy_digest, bound.policy_digest, AuthorityField::policy_content, delta);
  compare_digest(live.objective_digest, bound.objective_digest, AuthorityField::objective_content,
                 delta);

  const auto normalize = [](std::vector<AuthorityField>& fields) {
    std::sort(fields.begin(), fields.end());
    fields.erase(std::unique(fields.begin(), fields.end()), fields.end());
  };
  normalize(delta.advanced);
  normalize(delta.regressed);
  normalize(delta.conflicting);
  return delta;
}

std::string AuthorityDelta::describe() const {
  std::string out;
  const auto append = [&out](std::string_view label, const std::vector<AuthorityField>& fields) {
    if (fields.empty()) return;
    if (!out.empty()) out += "; ";
    out += label;
    out += "=[";
    for (std::size_t i = 0; i < fields.size(); ++i) {
      if (i != 0) out += ",";
      out += to_string(fields[i]);
    }
    out += "]";
  };
  append("advanced", advanced);
  append("regressed", regressed);
  append("conflicting", conflicting);
  if (out.empty()) out = "identical";
  return out;
}

}  // namespace tef
