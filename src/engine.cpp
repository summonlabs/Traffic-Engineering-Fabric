// Traffic Engineering Fabric - planning engine.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// ---------------------------------------------------------------------------
// CONCURRENCY AND LOCK-REENTRANCY CONTRACT (audited, see docs/concurrency.md)
// ---------------------------------------------------------------------------
//  * The engine owns exactly one lock: Impl::mutex (a shared_mutex).
//  * Read-only public methods take a shared_lock. Mutating public methods take a
//    unique_lock. No other lock is taken anywhere in this translation unit.
//  * Helper functions named *_locked assume the caller already holds the lock.
//    They never acquire it again, and no public method is ever called from a
//    *_locked helper. There is therefore no read-then-write upgrade path and no
//    recursive acquisition of Impl::mutex.
//  * No callback supplied by a caller is ever invoked while the lock is held.
//    The engine has no callback registry at all.
//  * Durable writes happen while the unique lock is held, on the calling thread,
//    through PlanRepository. Lock ordering is therefore strictly
//    Engine::Impl::mutex -> PlanRepository::Impl::mutex. PlanRepository never
//    calls back into the engine, so the order is never reversed.
//  * No lock is held while a thread is created or joined; the engine owns no
//    threads.
#include "tef/engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "tef/derived.hpp"
#include "tef/numeric.hpp"
#include "tef/persist.hpp"

namespace tef {
namespace {

bool is_live_state(PlanState state) { return PlanStateMachine::is_live(state); }

}  // namespace

struct Engine::Impl {
  struct PendingSolve {
    bool allow_degraded = false;
    bool has_incumbent = false;
    Allocation incumbent;
    PlanRef incumbent_ref;
  };

  mutable std::shared_mutex mutex;
  EngineConfig config;
  std::shared_ptr<PlanRepository> repository;

  std::optional<FabricSnapshot> snapshot;
  AuthorityVector live;
  bool live_valid = false;

  std::map<PlanId, Plan> plans;
  std::map<PlanId, Explanation> explanations;
  std::map<PlanId, PendingSolve> pending;
  PlanRef incumbent;
  std::uint64_t tick = 0;
  Stats stats;
  RecoverReport recovery;
};

Engine::Engine(EngineConfig config) : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
}

Engine::~Engine() = default;

Status Engine::attach_repository(std::shared_ptr<PlanRepository> repository) {
  std::unique_lock lock(impl_->mutex);
  if (impl_->repository != nullptr) {
    return fail(ErrorCode::invalid_argument, "a repository is already attached");
  }
  if (!impl_->plans.empty()) {
    return fail(ErrorCode::invalid_argument,
                "a repository can only be attached before any plan is created");
  }
  impl_->repository = std::move(repository);
  return Status::success();
}

void Engine::detach_repository() {
  std::unique_lock lock(impl_->mutex);
  impl_->repository.reset();
}

Status Engine::submit_snapshot(FabricSnapshot snapshot) {
  std::unique_lock lock(impl_->mutex);
  canonicalize(snapshot);
  const Status structural = validate_structure(snapshot);
  if (!structural.ok()) return structural;
  impl_->snapshot = std::move(snapshot);
  impl_->live = authority_of(*impl_->snapshot);
  impl_->live_valid = true;

  // A plan whose bound authority is no longer the live authority is moved to
  // STALE rather than being silently reinterpreted against the new inputs:
  //   * a live plan can never be committed against inputs that have moved on;
  //   * a committed plan stays a durable historical fact, but its *applicability*
  //     to the live fabric is no longer CURRENT, so a reader must never be told
  //     that a plan which no longer fits the live fabric is current.
  for (auto& kv : impl_->plans) {
    Plan& plan = kv.second;
    if (plan.state != PlanState::committed && !is_live_state(plan.state)) continue;
    const AuthorityDelta delta = compare_authority(plan.authority, impl_->live);
    if (delta.identical()) continue;
    plan.state = PlanState::stale;
    plan.applicability = PlanApplicability::stale;
    plan.feasibility.summary = "stale: " + delta.describe();
    ++impl_->stats.plans_stale;
    const Status persisted =
        impl_->repository ? impl_->repository->append(RecordType::plan, encode_plan(plan))
                          : Status::success();
    if (!persisted.ok()) return persisted;
  }
  return Status::success();
}

