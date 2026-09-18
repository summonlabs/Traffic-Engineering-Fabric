// Traffic Engineering Fabric - plan lifecycle, churn and explanation proofs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "support/fixtures.hpp"
#include "support/test_framework.hpp"
#include "tef/engine.hpp"
#include "tef/inspect.hpp"
#include "tef/persist.hpp"
#include "tef/solver.hpp"

using namespace tef;
using tef::test::build_snapshot;
using tef::test::make_objective;
using tef::test::simple_fabric;
using tef::test::SnapshotSpec;

namespace {

DeclareRequest declare_request(std::string_view plan, std::uint64_t tick = 1) {
  DeclareRequest request;
  request.plan_id = PlanId::parse(plan).value();
  request.attempt = AttemptId::parse(std::string("attempt-") + std::string(plan)).value();
  request.attempt_generation = AttemptGeneration::first();
  request.publisher = PublisherId::parse("publisher-one").value();
  request.publisher_boot = derive_boot_id(1, 1);
  request.tick = tick;
  return request;
}

EngineConfig engine_config() {
  EngineConfig config;
  config.coordinator_id = CoordinatorId::parse("tef-coordinator").value();
  config.coordinator_incarnation = CoordinatorIncarnation::first();
  return config;
}

Plan run_to_proposed(Engine& engine, const FabricSnapshot& snapshot, std::string_view plan,
                     DeclareRequest request) {
  const Status submitted = engine.submit_snapshot(snapshot);
  TEF_CHECK_MSG(submitted.ok(), submitted.format());
  const Result<Plan> declared = engine.declare(request);
  TEF_CHECK_MSG(declared.has_value(), declared.has_value() ? "" : declared.error().format());
  const Status validated = engine.validate(declared.value().ref());
  TEF_CHECK_MSG(validated.ok(), validated.format());
  const Result<Plan> solved = engine.solve(declared.value().ref());
  TEF_CHECK_MSG(solved.has_value(), solved.has_value() ? "" : solved.error().format());
  (void)plan;
  return solved.value();
}

}  // namespace

TEF_TEST(engine_advances_a_plan_through_every_lifecycle_stage) {
  Engine engine(engine_config());
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const Plan proposed = run_to_proposed(engine, snapshot, "plan-one", declare_request("plan-one"));
  TEF_CHECK_EQ(proposed.state, PlanState::proposed);
  TEF_CHECK_EQ(proposed.feasibility.status, FeasibilityStatus::feasible);

  const Result<Plan> authorized = engine.authorize(proposed.ref(), AuthorizeRequest{});
  TEF_CHECK_MSG(authorized.has_value(), authorized.has_value() ? "" : authorized.error().format());
  TEF_CHECK_EQ(authorized.value().state, PlanState::authorized);

  CommitIntent intent;
  intent.commit = CommitId::parse("commit-one").value();
  intent.commit_generation = CommitGeneration::first();
  intent.tick = 10;
  const Result<Plan> committed = engine.commit(proposed.ref(), intent);
  TEF_CHECK_MSG(committed.has_value(), committed.has_value() ? "" : committed.error().format());
  TEF_CHECK_EQ(committed.value().state, PlanState::committed);
  TEF_CHECK_EQ(committed.value().commit.str(), std::string("commit-one"));

  const auto incumbent = engine.incumbent();
  TEF_CHECK(incumbent.has_value());
  TEF_CHECK_EQ(incumbent->id.str(), std::string("plan-one"));

  const Result<Plan> retired = engine.retire(proposed.ref());
  TEF_CHECK(retired.has_value());
  TEF_CHECK_EQ(retired.value().state, PlanState::retired);
  TEF_CHECK(!engine.incumbent().has_value());
}

