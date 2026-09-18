// Traffic Engineering Fabric - plan lifecycle implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "tef/plan.hpp"

#include <string>
#include <string_view>

#include "tef/binary.hpp"

namespace tef {

std::string_view to_string(PlanState state) noexcept {
  switch (state) {
    case PlanState::declared: return "DECLARED";
    case PlanState::validated: return "VALIDATED";
    case PlanState::solving: return "SOLVING";
    case PlanState::proposed: return "PROPOSED";
    case PlanState::authorized: return "AUTHORIZED";
    case PlanState::committed: return "COMMITTED";
    case PlanState::superseded: return "SUPERSEDED";
    case PlanState::stale: return "STALE";
    case PlanState::degraded: return "DEGRADED";
    case PlanState::rejected: return "REJECTED";
    case PlanState::retired: return "RETIRED";
  }
  return "UNKNOWN";
}

std::optional<PlanState> plan_state_from_string(std::string_view text) noexcept {
  const PlanState all[] = {PlanState::declared,   PlanState::validated, PlanState::solving,
                           PlanState::proposed,   PlanState::authorized, PlanState::committed,
                           PlanState::superseded, PlanState::stale,     PlanState::degraded,
                           PlanState::rejected,   PlanState::retired};
  for (PlanState state : all) {
    if (to_string(state) == text) return state;
  }
  return std::nullopt;
}

std::string_view to_string(PlanApplicability value) noexcept {
  switch (value) {
    case PlanApplicability::not_evaluated: return "NOT_EVALUATED";
    case PlanApplicability::current: return "CURRENT";
    case PlanApplicability::revalidation_required: return "REVALIDATION_REQUIRED";
    case PlanApplicability::stale: return "STALE";
    case PlanApplicability::superseded: return "SUPERSEDED";
    case PlanApplicability::retired: return "RETIRED";
  }
  return "UNKNOWN";
}

bool PlanStateMachine::is_terminal(PlanState state) noexcept {
  return state == PlanState::rejected || state == PlanState::retired;
}

bool PlanStateMachine::is_live(PlanState state) noexcept {
  switch (state) {
    case PlanState::declared:
    case PlanState::validated:
    case PlanState::solving:
    case PlanState::proposed:
    case PlanState::degraded:
    case PlanState::authorized:
      return true;
    case PlanState::committed:
    case PlanState::superseded:
    case PlanState::stale:
    case PlanState::rejected:
    case PlanState::retired:
      return false;
  }
  return false;
}

bool PlanStateMachine::holds_allocation(PlanState state) noexcept {
  switch (state) {
    case PlanState::proposed:
    case PlanState::degraded:
    case PlanState::authorized:
    case PlanState::committed:
    case PlanState::superseded:
      return true;
    case PlanState::declared:
    case PlanState::validated:
    case PlanState::solving:
    case PlanState::stale:
    case PlanState::rejected:
    case PlanState::retired:
      return false;
  }
  return false;
}

bool PlanStateMachine::can_transition(PlanState from, PlanState to) noexcept {
  if (from == to) return false;
  switch (from) {
    case PlanState::declared:
      return to == PlanState::validated || to == PlanState::rejected || to == PlanState::stale;
    case PlanState::validated:
      return to == PlanState::solving || to == PlanState::rejected || to == PlanState::stale;
    case PlanState::solving:
      return to == PlanState::proposed || to == PlanState::degraded || to == PlanState::rejected ||
             to == PlanState::stale;
    case PlanState::proposed:
      return to == PlanState::authorized || to == PlanState::rejected || to == PlanState::stale;
    case PlanState::degraded:
      return to == PlanState::authorized || to == PlanState::rejected || to == PlanState::stale;
    case PlanState::authorized:
      return to == PlanState::committed || to == PlanState::rejected || to == PlanState::stale;
    case PlanState::committed:
      return to == PlanState::superseded || to == PlanState::stale || to == PlanState::retired;
    case PlanState::superseded:
      return to == PlanState::retired || to == PlanState::stale;
    case PlanState::stale:
      return to == PlanState::retired;
    case PlanState::rejected:
    case PlanState::retired:
      return false;
  }
  return false;
}

std::string_view PlanStateMachine::reason_for_rejection(PlanState from, PlanState to) noexcept {
  if (from == to) return "the plan is already in the requested state";
  if (is_terminal(from)) return "the plan is in a terminal state";
  if (from == PlanState::committed && to != PlanState::superseded && to != PlanState::stale &&
      to != PlanState::retired) {
    return "a committed plan may only be superseded, marked stale, or retired";
  }
  return "the requested state transition is not part of the plan lifecycle";
}

Digest Plan::content_digest() const {
  Writer w;
  w.str(id.str());
  w.u64(generation.value());
  w.u16(static_cast<std::uint16_t>(state));
  w.u16(static_cast<std::uint16_t>(applicability));
  w.str(attempt.str());
  w.u64(attempt_generation.value());
  w.str(publisher.str());
  w.raw(publisher_boot.bytes.data(), publisher_boot.bytes.size());
  w.u64(coordinator_incarnation.value());
  w.digest(authority.digest());
  w.digest(allocation.digest());
  w.u16(static_cast<std::uint16_t>(feasibility.status));
  w.str(feasibility.summary);
  w.u32(static_cast<std::uint32_t>(feasibility.binding.size()));
  for (const auto& constraint : feasibility.binding) {
    w.u16(static_cast<std::uint16_t>(constraint.kind));
    w.str(constraint.subject);
    w.str(constraint.demand.str());
    w.str(constraint.resource.str());
    w.str(constraint.path.str());
    w.i64(constraint.required);
    w.i64(constraint.available);
    w.str(constraint.detail);
  }
  w.u8(static_cast<std::uint8_t>(churn.decision));
  w.i64(churn.moved_bandwidth);
  w.u32(churn.demands_changed);
  w.i64(objective_score);
  w.u32(static_cast<std::uint32_t>(components.size()));
  for (const auto& component : components) {
    w.u16(static_cast<std::uint16_t>(component.term));
    w.i64(component.weight);
    w.i64(component.raw);
    w.i64(component.weighted);
  }
  w.u32(static_cast<std::uint32_t>(alternatives.size()));
  for (const auto& alternative : alternatives) {
    w.str(alternative.subject);
    w.str(alternative.reason);
    w.i64(alternative.delta);
  }
  w.str(explanation.str());
  w.digest(explanation_digest);
  w.str(supersedes.id.str());
  w.u64(supersedes.generation.value());
  w.str(superseded_by.id.str());
  w.u64(superseded_by.generation.value());
  w.str(commit.str());
  w.u64(commit_generation.value());
  w.u64(committed_tick);
  return w.finish_digest("tef.plan.v1");
}

}  // namespace tef