Result<Plan> Engine::declare(const DeclareRequest& request) {
  std::unique_lock lock(impl_->mutex);
  if (!impl_->live_valid) {
    return fail_as<Plan>(ErrorCode::stale_authority,
                         "no authoritative snapshot has been submitted");
  }
  if (!request.plan_id.valid() || !request.attempt.valid() || !request.attempt_generation.valid() ||
      !request.publisher.valid() || !request.publisher_boot.valid()) {
    return fail_as<Plan>(ErrorCode::invalid_argument,
                         "the declare request is missing identity or incarnation fields");
  }
  if (!impl_->config.coordinator_incarnation.valid()) {
    return fail_as<Plan>(ErrorCode::invalid_generation,
                         "the engine has no coordinator incarnation and cannot bind a plan");
  }
  if (impl_->plans.size() >= impl_->config.max_records) {
    return fail_as<Plan>(ErrorCode::limit_exceeded, "the plan record bound has been reached");
  }
  if (impl_->plans.find(request.plan_id) != impl_->plans.end()) {
    return fail_as<Plan>(ErrorCode::duplicate_identity,
                         "a plan with this identity already exists");
  }

  Plan plan;
  plan.id = request.plan_id;
  plan.generation = PlanGeneration::first();
  plan.state = PlanState::declared;
  plan.applicability = PlanApplicability::not_evaluated;
  plan.attempt = request.attempt;
  plan.attempt_generation = request.attempt_generation;
  plan.publisher = request.publisher;
  plan.publisher_boot = request.publisher_boot;
  plan.coordinator_incarnation = impl_->config.coordinator_incarnation;
  plan.authority = impl_->live;
  plan.declared_tick = request.tick;

  Impl::PendingSolve pending;
  pending.allow_degraded = request.allow_degraded;
  pending.has_incumbent = request.has_incumbent;
  pending.incumbent = request.incumbent;
  pending.incumbent_ref = request.incumbent_ref;

  if (impl_->repository) {
    const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
    if (!persisted.ok()) return Result<Plan>(persisted.error());
  }
  ++impl_->stats.plans_created;
  impl_->pending.emplace(plan.id, std::move(pending));
  impl_->plans.emplace(plan.id, plan);
  return plan;
}

Status Engine::validate(const PlanRef& ref) {
  std::unique_lock lock(impl_->mutex);
  auto it = impl_->plans.find(ref.id);
  if (it == impl_->plans.end()) return fail(ErrorCode::not_found, "unknown plan identity");
  Plan& plan = it->second;
  if (!(plan.generation == ref.generation)) {
    return fail(ErrorCode::stale_authority, "the plan generation reference is not current");
  }
  if (!PlanStateMachine::can_transition(plan.state, PlanState::validated)) {
    return fail(ErrorCode::invalid_transition,
                std::string("cannot validate a plan in state ") + std::string(to_string(plan.state)));
  }
  if (!impl_->live_valid) {
    plan.state = PlanState::stale;
    plan.applicability = PlanApplicability::stale;
    return fail(ErrorCode::stale_authority, "no authoritative snapshot has been submitted");
  }
  const AuthorityDelta delta = compare_authority(plan.authority, impl_->live);
  if (!delta.identical()) {
    plan.state = PlanState::stale;
    plan.applicability = PlanApplicability::stale;
    plan.feasibility.summary = "stale: " + delta.describe();
    ++impl_->stats.plans_stale;
    if (impl_->repository) {
      const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
      if (!persisted.ok()) return persisted;
    }
    return fail(ErrorCode::stale_authority, "the bound authority advanced: " + delta.describe());
  }
  const Status structural = validate_structure(*impl_->snapshot);
  if (!structural.ok()) return structural;

  plan.state = PlanState::validated;
  plan.applicability = PlanApplicability::current;
  if (impl_->repository) {
    const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
    if (!persisted.ok()) return persisted;
  }
  return Status::success();
}