TEF_TEST(engine_rejects_stage_skipping) {
  Engine engine(engine_config());
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  TEF_CHECK(engine.submit_snapshot(snapshot).ok());
  const Result<Plan> declared = engine.declare(declare_request("plan-skip"));
  TEF_CHECK(declared.has_value());

  const Result<Plan> solved_early = engine.solve(declared.value().ref());
  TEF_CHECK(!solved_early.has_value());
  TEF_CHECK_EQ(solved_early.error().code(), ErrorCode::invalid_transition);

  const Result<Plan> authorized_early = engine.authorize(declared.value().ref(), AuthorizeRequest{});
  TEF_CHECK(!authorized_early.has_value());
  TEF_CHECK_EQ(authorized_early.error().code(), ErrorCode::invalid_transition);

  CommitIntent intent;
  intent.commit = CommitId::parse("commit-early").value();
  intent.commit_generation = CommitGeneration::first();
  const Result<Plan> committed_early = engine.commit(declared.value().ref(), intent);
  TEF_CHECK(!committed_early.has_value());
  TEF_CHECK_EQ(committed_early.error().code(), ErrorCode::invalid_transition);
}

TEF_TEST(planning_without_a_snapshot_is_refused) {
  Engine engine(engine_config());
  const Result<Plan> declared = engine.declare(declare_request("plan-no-snapshot"));
  TEF_CHECK(!declared.has_value());
  TEF_CHECK_EQ(declared.error().code(), ErrorCode::stale_authority);
}

TEF_TEST(duplicate_plan_identity_is_refused) {
  Engine engine(engine_config());
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  TEF_CHECK(engine.submit_snapshot(snapshot).ok());
  TEF_CHECK(engine.declare(declare_request("plan-dup")).has_value());
  const Result<Plan> second = engine.declare(declare_request("plan-dup"));
  TEF_CHECK(!second.has_value());
  TEF_CHECK_EQ(second.error().code(), ErrorCode::duplicate_identity);
}

TEF_TEST(a_snapshot_advance_makes_live_plans_stale) {
  Engine engine(engine_config());
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const Plan proposed = run_to_proposed(engine, snapshot, "plan-stale", declare_request("plan-stale"));
  TEF_CHECK_EQ(proposed.state, PlanState::proposed);

  SnapshotSpec advanced = simple_fabric();
  advanced.topology_generation = 2;
  TEF_CHECK(engine.submit_snapshot(build_snapshot(advanced)).ok());

  const auto refreshed = engine.get(proposed.ref());
  TEF_CHECK(refreshed.has_value());
  TEF_CHECK_EQ(refreshed->state, PlanState::stale);

  const Result<Plan> authorized = engine.authorize(proposed.ref(), AuthorizeRequest{});
  TEF_CHECK(!authorized.has_value());
  TEF_CHECK_EQ(authorized.error().code(), ErrorCode::invalid_transition);
}

TEF_TEST(a_snapshot_advance_demotes_the_committed_incumbent_to_stale) {
  Engine engine(engine_config());
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const Plan proposed = run_to_proposed(engine, snapshot, "plan-demote", declare_request("plan-demote"));
  TEF_CHECK(engine.authorize(proposed.ref(), AuthorizeRequest{}).has_value());
  CommitIntent intent;
  intent.commit = CommitId::parse("commit-demote").value();
  intent.commit_generation = CommitGeneration::first();
  const Result<Plan> committed = engine.commit(proposed.ref(), intent);
  TEF_CHECK(committed.has_value());
  TEF_CHECK_EQ(committed.value().applicability, PlanApplicability::current);
  TEF_CHECK(engine.incumbent().has_value());
  TEF_CHECK_EQ(engine.incumbent()->applicability, PlanApplicability::current);

  // Re-submitting the identical authority changes nothing.
  TEF_CHECK(engine.submit_snapshot(snapshot).ok());
  TEF_CHECK_EQ(engine.get(proposed.ref())->state, PlanState::committed);
  TEF_CHECK_EQ(engine.get(proposed.ref())->applicability, PlanApplicability::current);

  // Advancing any bound input demotes the committed record immediately, so no
  // reader can be told that an inapplicable plan is current.
  SnapshotSpec advanced = simple_fabric();
  advanced.topology_generation = 5;
  TEF_CHECK(engine.submit_snapshot(build_snapshot(advanced)).ok());
  const auto after = engine.get(proposed.ref());
  TEF_CHECK(after.has_value());
  TEF_CHECK_EQ(after->state, PlanState::stale);
  TEF_CHECK_EQ(after->applicability, PlanApplicability::stale);
  TEF_CHECK(after->feasibility.summary.find("stale") != std::string::npos);

  // The durable fact survives: the incumbent reference and the commit identity
  // are untouched, only its applicability changed.
  const std::optional<Plan> incumbent_after = engine.incumbent();
  TEF_CHECK(incumbent_after.has_value());
  TEF_CHECK_EQ(incumbent_after->commit.str(), std::string("commit-demote"));
  TEF_CHECK_EQ(incumbent_after->state, PlanState::stale);
  TEF_CHECK_EQ(engine.revalidate(proposed.ref()).value(), PlanApplicability::stale);
}

