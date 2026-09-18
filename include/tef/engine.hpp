// Traffic Engineering Fabric - planning engine.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "tef/allocation.hpp"
#include "tef/authority.hpp"
#include "tef/explain.hpp"
#include "tef/model.hpp"
#include "tef/plan.hpp"
#include "tef/solver.hpp"

namespace tef {

class PlanRepository;

struct EngineConfig {
  // The incarnation is stamped on every plan this engine produces. It defaults
  // to the first generation so that a standalone engine is usable immediately;
  // a coordinator always sets its own, advanced, incarnation.
  CoordinatorIncarnation coordinator_incarnation = CoordinatorIncarnation::first();
  CoordinatorId coordinator_id;
  std::uint64_t max_records = Limits::max_audit_records;
};

struct DeclareRequest {
  PlanId plan_id;
  AttemptId attempt;
  AttemptGeneration attempt_generation;
  PublisherId publisher;
  BootId publisher_boot;
  bool allow_degraded = false;
  bool has_incumbent = false;
  Allocation incumbent;
  PlanRef incumbent_ref;
  std::uint64_t tick = 0;
};

struct AuthorizeRequest {
  std::uint64_t tick = 0;
  std::string note;
};

struct RecoverReport {
  std::uint64_t records_read = 0;
  std::uint64_t plans_restored = 0;
  std::uint64_t plans_marked_stale = 0;
  std::uint64_t plans_marked_revalidation_required = 0;
  std::uint64_t plans_skipped = 0;
};

// Engine-side commit intent. Distinct from the wire CommitRequest carried by
// tef/protocol.hpp: this type is the local decision, not the framed message.
struct CommitIntent {
  CommitId commit;
  CommitGeneration commit_generation;
  std::uint64_t tick = 0;
};

// The engine owns committed traffic-engineering intent. It does not own
// topology, path legality, admission, reservations lifecycle, or route
// installation.
class Engine {
 public:
  explicit Engine(EngineConfig config = {});
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Attaches a durable repository. All subsequent authoritative mutations are
  // written to it before they become visible. May only be set while no plan
  // exists.
  Status attach_repository(std::shared_ptr<PlanRepository> repository);
  void detach_repository();

  // Publishes the authoritative snapshot this runtime plans against. The
  // snapshot is validated and canonicalized before it becomes current.
  Status submit_snapshot(FabricSnapshot snapshot);

  // Planning pipeline. Each stage is separately callable so that planning,
  // authorization, and commit remain distinct operations.
  Result<Plan> declare(const DeclareRequest& request);
  Status validate(const PlanRef& ref);
  Result<Plan> solve(const PlanRef& ref);
  Result<Plan> authorize(const PlanRef& ref, const AuthorizeRequest& request);
  Result<Plan> commit(const PlanRef& ref, const CommitIntent& intent);
  Result<Plan> supersede(const PlanRef& incumbent, const PlanRef& by);
  Result<Plan> mark_stale(const PlanRef& ref);
  Result<Plan> retire(const PlanRef& ref);

  // Read-only queries. These never mutate state and never acquire a write lock.
  std::optional<Plan> get(const PlanRef& ref) const;
  std::optional<Plan> get(const PlanId& id) const;
  std::vector<Plan> list(PlanState state) const;
  std::optional<Plan> incumbent() const;
  std::optional<Explanation> explain(const PlanRef& ref) const;
  AuthorityVector live_authority() const;
  std::optional<FabricSnapshot> snapshot_copy() const;

  // Re-validates a durable plan against the live authority. Never mutates the
  // committed record; only the applicability judgement is updated.
  Result<PlanApplicability> revalidate(const PlanRef& ref);

  // Recovery. Restores durable plan records and marks every one of them
  // REVALIDATION_REQUIRED: durable history never silently becomes live
  // authority after a restart.
  Status recover_from_repository();
  // Returned by value: the report describes internal mutable state, so handing
  // out a reference would let a caller read it concurrently with a later
  // recovery.
  RecoverReport recovery_report() const;

  struct Stats {
    std::uint64_t plans_created = 0;
    std::uint64_t plans_committed = 0;
    std::uint64_t plans_rejected = 0;
    std::uint64_t plans_stale = 0;
    std::uint64_t supersessions = 0;
    std::uint64_t commits_revalidated = 0;
  };
  Stats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tef