Result<Plan> Engine::solve(const PlanRef& ref) {
  std::unique_lock lock(impl_->mutex);
  auto it = impl_->plans.find(ref.id);
  if (it == impl_->plans.end()) return fail_as<Plan>(ErrorCode::not_found, "unknown plan identity");
  Plan& plan = it->second;
  if (!(plan.generation == ref.generation)) {
    return fail_as<Plan>(ErrorCode::stale_authority, "the plan generation reference is not current");
  }
  if (!PlanStateMachine::can_transition(plan.state, PlanState::solving)) {
    return fail_as<Plan>(ErrorCode::invalid_transition,
                         std::string("cannot solve a plan in state ") +
                             std::string(to_string(plan.state)));
  }
  if (!impl_->live_valid) {
    return fail_as<Plan>(ErrorCode::stale_authority, "no authoritative snapshot has been submitted");
  }
  const AuthorityDelta delta = compare_authority(plan.authority, impl_->live);
  if (!delta.identical()) {
    plan.state = PlanState::stale;
    plan.applicability = PlanApplicability::stale;
    plan.feasibility.summary = "stale: " + delta.describe();
    ++impl_->stats.plans_stale;
    if (impl_->repository) {
      const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
      if (!persisted.ok()) return Result<Plan>(persisted.error());
    }
    return fail_as<Plan>(ErrorCode::stale_authority,
                         "the bound authority advanced before solving: " + delta.describe());
  }

  plan.state = PlanState::solving;

  Impl::PendingSolve pending;
  const auto pending_it = impl_->pending.find(plan.id);
  if (pending_it != impl_->pending.end()) pending = pending_it->second;

  SolveOptions options;
  options.allow_degraded = pending.allow_degraded;
  options.has_incumbent = pending.has_incumbent;
  options.incumbent = pending.incumbent;

  const Result<SolveOutcome> outcome = tef::solve(*impl_->snapshot, options);
  if (!outcome.has_value()) {
    plan.state = PlanState::rejected;
    plan.applicability = PlanApplicability::not_evaluated;
    plan.feasibility.status = FeasibilityStatus::conflicting_input;
    plan.feasibility.summary = outcome.error().format();
    ++impl_->stats.plans_rejected;
    if (impl_->repository) {
      const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
      if (!persisted.ok()) return Result<Plan>(persisted.error());
    }
    return Result<Plan>(outcome.error());
  }

  const SolveOutcome& solved = outcome.value();
  plan.allocation = solved.allocation;
  plan.feasibility = solved.feasibility;
  plan.components = solved.components;
  plan.alternatives = solved.alternatives;
  plan.objective_score = solved.score;
  plan.proposed_tick = impl_->tick++;

  switch (solved.status) {
    case FeasibilityStatus::feasible:
      plan.state = PlanState::proposed;
      break;
    case FeasibilityStatus::feasible_degraded:
      plan.state = PlanState::degraded;
      break;
    default:
      plan.state = PlanState::rejected;
      ++impl_->stats.plans_rejected;
      break;
  }

  if (pending.has_incumbent && PlanStateMachine::holds_allocation(plan.state)) {
    const std::int64_t incumbent_score =
        score_allocation(*impl_->snapshot, pending.incumbent, nullptr);
    plan.churn = compare_churn(pending.incumbent, plan.allocation, *impl_->snapshot,
                               impl_->snapshot->policy, incumbent_score, plan.objective_score);
  } else if (pending.has_incumbent) {
    plan.churn.decision = ChurnDecision::hold_incumbent;
    plan.churn.rationale = "the proposal was rejected before a churn comparison could be made";
  }

  Explanation explanation = build_explanation(plan, *impl_->snapshot);
  plan.explanation = explanation.id;
  plan.explanation_digest = explanation.digest();
  impl_->explanations[plan.id] = std::move(explanation);

  if (impl_->repository) {
    const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
    if (!persisted.ok()) return Result<Plan>(persisted.error());
  }
  return plan;
}