TEF_TEST(capacity_generation_change_between_authorize_and_commit_is_rejected) {
  Engine engine(engine_config());
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const Plan proposed = run_to_proposed(engine, snapshot, "plan-race", declare_request("plan-race"));
  const Result<Plan> authorized = engine.authorize(proposed.ref(), AuthorizeRequest{});
  TEF_CHECK(authorized.has_value());

  const auto before = engine.get(proposed.ref());
  TEF_CHECK(before.has_value());
  TEF_CHECK_EQ(before->state, PlanState::authorized);

  SnapshotSpec advanced = simple_fabric();
  advanced.link_state_generation = 2;
  advanced.resources[0].usable = 9000;
  TEF_CHECK(engine.submit_snapshot(build_snapshot(advanced)).ok());

  CommitIntent intent;
  intent.commit = CommitId::parse("commit-race").value();
  intent.commit_generation = CommitGeneration::first();
  const Result<Plan> committed = engine.commit(proposed.ref(), intent);
  TEF_CHECK(!committed.has_value());
  TEF_CHECK_EQ(committed.error().code(), ErrorCode::invalid_transition);

  const auto after = engine.get(proposed.ref());
  TEF_CHECK(after.has_value());
  TEF_CHECK_EQ(after->state, PlanState::stale);
  TEF_CHECK(!engine.incumbent().has_value());
}

TEF_TEST(commit_replay_is_idempotent_and_conflicting_replay_is_refused) {
  Engine engine(engine_config());
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const Plan proposed = run_to_proposed(engine, snapshot, "plan-replay", declare_request("plan-replay"));
  TEF_CHECK(engine.authorize(proposed.ref(), AuthorizeRequest{}).has_value());

  CommitIntent intent;
  intent.commit = CommitId::parse("commit-replay").value();
  intent.commit_generation = CommitGeneration::first();
  const Result<Plan> first = engine.commit(proposed.ref(), intent);
  TEF_CHECK(first.has_value());

  const Result<Plan> replay = engine.commit(proposed.ref(), intent);
  TEF_CHECK(replay.has_value());
  TEF_CHECK_EQ(replay.value().state, PlanState::committed);
  TEF_CHECK_EQ(replay.value().commit.str(), std::string("commit-replay"));

  CommitIntent conflicting;
  conflicting.commit = CommitId::parse("commit-other").value();
  conflicting.commit_generation = CommitGeneration::parse(2).value();
  const Result<Plan> second = engine.commit(proposed.ref(), conflicting);
  TEF_CHECK(!second.has_value());
  TEF_CHECK_EQ(second.error().code(), ErrorCode::already_committed);
}

TEF_TEST(commit_produces_supersession_lineage_without_cycles) {
  Engine engine(engine_config());
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());

  const Plan first = run_to_proposed(engine, snapshot, "plan-a", declare_request("plan-a", 1));
  TEF_CHECK(engine.authorize(first.ref(), AuthorizeRequest{}).has_value());
  CommitIntent first_intent;
  first_intent.commit = CommitId::parse("commit-a").value();
  first_intent.commit_generation = CommitGeneration::first();
  TEF_CHECK(engine.commit(first.ref(), first_intent).has_value());

  const Plan second = run_to_proposed(engine, snapshot, "plan-b", declare_request("plan-b", 2));
  TEF_CHECK(engine.authorize(second.ref(), AuthorizeRequest{}).has_value());
  CommitIntent second_intent;
  second_intent.commit = CommitId::parse("commit-b").value();
  second_intent.commit_generation = CommitGeneration::parse(2).value();
  const Result<Plan> committed = engine.commit(second.ref(), second_intent);
  TEF_CHECK(committed.has_value());
  TEF_CHECK_EQ(committed.value().supersedes.id.str(), std::string("plan-a"));

  const auto previous = engine.get(first.ref());
  TEF_CHECK(previous.has_value());
  TEF_CHECK_EQ(previous->state, PlanState::superseded);
  TEF_CHECK_EQ(previous->superseded_by.id.str(), std::string("plan-b"));

  const Result<Plan> cycle = engine.supersede(second.ref(), first.ref());
  TEF_CHECK(!cycle.has_value());
  TEF_CHECK_EQ(cycle.error().code(), ErrorCode::invalid_transition);
}

