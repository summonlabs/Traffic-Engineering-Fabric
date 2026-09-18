// Traffic Engineering Fabric - allocation semantics.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tef/analysis.hpp"
#include "tef/derived.hpp"
#include "tef/diagnostic.hpp"
#include "tef/digest.hpp"
#include "tef/feasibility.hpp"
#include "tef/model.hpp"

namespace tef {

// Bandwidth is carried in fabric bandwidth units (FBU); one unit is one kilobit
// per second. All authoritative arithmetic is integer.

struct ResourceUtilization {
  ResourceId resource;
  ResourceGeneration generation;
  std::int64_t usable_capacity = 0;
  std::int64_t committed_load = 0;   // load already committed by other owners
  std::int64_t reserved = 0;         // obligations carried by reservations
  std::int64_t available = 0;        // max(0, usable - committed - reserved)
  std::int64_t allocated = 0;        // allocated by this plan
  std::int64_t headroom = 0;         // available - allocated
  std::int64_t utilization_permille = 0;  // (committed + reserved + allocated) / usable
  bool saturated = false;

  friend bool operator==(const ResourceUtilization&, const ResourceUtilization&) noexcept = default;
};

// One (demand, path) share. 'granted' is authoritative intent. 'observed
// applied' is only populated when an external observer reports what is actually
// installed; it is never inferred from granted bandwidth.
struct PathShare {
  PathId path;
  PathGeneration generation;

  std::int64_t minimum = 0;    // contribution toward the demand's hard floor
  std::int64_t desired = 0;    // contribution toward the demand's soft target
  std::int64_t granted = 0;    // authoritative intended rate on this path
  std::int64_t reserved = 0;   // portion of granted that carries reservation obligations
  std::int64_t delta_from_incumbent = 0;  // signed change versus the incumbent
  bool newly_used = false;

  friend bool operator==(const PathShare&, const PathShare&) noexcept = default;
  friend std::strong_ordering operator<=>(const PathShare& a, const PathShare& b) noexcept {
    return a.path <=> b.path;
  }
};

struct DemandAllocation {
  DemandId demand;
  DemandGeneration generation;

  std::int64_t minimum = 0;
  std::int64_t desired = 0;
  std::int64_t maximum = 0;
  std::int64_t granted = 0;    // sum of path shares
  std::int64_t reserved = 0;   // sum of reservation-backed shares
  std::int64_t effective = 0;  // granted after policy qualification (== granted unless degraded)
  std::int64_t observed_applied = 0;  // externally reported installed rate; 0 == unknown
  bool applied_known = false;
  std::int64_t shortfall_against_minimum = 0;
  std::int64_t shortfall_against_desired = 0;

  std::vector<PathShare> shares;  // canonical order by path id

  friend bool operator==(const DemandAllocation&, const DemandAllocation&) noexcept = default;
};

struct Allocation {
  std::vector<DemandAllocation> demands;         // canonical order by demand id
  std::vector<ResourceUtilization> resources;    // canonical order by resource id
  std::int64_t total_granted = 0;
  std::int64_t total_reserved = 0;
  bool degraded = false;

  bool empty() const noexcept { return demands.empty(); }
  const DemandAllocation* find_demand(const DemandId& id) const noexcept;

  Digest digest() const;
};

// Independent verifier. Re-checks every hard invariant of the allocation against
// the snapshot without trusting the solver. Returns the canonical binding set
// that was violated (empty == verified).
std::vector<BindingConstraint> verify_allocation(const FabricSnapshot& snapshot,
                                                 const Allocation& allocation);

// ---------------------------------------------------------------------------
// Churn control
// ---------------------------------------------------------------------------

enum class ChurnDecision : std::uint8_t {
  no_incumbent = 0,        // nothing to compare against; the proposal stands alone
  accept = 1,              // improvement crossed every policy threshold
  hold_incumbent = 2,      // the improvement did not clear a policy gate
  reject_regression = 3,   // the proposal scores worse than the incumbent
};

std::string_view to_string(ChurnDecision decision) noexcept;

struct ChurnReport {
  ChurnDecision decision = ChurnDecision::no_incumbent;
  std::int64_t incumbent_score = 0;
  std::int64_t proposal_score = 0;
  std::int64_t improvement_permille = 0;
  std::int64_t required_improvement_permille = 0;

  std::int64_t moved_bandwidth = 0;
  std::int64_t total_bandwidth = 0;
  std::int64_t moved_permille = 0;
  std::int64_t max_moved_permille = 0;

  std::uint32_t demands_changed = 0;
  std::uint32_t max_affected_demands = 0;
  std::uint32_t paths_added = 0;
  std::uint32_t paths_removed = 0;
  std::uint32_t failure_domains_changed = 0;
  std::int64_t operational_risk_permille = 0;

  std::string rationale;

  friend bool operator==(const ChurnReport&, const ChurnReport&) noexcept = default;
};

// Deterministic comparison between an incumbent allocation and a proposal.
// Scores are lower-is-better objective scores.
ChurnReport compare_churn(const Allocation& incumbent,
                          const Allocation& proposal,
                          const FabricSnapshot& snapshot,
                          const Policy& policy,
                          std::int64_t incumbent_score,
                          std::int64_t proposal_score);

}  // namespace tef