Result<Plan> Engine::authorize(const PlanRef& ref, const AuthorizeRequest& request) {
  std::unique_lock lock(impl_->mutex);
  auto it = impl_->plans.find(ref.id);
  if (it == impl_->plans.end()) return fail_as<Plan>(ErrorCode::not_found, "unknown plan identity");
  Plan& plan = it->second;
  if (!(plan.generation == ref.generation)) {
    return fail_as<Plan>(ErrorCode::stale_authority, "the plan generation reference is not current");
  }
  if (!PlanStateMachine::can_transition(plan.state, PlanState::authorized)) {
    return fail_as<Plan>(ErrorCode::invalid_transition,
                         std::string("cannot authorize a plan in state ") +
                             std::string(to_string(plan.state)));
  }
  if (!impl_->live_valid) {
    return fail_as<Plan>(ErrorCode::stale_authority, "no authoritative snapshot has been submitted");
  }

  // Pre-authorization revalidation against the live authority.
  const AuthorityDelta delta = compare_authority(plan.authority, impl_->live);
  if (delta.stale()) {
    plan.state = PlanState::stale;
    plan.applicability = PlanApplicability::stale;
    plan.feasibility.summary = "stale: " + delta.describe();
    ++impl_->stats.plans_stale;
    if (impl_->repository) {
      const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
      if (!persisted.ok()) return Result<Plan>(persisted.error());
    }
    return fail_as<Plan>(ErrorCode::stale_authority,
                         "the bound authority advanced before authorization: " + delta.describe());
  }
  if (delta.contradictory()) {
    plan.state = PlanState::rejected;
    plan.feasibility.summary = "conflicting authority: " + delta.describe();
    ++impl_->stats.plans_rejected;
    if (impl_->repository) {
      const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
      if (!persisted.ok()) return Result<Plan>(persisted.error());
    }
    return fail_as<Plan>(ErrorCode::conflicting_authority,
                         "the bound authority contradicts the live authority: " + delta.describe());
  }

  if (plan.state == PlanState::degraded && !impl_->snapshot->policy.allow_degraded_commit) {
    plan.state = PlanState::rejected;
    plan.feasibility.summary =
        "the active policy does not permit a degraded result to be committed";
    ++impl_->stats.plans_rejected;
    if (impl_->repository) {
      const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
      if (!persisted.ok()) return Result<Plan>(persisted.error());
    }
    return fail_as<Plan>(ErrorCode::degraded_not_permitted,
                         "the plan is degraded and the active policy forbids a degraded commit");
  }

  if (plan.churn.decision == ChurnDecision::hold_incumbent ||
      plan.churn.decision == ChurnDecision::reject_regression) {
    return fail_as<Plan>(ErrorCode::churn_bound,
                         "the incumbent allocation is retained: " + plan.churn.rationale);
  }

  plan.state = PlanState::authorized;
  plan.applicability = PlanApplicability::current;
  ++impl_->stats.commits_revalidated;
  if (impl_->repository) {
    const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
    if (!persisted.ok()) return Result<Plan>(persisted.error());
  }
  (void)request;
  return plan;
}