TEF_TEST(churn_policy_holds_the_incumbent_when_improvement_is_too_small) {
  SnapshotSpec spec = simple_fabric();
  spec.policy.churn_improvement_threshold_permille = 500;
  const FabricSnapshot snapshot = build_snapshot(spec);

  Engine engine(engine_config());
  TEF_CHECK(engine.submit_snapshot(snapshot).ok());
  DeclareRequest baseline_request = declare_request("plan-incumbent", 1);
  const Result<Plan> baseline = engine.declare(baseline_request);
  TEF_CHECK(baseline.has_value());
  TEF_CHECK(engine.validate(baseline.value().ref()).ok());
  const Result<Plan> baseline_solved = engine.solve(baseline.value().ref());
  TEF_CHECK(baseline_solved.has_value());
  TEF_CHECK(engine.authorize(baseline.value().ref(), AuthorizeRequest{}).has_value());
  CommitIntent intent;
  intent.commit = CommitId::parse("commit-incumbent").value();
  intent.commit_generation = CommitGeneration::first();
  TEF_CHECK(engine.commit(baseline.value().ref(), intent).has_value());
  const Allocation incumbent = engine.incumbent()->allocation;

  DeclareRequest request = declare_request("plan-proposal", 2);
  request.has_incumbent = true;
  request.incumbent = incumbent;
  request.incumbent_ref = engine.incumbent()->ref();
  const Result<Plan> proposal = engine.declare(request);
  TEF_CHECK(proposal.has_value());
  TEF_CHECK(engine.validate(proposal.value().ref()).ok());
  const Result<Plan> solved = engine.solve(proposal.value().ref());
  TEF_CHECK(solved.has_value());
  TEF_CHECK_EQ(solved.value().state, PlanState::proposed);
  TEF_CHECK_EQ(solved.value().churn.decision, ChurnDecision::hold_incumbent);

  const Result<Plan> authorized = engine.authorize(solved.value().ref(), AuthorizeRequest{});
  TEF_CHECK(!authorized.has_value());
  TEF_CHECK_EQ(authorized.error().code(), ErrorCode::churn_bound);
}

TEF_TEST(churn_comparison_is_computed_against_a_supplied_incumbent) {
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  Engine engine(engine_config());
  const Plan proposed = run_to_proposed(engine, snapshot, "plan-churn", declare_request("plan-churn"));
  TEF_CHECK(engine.authorize(proposed.ref(), AuthorizeRequest{}).has_value());

  // A synthetic incumbent that spreads the same total across two paths, so the
  // comparison sees real moved bandwidth rather than an identical allocation.
  Allocation incumbent = proposed.allocation;
  TEF_CHECK(!incumbent.demands.empty());
  {
    const std::int64_t total = incumbent.demands[0].granted;
    TEF_CHECK(total >= 2);
    PathShare first;
    first.path = incumbent.demands[0].shares.empty()
                     ? PathId::parse("path-ab").value()
                     : incumbent.demands[0].shares[0].path;
    first.generation = PathGeneration::first();
    first.granted = total / 2;
    PathShare second;
    second.path = PathId::parse("path-cd").value();
    second.generation = PathGeneration::first();
    second.granted = total - first.granted;
    incumbent.demands[0].shares.clear();
    incumbent.demands[0].shares.push_back(first);
    incumbent.demands[0].shares.push_back(second);
  }

  const Allocation proposal = proposed.allocation;
  const ChurnReport report =
      compare_churn(incumbent, proposal, snapshot, snapshot.policy, 1000, 900);
  TEF_CHECK_EQ(report.incumbent_score, 1000);
  TEF_CHECK_EQ(report.proposal_score, 900);
  TEF_CHECK_EQ(report.improvement_permille, 100);
  TEF_CHECK(report.moved_bandwidth > 0);
  TEF_CHECK_EQ(report.decision, ChurnDecision::accept);

  const ChurnReport worse = compare_churn(incumbent, proposal, snapshot, snapshot.policy, 900, 1000);
  TEF_CHECK_EQ(worse.decision, ChurnDecision::reject_regression);
}

