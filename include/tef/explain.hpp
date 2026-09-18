// Traffic Engineering Fabric - deterministic, bounded explanations.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tef/allocation.hpp"
#include "tef/analysis.hpp"
#include "tef/authority.hpp"
#include "tef/derived.hpp"
#include "tef/feasibility.hpp"
#include "tef/model.hpp"
#include "tef/plan.hpp"

namespace tef {

struct Explanation {
  ExplanationId id;
  PlanRef plan;
  PlanState state = PlanState::declared;
  PlanApplicability applicability = PlanApplicability::not_evaluated;
  AuthorityVector authority;
  ObjectiveProfileRef objective;

  FeasibilityResult feasibility;
  std::vector<ObjectiveComponent> components;
  std::int64_t total_score = 0;

  std::vector<BindingConstraint> binding_constraints;
  std::vector<ResourceUtilization> saturated_resources;
  std::vector<DemandAllocation> allocations;
  std::vector<std::string> policy_exclusions;
  std::vector<RejectedAlternative> alternatives;
  ChurnReport churn;
  std::vector<std::string> notes;
  std::vector<std::string> provenance_notes;

  bool truncated = false;
  std::size_t omitted_binding_constraints = 0;
  std::size_t omitted_alternatives = 0;
  std::size_t omitted_allocations = 0;

  // Stable digest over the explanation's canonical content.
  Digest digest() const;

  // Machine-readable rendering (deterministic key order, bounded size).
  std::string to_json() const;
  // Human-readable rendering (deterministic ordering, bounded size).
  std::string to_text() const;
};

// Builds an explanation from a plan. The result is deterministic: identical
// plans produce byte-identical renderings.
Explanation build_explanation(const Plan& plan, const FabricSnapshot& snapshot);

}  // namespace tef
