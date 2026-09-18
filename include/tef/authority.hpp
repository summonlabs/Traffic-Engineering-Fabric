// Traffic Engineering Fabric - authority binding and staleness.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "tef/digest.hpp"
#include "tef/model.hpp"

namespace tef {

// Every plan is bound to the exact generations it was computed from. A plan
// without a complete, valid authority vector cannot exist.
struct AuthorityVector {
  FabricEpoch fabric_epoch;
  TopologyGeneration topology_generation;
  LinkStateGeneration link_state_generation;
  PathAuthorityGeneration path_authority_generation;
  FailureDomainGeneration failure_domain_generation;

  CapacitySnapshotRef capacity;
  ReservationSnapshotRef reservations;
  PolicyRef policy;
  ObjectiveProfileRef objective;
  CandidateSetRef candidate_set;

  Digest demand_set_digest;
  Digest candidate_set_digest;
  Digest resource_catalog_digest;
  Digest reservation_set_digest;
  Digest policy_digest;
  Digest objective_digest;

  bool valid() const noexcept;
  Digest digest() const noexcept;

  friend bool operator==(const AuthorityVector&, const AuthorityVector&) noexcept = default;
};

AuthorityVector authority_of(const FabricSnapshot& snapshot);

enum class AuthorityField : std::uint16_t {
  fabric_epoch = 0,
  topology_generation = 1,
  link_state_generation = 2,
  path_authority_generation = 3,
  failure_domain_generation = 4,
  capacity_snapshot = 5,
  capacity_snapshot_generation = 6,
  reservation_snapshot = 7,
  reservation_snapshot_generation = 8,
  policy = 9,
  policy_generation = 10,
  objective_profile = 11,
  objective_profile_generation = 12,
  candidate_set = 13,
  candidate_set_generation = 14,
  demand_set = 15,
  candidate_set_content = 16,
  resource_catalog = 17,
  reservation_set = 18,
  policy_content = 19,
  objective_content = 20,
};

std::string_view to_string(AuthorityField field) noexcept;

// Comparison of a plan's bound authority against the live authority.
//
//   advanced    - the live input is strictly newer; the plan is STALE.
//   regressed   - the live input is strictly older; the inputs are contradictory.
//   conflicting - same generation but different content digest.
struct AuthorityDelta {
  std::vector<AuthorityField> advanced;
  std::vector<AuthorityField> regressed;
  std::vector<AuthorityField> conflicting;

  bool identical() const noexcept {
    return advanced.empty() && regressed.empty() && conflicting.empty();
  }
  bool stale() const noexcept { return !advanced.empty(); }
  bool contradictory() const noexcept { return !regressed.empty() || !conflicting.empty(); }

  std::string describe() const;
};

AuthorityDelta compare_authority(const AuthorityVector& bound, const AuthorityVector& live);

}  // namespace tef