TEF_TEST(explanations_are_deterministic_and_bounded) {
  Engine engine(engine_config());
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const Plan proposed = run_to_proposed(engine, snapshot, "plan-explain", declare_request("plan-explain"));

  const auto explanation = engine.explain(proposed.ref());
  TEF_CHECK(explanation.has_value());
  const std::string first_json = explanation->to_json();
  const std::string second_json = explanation->to_json();
  TEF_CHECK_EQ(first_json, second_json);
  TEF_CHECK_EQ(explanation->digest().hex(), explanation->digest().hex());
  TEF_CHECK(first_json.size() < 1024 * 1024);
  TEF_CHECK(first_json.find("authority") != std::string::npos);
  TEF_CHECK(first_json.find("feasibility") != std::string::npos);
  TEF_CHECK(first_json.find("allocations") != std::string::npos);

  const std::string text = explanation->to_text();
  TEF_CHECK(text.find("Traffic Engineering Fabric explanation") != std::string::npos);
  TEF_CHECK(text.find("demand-one") != std::string::npos);

  TEF_CHECK_EQ(explanation->digest().hex(), proposed.explanation_digest.hex());
}

TEF_TEST(recovery_never_restores_live_authority) {
  const std::string directory = tef::test::temporary_directory("recover");
  FabricSnapshot snapshot = build_snapshot(simple_fabric());

  {
    Engine engine(engine_config());
    PlanRepository::Options options;
    options.directory = directory;
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options);
    TEF_CHECK(repository.has_value());
    std::shared_ptr<PlanRepository> shared(std::move(repository.value()));
    TEF_CHECK(engine.attach_repository(shared).ok());

    // A plan that reaches COMMITTED survives the restart.
    const Plan committed = run_to_proposed(engine, snapshot, "plan-durable", declare_request("plan-durable"));
    TEF_CHECK(engine.authorize(committed.ref(), AuthorizeRequest{}).has_value());
    CommitIntent intent;
    intent.commit = CommitId::parse("commit-durable").value();
    intent.commit_generation = CommitGeneration::first();
    TEF_CHECK(engine.commit(committed.ref(), intent).has_value());

    // A plan left mid-flight must NOT come back as live.
    TEF_CHECK(engine.submit_snapshot(snapshot).ok());
    const Result<Plan> live = engine.declare(declare_request("plan-inflight", 5));
    TEF_CHECK(live.has_value());
    TEF_CHECK(engine.validate(live.value().ref()).ok());
    TEF_CHECK(engine.solve(live.value().ref()).has_value());
    TEF_CHECK(shared->close().ok());
  }

  {
    Engine engine(engine_config());
    PlanRepository::Options options;
    options.directory = directory;
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options);
    TEF_CHECK(repository.has_value());
    std::shared_ptr<PlanRepository> shared(std::move(repository.value()));
    TEF_CHECK(engine.attach_repository(shared).ok());
    const Status recovered = engine.recover_from_repository();
    TEF_CHECK_MSG(recovered.ok(), recovered.format());
    TEF_CHECK_EQ(engine.recovery_report().plans_restored, std::uint64_t{2});

    const auto durable = engine.get(PlanId::parse("plan-durable").value());
    TEF_CHECK(durable.has_value());
    TEF_CHECK_EQ(durable->state, PlanState::committed);
    TEF_CHECK_EQ(durable->applicability, PlanApplicability::revalidation_required);

    const auto inflight = engine.get(PlanId::parse("plan-inflight").value());
    TEF_CHECK(inflight.has_value());
    TEF_CHECK_EQ(inflight->state, PlanState::stale);
    TEF_CHECK_EQ(inflight->applicability, PlanApplicability::stale);

    // No snapshot was restored, so a durable plan cannot be treated as current.
    const Result<PlanApplicability> applicability = engine.revalidate(durable->ref());
    TEF_CHECK(applicability.has_value());
    TEF_CHECK_EQ(applicability.value(), PlanApplicability::revalidation_required);

    // Re-submitting the identical authority revalidates the durable plan.
    TEF_CHECK(engine.submit_snapshot(snapshot).ok());
    const Result<PlanApplicability> again = engine.revalidate(durable->ref());
    TEF_CHECK(again.has_value());
    TEF_CHECK_EQ(again.value(), PlanApplicability::current);
    TEF_CHECK(shared->close().ok());
  }
}