Result<Plan> Engine::commit(const PlanRef& ref, const CommitIntent& intent) {
  std::unique_lock lock(impl_->mutex);
  auto it = impl_->plans.find(ref.id);
  if (it == impl_->plans.end()) return fail_as<Plan>(ErrorCode::not_found, "unknown plan identity");
  Plan& plan = it->second;
  if (!(plan.generation == ref.generation)) {
    return fail_as<Plan>(ErrorCode::stale_authority, "the plan generation reference is not current");
  }
  if (!intent.commit.valid() || !intent.commit_generation.valid()) {
    return fail_as<Plan>(ErrorCode::invalid_generation,
                         "the commit identity or commit generation is absent");
  }

  // Duplicate commit frames: an exact replay of the same commit identity and
  // generation under the same authority is idempotent. Any other repeat is a
  // conflicting commit and is refused.
  if (plan.state == PlanState::committed) {
    if (plan.commit == intent.commit && plan.commit_generation == intent.commit_generation) {
      return plan;
    }
    return fail_as<Plan>(ErrorCode::already_committed,
                         "the plan is already committed under a different commit generation");
  }
  if (!PlanStateMachine::can_transition(plan.state, PlanState::committed)) {
    return fail_as<Plan>(ErrorCode::invalid_transition,
                         std::string("cannot commit a plan in state ") +
                             std::string(to_string(plan.state)));
  }
  if (!impl_->live_valid) {
    return fail_as<Plan>(ErrorCode::stale_authority, "no authoritative snapshot has been submitted");
  }

  // Pre-commit revalidation: this is the last gate before authoritative state
  // changes, and it re-checks every bound generation.
  const AuthorityDelta delta = compare_authority(plan.authority, impl_->live);
  if (delta.stale()) {
    plan.state = PlanState::stale;
    plan.applicability = PlanApplicability::stale;
    plan.feasibility.summary = "stale: " + delta.describe();
    ++impl_->stats.plans_stale;
    if (impl_->repository) {
      const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
      if (!persisted.ok()) return Result<Plan>(persisted.error());
    }
    return fail_as<Plan>(ErrorCode::stale_authority,
                         "the bound authority advanced before commit: " + delta.describe());
  }
  if (delta.contradictory()) {
    plan.state = PlanState::rejected;
    plan.feasibility.summary = "conflicting authority: " + delta.describe();
    ++impl_->stats.plans_rejected;
    if (impl_->repository) {
      const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
      if (!persisted.ok()) return Result<Plan>(persisted.error());
    }
    return fail_as<Plan>(ErrorCode::conflicting_authority,
                         "the bound authority contradicts the live authority: " + delta.describe());
  }

  const std::vector<BindingConstraint> violations =
      verify_allocation(*impl_->snapshot, plan.allocation);
  if (!violations.empty()) {
    plan.state = PlanState::rejected;
    plan.feasibility.summary = "pre-commit verification failed";
    for (const auto& violation : violations) plan.feasibility.binding.push_back(violation);
    canonicalize_bindings(plan.feasibility.binding, plan.feasibility.binding_truncated);
    ++impl_->stats.plans_rejected;
    if (impl_->repository) {
      const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
      if (!persisted.ok()) return Result<Plan>(persisted.error());
    }
    return fail_as<Plan>(ErrorCode::integrity_failure,
                         "the allocation failed pre-commit verification against the live snapshot");
  }

  // Supersede the current incumbent, if any.
  Plan* previous = nullptr;
  if (impl_->incumbent.id.valid() && !(impl_->incumbent.id == plan.id)) {
    const auto previous_it = impl_->plans.find(impl_->incumbent.id);
    if (previous_it != impl_->plans.end() && previous_it->second.state == PlanState::committed) {
      previous = &previous_it->second;
    }
  }

  plan.state = PlanState::committed;
  plan.applicability = PlanApplicability::current;
  plan.commit = intent.commit;
  plan.commit_generation = intent.commit_generation;
  plan.committed_tick = intent.tick;

  if (previous != nullptr) {
    previous->state = PlanState::superseded;
    previous->applicability = PlanApplicability::superseded;
    previous->superseded_by = plan.ref();
    plan.supersedes = previous->ref();
    ++impl_->stats.supersessions;
  }
  impl_->incumbent = plan.ref();
  ++impl_->stats.plans_committed;

  if (impl_->repository) {
    if (previous != nullptr) {
      const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(*previous));
      if (!persisted.ok()) return Result<Plan>(persisted.error());
    }
    const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
    if (!persisted.ok()) return Result<Plan>(persisted.error());
  }
  return plan;
}

