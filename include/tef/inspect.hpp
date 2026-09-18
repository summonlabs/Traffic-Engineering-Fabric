// Traffic Engineering Fabric - inspection helpers.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deterministic, bounded textual renderings used by the shipped tools and by
// tests. These never mutate state and never plan anything.
#pragma once

#include <string>

#include "tef/allocation.hpp"
#include "tef/authority.hpp"
#include "tef/model.hpp"
#include "tef/plan.hpp"

namespace tef {

std::string render_authority(const AuthorityVector& authority);
std::string render_snapshot_summary(const FabricSnapshot& snapshot);
std::string render_feasibility(const FeasibilityResult& feasibility);
std::string render_allocation(const Allocation& allocation);
std::string render_churn(const ChurnReport& churn);
std::string render_plan_summary(const Plan& plan);

}  // namespace tef
