// Traffic Engineering Fabric - plan lifecycle.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tef/allocation.hpp"
#include "tef/authority.hpp"
#include "tef/feasibility.hpp"
#include "tef/model.hpp"

namespace tef {

// Planning, validation, proposal, authorization, commit, supersession and
// application are distinct stages. A plan advances only along the transitions
// declared below; every other transition is rejected.
enum class PlanState : std::uint8_t {
  declared = 0,
  validated = 1,
  solving = 2,
  proposed = 3,
  authorized = 4,
  committed = 5,
  superseded = 6,
  stale = 7,
  degraded = 8,
  rejected = 9,
  retired = 10,
};

std::string_view to_string(PlanState state) noexcept;
std::optional<PlanState> plan_state_from_string(std::string_view text) noexcept;

// Applicability is orthogonal to lifecycle state: a COMMITTED plan remains a
// durable historical fact forever, but its applicability to the *current* fabric
// is a separate judgement that must be re-established after any restart.
enum class PlanApplicability : std::uint8_t {
  not_evaluated = 0,
  current = 1,                 // revalidated against the live authority
  revalidation_required = 2,   // loaded from durable storage; not yet revalidated
  stale = 3,
  superseded = 4,
  retired = 5,
};

std::string_view to_string(PlanApplicability value) noexcept;

struct PlanStateMachine {
  static bool is_terminal(PlanState state) noexcept;
  static bool is_live(PlanState state) noexcept;   // still advancing toward commit
  static bool holds_allocation(PlanState state) noexcept;
  static bool can_transition(PlanState from, PlanState to) noexcept;
  static std::string_view reason_for_rejection(PlanState from, PlanState to) noexcept;
};

struct Plan {
  PlanId id;
  PlanGeneration generation;
  PlanState state = PlanState::declared;
  PlanApplicability applicability = PlanApplicability::not_evaluated;

  AttemptId attempt;
  AttemptGeneration attempt_generation;
  PublisherId publisher;
  BootId publisher_boot;
  CoordinatorIncarnation coordinator_incarnation;

  AuthorityVector authority;
  Allocation allocation;
  FeasibilityResult feasibility;
  ChurnReport churn;

  std::int64_t objective_score = 0;
  std::vector<ObjectiveComponent> components;
  std::vector<RejectedAlternative> alternatives;
  ExplanationId explanation;
  Digest explanation_digest;

  PlanRef supersedes;          // incumbent this plan replaces (after commit)
  PlanRef superseded_by;

  CommitId commit;
  CommitGeneration commit_generation;
  std::uint64_t committed_tick = 0;

  std::uint64_t declared_tick = 0;
  std::uint64_t proposed_tick = 0;

  PlanRef ref() const noexcept { return PlanRef{id, generation}; }

  // Canonical content digest: identity + state + authority + allocation +
  // feasibility + churn. Two plans with identical content have identical
  // digests regardless of when or where they were produced.
  Digest content_digest() const;

  friend bool operator==(const Plan&, const Plan&) noexcept = default;
};

// Durable record: the plan plus the lineage and audit information required to
// reconstruct supersession after a reopen.
struct PlanRecord {
  Plan plan;
  Digest previous_record_digest;   // hash chain over the durable journal
  std::uint64_t sequence = 0;      // monotonic durable sequence number
};

}  // namespace tef