Result<Plan> Engine::supersede(const PlanRef& incumbent_ref, const PlanRef& replacement_ref) {
  std::unique_lock lock(impl_->mutex);
  auto incumbent_it = impl_->plans.find(incumbent_ref.id);
  auto replacement_it = impl_->plans.find(replacement_ref.id);
  if (incumbent_it == impl_->plans.end() || replacement_it == impl_->plans.end()) {
    return fail_as<Plan>(ErrorCode::not_found, "unknown plan identity");
  }
  Plan& incumbent = incumbent_it->second;
  Plan& replacement = replacement_it->second;
  if (incumbent.state != PlanState::committed) {
    return fail_as<Plan>(ErrorCode::invalid_transition,
                         "only a committed plan can be superseded explicitly");
  }
  if (replacement.state != PlanState::committed) {
    return fail_as<Plan>(ErrorCode::invalid_transition,
                         "only a committed plan can supersede another plan");
  }
  if (incumbent.id == replacement.id) {
    return fail_as<Plan>(ErrorCode::cyclic_reference, "a plan cannot supersede itself");
  }

  // Lineage cycle detection: walk the supersedes chain from the replacement.
  PlanRef cursor = replacement.supersedes;
  std::size_t depth = 0;
  while (cursor.id.valid() && depth < Limits::max_lineage_depth) {
    if (cursor.id == incumbent.id) {
      return fail_as<Plan>(ErrorCode::cyclic_reference,
                           "the supersession lineage would contain a cycle");
    }
    const auto next = impl_->plans.find(cursor.id);
    if (next == impl_->plans.end()) break;
    cursor = next->second.supersedes;
    ++depth;
  }
  if (depth >= Limits::max_lineage_depth) {
    return fail_as<Plan>(ErrorCode::cyclic_reference, "the supersession lineage depth exceeds the bound");
  }

  incumbent.state = PlanState::superseded;
  incumbent.applicability = PlanApplicability::superseded;
  incumbent.superseded_by = replacement.ref();
  replacement.supersedes = incumbent.ref();
  ++impl_->stats.supersessions;

  if (impl_->repository) {
    const Status first = impl_->repository->append(RecordType::plan, encode_plan(incumbent));
    if (!first.ok()) return Result<Plan>(first.error());
    const Status second = impl_->repository->append(RecordType::plan, encode_plan(replacement));
    if (!second.ok()) return Result<Plan>(second.error());
  }
  return incumbent;
}

Result<Plan> Engine::mark_stale(const PlanRef& ref) {
  std::unique_lock lock(impl_->mutex);
  auto it = impl_->plans.find(ref.id);
  if (it == impl_->plans.end()) return fail_as<Plan>(ErrorCode::not_found, "unknown plan identity");
  Plan& plan = it->second;
  if (!PlanStateMachine::can_transition(plan.state, PlanState::stale)) {
    return fail_as<Plan>(ErrorCode::invalid_transition,
                         std::string("cannot mark a plan in state ") +
                             std::string(to_string(plan.state)) + " stale");
  }
  plan.state = PlanState::stale;
  plan.applicability = PlanApplicability::stale;
  ++impl_->stats.plans_stale;
  if (impl_->repository) {
    const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
    if (!persisted.ok()) return Result<Plan>(persisted.error());
  }
  return plan;
}

Result<Plan> Engine::retire(const PlanRef& ref) {
  std::unique_lock lock(impl_->mutex);
  auto it = impl_->plans.find(ref.id);
  if (it == impl_->plans.end()) return fail_as<Plan>(ErrorCode::not_found, "unknown plan identity");
  Plan& plan = it->second;
  if (!PlanStateMachine::can_transition(plan.state, PlanState::retired)) {
    return fail_as<Plan>(ErrorCode::invalid_transition,
                         std::string("cannot retire a plan in state ") +
                             std::string(to_string(plan.state)));
  }
  plan.state = PlanState::retired;
  plan.applicability = PlanApplicability::retired;
  if (impl_->incumbent.id == plan.id) impl_->incumbent = PlanRef{};
  if (impl_->repository) {
    const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
    if (!persisted.ok()) return Result<Plan>(persisted.error());
  }
  return plan;
}

std::optional<Plan> Engine::get(const PlanRef& ref) const {
  std::shared_lock lock(impl_->mutex);
  const auto it = impl_->plans.find(ref.id);
  if (it == impl_->plans.end()) return std::nullopt;
  if (ref.generation.valid() && !(it->second.generation == ref.generation)) return std::nullopt;
  return it->second;
}

std::optional<Plan> Engine::get(const PlanId& id) const {
  std::shared_lock lock(impl_->mutex);
  const auto it = impl_->plans.find(id);
  if (it == impl_->plans.end()) return std::nullopt;
  return it->second;
}