TEF_TEST(degraded_results_require_policy_permission_to_commit) {
  SnapshotSpec spec = simple_fabric();
  for (auto& resource : spec.resources) resource.usable = 300;
  spec.demands[0].minimum = 1000;
  spec.demands[0].desired = 1000;
  spec.demands[0].maximum = 1000;
  spec.demands[1].minimum = 0;
  spec.demands[1].desired = 0;
  spec.demands[1].maximum = 0;
  spec.policy.require_minimums = false;
  const FabricSnapshot snapshot = build_snapshot(spec);

  Engine engine(engine_config());
  TEF_CHECK(engine.submit_snapshot(snapshot).ok());
  DeclareRequest request = declare_request("plan-degraded");
  request.allow_degraded = true;
  const Result<Plan> declared = engine.declare(request);
  TEF_CHECK(declared.has_value());
  TEF_CHECK(engine.validate(declared.value().ref()).ok());
  const Result<Plan> solved = engine.solve(declared.value().ref());
  TEF_CHECK(solved.has_value());
  TEF_CHECK_EQ(solved.value().state, PlanState::degraded);

  const Result<Plan> authorized = engine.authorize(solved.value().ref(), AuthorizeRequest{});
  TEF_CHECK(!authorized.has_value());
  TEF_CHECK_EQ(authorized.error().code(), ErrorCode::degraded_not_permitted);
}

TEF_TEST(queries_do_not_mutate_state) {
  Engine engine(engine_config());
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const Plan proposed = run_to_proposed(engine, snapshot, "plan-query", declare_request("plan-query"));
  const Engine::Stats before = engine.stats();
  const Plan before_plan = engine.get(proposed.ref()).value();

  for (int i = 0; i < 8; ++i) {
    (void)engine.get(proposed.ref());
    (void)engine.list(PlanState::proposed);
    (void)engine.incumbent();
    (void)engine.explain(proposed.ref());
    (void)engine.live_authority();
    (void)engine.snapshot_copy();
  }

  const Engine::Stats after = engine.stats();
  TEF_CHECK_EQ(before.plans_created, after.plans_created);
  TEF_CHECK_EQ(before.plans_committed, after.plans_committed);
  TEF_CHECK_EQ(before.plans_stale, after.plans_stale);
  const Plan after_plan = engine.get(proposed.ref()).value();
  TEF_CHECK_EQ(before_plan.content_digest().hex(), after_plan.content_digest().hex());
  TEF_CHECK_EQ(before_plan.state, after_plan.state);
}

TEF_TEST(inspection_helpers_render_the_plan_without_mutating_it) {
  Engine engine(engine_config());
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const Plan proposed = run_to_proposed(engine, snapshot, "plan-inspect", declare_request("plan-inspect"));
  const Digest before = proposed.content_digest();
  const std::string summary = render_plan_summary(proposed);
  TEF_CHECK(summary.find("plan=plan-inspect") != std::string::npos);
  TEF_CHECK(summary.find("state=PROPOSED") != std::string::npos);
  TEF_CHECK_EQ(before.hex(), proposed.content_digest().hex());
  TEF_CHECK(!render_authority(proposed.authority).empty());
  TEF_CHECK(!render_snapshot_summary(snapshot).empty());
  TEF_CHECK(!render_feasibility(proposed.feasibility).empty());
  TEF_CHECK(!render_allocation(proposed.allocation).empty());
  TEF_CHECK(!render_churn(proposed.churn).empty());
}

int main(int argc, char** argv) { return tef::test::run_all(argc, argv); }
