// Traffic Engineering Fabric - deterministic allocation solver.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Algorithm (explicit, deterministic, documented in docs/algorithm.md):
//
//  1. Canonicalize and structurally validate the snapshot.
//  2. Build the effective resource residual set:
//         available(r) = max(0, usable(r) - committed(r) - reserved(r))
//     where reserved(r) is derived from reservation obligations that policy does
//     not permit this runtime to displace.
//  3. Derive each demand's eligible path set (candidate set membership, allowed
//     list, forbidden list, eligibility scope, latency/cost ceilings, explicit
//     failure-domain requirements, policy path-count limits).
//  4. Hard-minimum satisfaction: demands are processed in canonical order
//     (priority descending, then demand id ascending). Each demand's hard
//     minimum is placed by deterministic bottleneck water-filling over its
//     eligible paths, taken in canonical cost order. When a demand cannot be
//     satisfied, a bounded deterministic augmenting repair pass attempts to free
//     capacity by moving already-placed bandwidth of lower-priority demands.
//  5. Infeasibility certification: when a minimum cannot be placed, a
//     max-flow/min-cut certificate over the demand's eligible-path/resource
//     bipartite relaxation proves the shortfall is structural, or the outcome is
//     reported as solver_limit_reached.
//  6. Soft optimization: with all hard minimums placed, the remaining headroom is
//     distributed toward each demand's soft target in canonical order using the
//     active objective profile. Utilization-cap objectives are handled by a
//     bounded parametric search over integer utilization ceilings; the tightest
//     ceiling that still admits the hard minimums becomes a hard bound for the
//     remaining stages.
//  7. Churn control compares the proposal with the incumbent and applies policy
//     thresholds before the proposal is offered for authorization.
//  8. The constructed allocation is re-verified by an independent verifier that
//     re-derives every hard invariant from the snapshot.
#pragma once

#include <cstdint>
#include <vector>

#include "tef/allocation.hpp"
#include "tef/analysis.hpp"
#include "tef/authority.hpp"
#include "tef/derived.hpp"
#include "tef/feasibility.hpp"
#include "tef/model.hpp"

namespace tef {

struct SolveOptions {
  // Deterministic budget. Exceeding it yields solver_limit_reached, never a
  // silently truncated answer.
  std::uint64_t max_iterations = Limits::max_solver_iterations;

  // Optional incumbent used for churn-aware objective terms.
  bool has_incumbent = false;
  Allocation incumbent;

  // When false, an unmet hard minimum is a hard failure. When true, the result
  // is FEASIBLE_DEGRADED and the unmet minimums are reported explicitly.
  bool allow_degraded = false;
};

struct SolveOutcome {
  FeasibilityStatus status = FeasibilityStatus::feasible;
  std::string summary;
  Allocation allocation;
  FeasibilityResult feasibility;
  std::vector<ObjectiveComponent> components;
  std::int64_t score = 0;
  std::vector<RejectedAlternative> alternatives;
  std::vector<std::string> notes;
  std::vector<std::string> policy_exclusions;
  std::uint64_t iterations = 0;
  Digest allocation_digest;

  // True when the allocation was independently re-verified against the snapshot
  // before being returned.
  bool verified = false;
};

Result<SolveOutcome> solve(const FabricSnapshot& snapshot, const SolveOptions& options);

// Evaluates the active objective profile against an existing allocation without
// planning anything. Used to score an incumbent so that churn comparisons are
// made on the same objective in the same way. Returns a saturating total score;
// component values are appended to 'components' when the pointer is non-null.
std::int64_t score_allocation(const FabricSnapshot& snapshot, const Allocation& allocation,
                              std::vector<ObjectiveComponent>* components);

}  // namespace tef