std::vector<Plan> Engine::list(PlanState state) const {
  std::shared_lock lock(impl_->mutex);
  std::vector<Plan> out;
  for (const auto& kv : impl_->plans) {
    if (kv.second.state == state) out.push_back(kv.second);
  }
  return out;
}

std::optional<Plan> Engine::incumbent() const {
  std::shared_lock lock(impl_->mutex);
  if (!impl_->incumbent.id.valid()) return std::nullopt;
  const auto it = impl_->plans.find(impl_->incumbent.id);
  if (it == impl_->plans.end()) return std::nullopt;
  return it->second;
}

std::optional<Explanation> Engine::explain(const PlanRef& ref) const {
  std::shared_lock lock(impl_->mutex);
  const auto it = impl_->explanations.find(ref.id);
  if (it == impl_->explanations.end()) return std::nullopt;
  return it->second;
}

AuthorityVector Engine::live_authority() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->live;
}

std::optional<FabricSnapshot> Engine::snapshot_copy() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->snapshot;
}

Result<PlanApplicability> Engine::revalidate(const PlanRef& ref) {
  std::unique_lock lock(impl_->mutex);
  auto it = impl_->plans.find(ref.id);
  if (it == impl_->plans.end()) return fail_as<PlanApplicability>(ErrorCode::not_found, "unknown plan identity");
  Plan& plan = it->second;
  if (!impl_->live_valid) {
    return PlanApplicability::revalidation_required;
  }
  const AuthorityDelta delta = compare_authority(plan.authority, impl_->live);
  if (delta.identical()) {
    plan.applicability = PlanApplicability::current;
    return PlanApplicability::current;
  }
  plan.applicability = PlanApplicability::stale;
  if (plan.state == PlanState::committed) {
    plan.state = PlanState::stale;
    plan.feasibility.summary = "stale: " + delta.describe();
    ++impl_->stats.plans_stale;
    if (impl_->repository) {
      const Status persisted = impl_->repository->append(RecordType::plan, encode_plan(plan));
      if (!persisted.ok()) return Result<PlanApplicability>(persisted.error());
    }
  }
  return PlanApplicability::stale;
}

Status Engine::recover_from_repository() {
  std::unique_lock lock(impl_->mutex);
  if (!impl_->repository) {
    return fail(ErrorCode::invalid_argument, "no repository is attached");
  }
  if (!impl_->plans.empty()) {
    return fail(ErrorCode::invalid_argument, "recovery requires an empty engine");
  }
  impl_->recovery = RecoverReport{};
  const std::vector<DurableRecord> records = impl_->repository->records();
  impl_->recovery.records_read = records.size();

  // The durable log records every transition, so a plan can appear many times.
  // Recovery collapses the log to the latest record per plan identity: that is
  // the durable fact, and it is the only record whose state is authoritative.
  for (const auto& record : records) {
    if (record.type != RecordType::plan) continue;
    const Result<Plan> decoded = decode_plan(record.payload);
    if (!decoded.has_value()) {
      ++impl_->recovery.plans_skipped;
      continue;
    }
    impl_->plans[decoded.value().id] = decoded.value();
  }

  for (auto& kv : impl_->plans) {
    Plan& plan = kv.second;
    // Durable state never restores live authority. A plan that was persisted
    // while it was still advancing is not resumed: it is stale, because the
    // authority that justified it is not restored either.
    if (is_live_state(plan.state)) {
      plan.state = PlanState::stale;
      plan.applicability = PlanApplicability::stale;
      ++impl_->recovery.plans_marked_stale;
      ++impl_->stats.plans_stale;
    } else {
      plan.applicability = PlanApplicability::revalidation_required;
      ++impl_->recovery.plans_marked_revalidation_required;
    }
    ++impl_->recovery.plans_restored;
  }

  impl_->incumbent = PlanRef{};
  for (const auto& kv : impl_->plans) {
    if (kv.second.state == PlanState::committed) impl_->incumbent = kv.second.ref();
  }
  impl_->live_valid = false;
  impl_->snapshot.reset();
  return Status::success();
}

RecoverReport Engine::recovery_report() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->recovery;
}

Engine::Stats Engine::stats() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->stats;
}

}  // namespace tef
