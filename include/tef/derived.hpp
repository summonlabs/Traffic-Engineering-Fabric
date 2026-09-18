// Traffic Engineering Fabric - derived planning inputs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Everything here is *derived* from the authoritative snapshot by deterministic
// rules. Nothing in this file invents topology, path legality, or capacity: all
// of it is accounting over inputs owned by adjacent systems.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tef/analysis.hpp"
#include "tef/diagnostic.hpp"
#include "tef/feasibility.hpp"
#include "tef/model.hpp"

namespace tef {

struct ResourceResidual {
  ResourceId resource;
  ResourceGeneration generation;
  std::int64_t usable_capacity = 0;
  std::int64_t committed_load = 0;
  std::int64_t reserved = 0;   // obligations this plan may not displace
  std::int64_t available = 0;  // max(0, usable - committed - reserved)

  friend bool operator==(const ResourceResidual&, const ResourceResidual&) noexcept = default;
};

struct ResidualSet {
  std::vector<ResourceResidual> resources;  // canonical order by resource id

  const ResourceResidual* find(const ResourceId& id) const noexcept;
  std::int64_t total_available() const noexcept;
};

// How each authoritative reservation was treated. Reservations covered by a
// demand in this snapshot are charged to that demand (never double charged to
// the resource residual).
struct ReservationAccounting {
  ReservationId reservation;
  ReservationGeneration generation;
  DemandId charged_to;             // invalid when charged to the resource residual
  std::int64_t bandwidth = 0;
  bool displaceable = false;       // policy permits this runtime to displace it
  bool active = true;              // effective interval covers the evaluation tick
  std::string disposition;

  friend bool operator==(const ReservationAccounting&, const ReservationAccounting&) noexcept = default;
  friend std::strong_ordering operator<=>(const ReservationAccounting& a,
                                          const ReservationAccounting& b) noexcept {
    return a.reservation <=> b.reservation;
  }
};

struct ExcludedPath {
  PathId path;
  ConstraintKind kind = ConstraintKind::path_eligibility;
  std::string reason;

  friend bool operator==(const ExcludedPath&, const ExcludedPath&) noexcept = default;
  friend std::strong_ordering operator<=>(const ExcludedPath& a, const ExcludedPath& b) noexcept {
    return a.path <=> b.path;
  }
};

struct DemandDerivation {
  DemandId demand;
  DemandGeneration generation;
  std::int64_t minimum = 0;      // authoritative minimum
  std::int64_t floor = 0;        // max(minimum, reservation obligation)
  std::int64_t desired = 0;
  std::int64_t maximum = 0;
  std::int64_t reservation_obligation = 0;
  std::uint8_t priority = 0;
  TenantId tenant;
  ServiceClassId service_class;
  bool active = true;            // effective interval covers the evaluation tick
  std::vector<PathId> eligible_paths;   // canonical cost order (cost, latency, id)
  std::vector<ExcludedPath> excluded_paths;
  std::vector<BindingConstraint> notes;  // structural / policy constraints found

  const CandidatePath* path(const std::vector<CandidatePath>& catalog, const PathId& id) const noexcept;

  friend bool operator==(const DemandDerivation&, const DemandDerivation&) noexcept = default;
};

struct Derivation {
  std::vector<DemandDerivation> demands;   // canonical order by demand id
  ResidualSet residuals;
  std::vector<ReservationAccounting> reservations;
  std::vector<BindingConstraint> constraints;   // snapshot-level hard problems
  std::vector<std::string> policy_exclusions;
  bool any_demand_active = false;
};

// Deterministic derivation. Returns an error only for structural reasons that
// make derivation impossible; policy/feasibility problems are reported inside
// the Derivation so that they can be explained.
Result<Derivation> derive(const FabricSnapshot& snapshot);

}  // namespace tef
