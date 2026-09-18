// Traffic Engineering Fabric - real concurrency and race proofs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every test in this file is a proof obligation about behaviour under real
// concurrency: real threads, real locks, real sockets and real files.
//
// Rules this file obeys without exception:
//   * synchronisation is expressed only with std::latch, std::atomic and thread
//     joins - never with a sleep, a timeout, a watchdog or a force-kill;
//   * every worker thread is joined before its test returns;
//   * a hang is a defect to be diagnosed, never a condition to be tolerated, so
//     nothing here bounds how long a correct implementation may take.
//
// The test framework reports a failure by throwing, and a throw escaping a
// std::thread entry point terminates the process instead of failing a test.
// Worker threads therefore only record observations into storage they own;
// every assertion is evaluated on the owning thread after all workers have been
// joined.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "support/fixtures.hpp"
#include "support/test_framework.hpp"
#include "tef/allocation.hpp"
#include "tef/authority.hpp"
#include "tef/coordinator.hpp"
#include "tef/engine.hpp"
#include "tef/persist.hpp"
#include "tef/protocol.hpp"
#include "tef/publisher.hpp"
#include "tef/solver.hpp"
#include "tef/version.hpp"

using namespace tef;
using tef::test::build_snapshot;
using tef::test::make_policy;
using tef::test::simple_fabric;
using tef::test::SnapshotSpec;
using tef::test::temporary_directory;

namespace {

EngineConfig engine_config() {
  EngineConfig config;
  config.coordinator_id = CoordinatorId::parse("tef-coordinator").value();
  config.coordinator_incarnation = CoordinatorIncarnation::first();
  return config;
}

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

CommitIntent commit_intent(std::string_view commit, std::uint64_t generation) {
  CommitIntent intent;
  intent.commit = CommitId::parse(commit).value();
  intent.commit_generation = CommitGeneration::parse(generation).value();
  intent.tick = generation;
  return intent;
}

// DECLARE -> VALIDATE -> SOLVE. The snapshot must already be live.
Result<Plan> prepare_plan(Engine& engine, std::string_view plan, std::uint64_t tick) {
  const Result<Plan> declared = engine.declare(declare_request(plan, tick));
  if (!declared.has_value()) return declared;
  const Status validated = engine.validate(declared.value().ref());
  if (!validated.ok()) return Result<Plan>(validated.error());
  return engine.solve(declared.value().ref());
}

// DECLARE -> VALIDATE -> SOLVE -> AUTHORIZE against the live authority: the plan
// is then ready to be committed.
Result<Plan> prepare_authorized_plan(Engine& engine, std::string_view plan, std::uint64_t tick) {
  const Result<Plan> proposed = prepare_plan(engine, plan, tick);
  if (!proposed.has_value()) return proposed;
  return engine.authorize(proposed.value().ref(), AuthorizeRequest{});
}

std::string error_text(ErrorCode code) { return std::string(to_string(code)); }

// Outcomes that are legitimate when an authority advance races a commit: the
// engine either refuses against the advanced authority (stale_authority /
// conflicting_authority) or reports the lifecycle consequence of the plan
// having been moved to STALE before the mutation reached the lock
// (invalid_transition). Nothing else may happen.
bool typed_authority_refusal(ErrorCode code) {
  return code == ErrorCode::stale_authority || code == ErrorCode::conflicting_authority ||
         code == ErrorCode::authority_regression || code == ErrorCode::invalid_transition;
}

// Authoritative residual capacity of a snapshot, hand-derived from the snapshot
// itself: available(r) = max(0, usable(r) - committed(r) - reserved(r)). The
// fixtures used here carry no reservations, so reserved(r) is zero.
std::map<ResourceId, std::int64_t> authoritative_available(const FabricSnapshot& snapshot) {
  std::map<ResourceId, std::int64_t> out;
  for (const FabricResource& resource : snapshot.capacity.resources) {
    const std::int64_t residual = resource.usable_capacity - resource.committed_load;
    out.emplace(resource.id, residual < 0 ? 0 : residual);
  }
  return out;
}

std::map<ResourceId, std::int64_t> authoritative_usable(const FabricSnapshot& snapshot) {
  std::map<ResourceId, std::int64_t> out;
  for (const FabricResource& resource : snapshot.capacity.resources) {
    out.emplace(resource.id, resource.usable_capacity);
  }
  return out;
}

// Blocks until a shared progress counter reaches the target. This is a real
// atomic wait (futex-style), not a sleep and not a timeout: the waiter is woken
// by the worker threads themselves, and the workers are guaranteed to make
// further progress, so a correct program cannot block here forever.
void await_progress(const std::atomic<std::size_t>& progress, std::size_t target) {
  std::size_t current = progress.load();
  while (current < target) {
    progress.wait(current);
    current = progress.load();
  }
}

constexpr PlanState kAllPlanStates[] = {
    PlanState::declared,   PlanState::validated, PlanState::solving, PlanState::proposed,
    PlanState::authorized, PlanState::committed, PlanState::superseded, PlanState::stale,
    PlanState::degraded,   PlanState::rejected,  PlanState::retired};

}  // namespace

// ===========================================================================
// 1. Eight threads plan against one engine at the same time.
// ===========================================================================
TEF_TEST(concurrent_plan_creation_from_many_threads) {
  constexpr std::size_t kThreads = 8;

  Engine engine(engine_config());
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const Status submitted = engine.submit_snapshot(snapshot);
  TEF_CHECK_MSG(submitted.ok(), submitted.format());

  struct Outcome {
    bool declared = false;
    bool validated = false;
    bool solved = false;
    ErrorCode failure = ErrorCode::ok;
    Plan plan;
  };
  std::vector<Outcome> outcomes(kThreads);
  std::latch release(kThreads);
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (std::size_t i = 0; i < kThreads; ++i) {
    workers.emplace_back([&engine, &outcomes, &release, i]() {
      Outcome& outcome = outcomes[i];
      const std::string plan = "plan-concurrent-" + std::to_string(i);
      release.arrive_and_wait();  // every thread enters the pipeline together
      const Result<Plan> declared = engine.declare(declare_request(plan, i + 1));
      if (!declared.has_value()) {
        outcome.failure = declared.error().code();
        return;
      }
      outcome.declared = true;
      const Status validated = engine.validate(declared.value().ref());
      if (!validated.ok()) {
        outcome.failure = validated.code();
        return;
      }
      outcome.validated = true;
      const Result<Plan> solved = engine.solve(declared.value().ref());
      if (!solved.has_value()) {
        outcome.failure = solved.error().code();
        return;
      }
      outcome.plan = solved.value();
      outcome.solved = true;
    });
  }
  for (std::thread& worker : workers) worker.join();

  std::set<std::string> distinct_ids;
  for (std::size_t i = 0; i < kThreads; ++i) {
    const Outcome& outcome = outcomes[i];
    TEF_CHECK_MSG(outcome.declared, "thread " + std::to_string(i) + " could not declare: " +
                                        error_text(outcome.failure));
    TEF_CHECK_MSG(outcome.validated, "thread " + std::to_string(i) + " could not validate: " +
                                         error_text(outcome.failure));
    TEF_CHECK_MSG(outcome.solved, "thread " + std::to_string(i) + " could not solve: " +
                                      error_text(outcome.failure));
    TEF_CHECK_EQ(outcome.plan.state, PlanState::proposed);
    TEF_CHECK_EQ(outcome.plan.id.str(), std::string("plan-concurrent-") + std::to_string(i));
    TEF_CHECK(distinct_ids.insert(outcome.plan.id.str()).second);
  }
  TEF_CHECK_EQ(distinct_ids.size(), kThreads);

  // Every produced allocation independently re-verifies against the snapshot it
  // was planned from, and the deterministic solver produced the same answer on
  // every thread despite the contention.
  const std::string first_allocation = outcomes[0].plan.allocation.digest().hex();
  for (std::size_t i = 0; i < kThreads; ++i) {
    const std::vector<BindingConstraint> violations =
        verify_allocation(snapshot, outcomes[i].plan.allocation);
    TEF_CHECK_MSG(violations.empty(), "thread " + std::to_string(i) +
                                          " produced an allocation the verifier rejects: " +
                                          std::to_string(violations.size()) + " violation(s)");
    TEF_CHECK_EQ(outcomes[i].plan.allocation.digest().hex(), first_allocation);
  }

  const Engine::Stats stats = engine.stats();
  TEF_CHECK_EQ(stats.plans_created, static_cast<std::uint64_t>(kThreads));
  TEF_CHECK_EQ(stats.plans_committed, std::uint64_t{0});
  TEF_CHECK_EQ(stats.plans_rejected, std::uint64_t{0});
  TEF_CHECK_EQ(engine.list(PlanState::proposed).size(), kThreads);
  TEF_CHECK_EQ(engine.list(PlanState::declared).size(), std::size_t{0});
}

// ===========================================================================
// 2. Concurrent read-only queries never mutate authoritative state.
// ===========================================================================
TEF_TEST(concurrent_queries_never_mutate_state) {
  constexpr std::size_t kReaders = 8;
  constexpr std::size_t kMaxIterations = 200;

  Engine engine(engine_config());
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  TEF_CHECK(engine.submit_snapshot(snapshot).ok());

  // plan-alpha: committed and then retired. A retired plan is terminal, so no
  // later mutation may touch its record: it is the stable reference the readers
  // watch while everything else moves underneath them.
  const Result<Plan> alpha = prepare_authorized_plan(engine, "plan-alpha", 1);
  TEF_CHECK_MSG(alpha.has_value(), alpha.has_value() ? "" : alpha.error().format());
  TEF_CHECK(engine.commit(alpha.value().ref(), commit_intent("commit-alpha", 1)).has_value());
  TEF_CHECK(engine.retire(alpha.value().ref()).has_value());
  const PlanId alpha_id = alpha.value().id;
  const PlanRef alpha_ref = alpha.value().ref();
  const Digest alpha_digest = engine.get(alpha_id)->content_digest();

  // plan-gamma: committed, and the incumbent that plan-beta replaces.
  const Result<Plan> gamma = prepare_authorized_plan(engine, "plan-gamma", 2);
  TEF_CHECK_MSG(gamma.has_value(), gamma.has_value() ? "" : gamma.error().format());
  TEF_CHECK(engine.commit(gamma.value().ref(), commit_intent("commit-gamma", 2)).has_value());
  const PlanId gamma_id = gamma.value().id;
  const Digest gamma_committed_digest = engine.get(gamma_id)->content_digest();

  // plan-beta: authorized before the readers start, committed and retired by
  // this thread while they are running.
  const Result<Plan> beta = prepare_authorized_plan(engine, "plan-beta", 3);
  TEF_CHECK_MSG(beta.has_value(), beta.has_value() ? "" : beta.error().format());
  const PlanId beta_id = beta.value().id;
  const PlanRef beta_ref = beta.value().ref();

  struct ReaderObservation {
    std::vector<std::string> alpha_digests;
    std::vector<std::string> gamma_digests;
    std::size_t iterations = 0;
    std::size_t missing_snapshots = 0;
    std::size_t missing_explanations = 0;
  };
  std::vector<ReaderObservation> observations(kReaders);
  std::atomic<bool> stop{false};
  std::atomic<std::size_t> progress{0};
  std::latch release(kReaders + 1);
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (std::size_t r = 0; r < kReaders; ++r) {
    readers.emplace_back([&engine, &observations, &stop, &progress, &release, r, alpha_id, alpha_ref,
                          gamma_id]() {
      ReaderObservation& observation = observations[r];
      release.arrive_and_wait();
      // The first iteration always runs, so every reader observes the record at
      // least once; the loop then ends as soon as this thread is told to stop.
      for (std::size_t iteration = 0;; ++iteration) {
        const std::optional<Plan> alpha_now = engine.get(alpha_id);
        if (alpha_now.has_value()) observation.alpha_digests.push_back(alpha_now->content_digest().hex());
        const std::optional<Plan> gamma_now = engine.get(gamma_id);
        if (gamma_now.has_value()) observation.gamma_digests.push_back(gamma_now->content_digest().hex());
        (void)engine.list(PlanState::committed);
        (void)engine.list(PlanState::superseded);
        (void)engine.list(PlanState::retired);
        if (!engine.explain(alpha_ref).has_value()) ++observation.missing_explanations;
        (void)engine.live_authority();
        if (!engine.snapshot_copy().has_value()) ++observation.missing_snapshots;
        ++observation.iterations;
        progress.fetch_add(1);
        progress.notify_all();  // wakes the coordinating thread if it is waiting
        if (stop.load() || iteration + 1 >= kMaxIterations) break;
      }
    });
  }

  // Released together. The coordinating thread then waits on the readers'
  // progress counter (an atomic wait, not a sleep) before every authoritative
  // mutation, so the commit and the retire land while eight threads are
  // demonstrably in the middle of reading.
  constexpr std::size_t kMilestone = kReaders * 32;
  release.arrive_and_wait();
  await_progress(progress, kMilestone);
  const Result<Plan> beta_committed = engine.commit(beta_ref, commit_intent("commit-beta", 3));
  TEF_CHECK_MSG(beta_committed.has_value(), beta_committed.has_value() ? "" : beta_committed.error().format());
  await_progress(progress, 2 * kMilestone);
  const Result<Plan> beta_retired = engine.retire(beta_ref);
  TEF_CHECK_MSG(beta_retired.has_value(), beta_retired.has_value() ? "" : beta_retired.error().format());
  await_progress(progress, 3 * kMilestone);
  stop.store(true);
  for (std::thread& reader : readers) reader.join();

  const Digest gamma_superseded_digest = engine.get(gamma_id)->content_digest();

  std::size_t total_iterations = 0;
  std::size_t alpha_observations = 0;
  std::size_t gamma_observations = 0;
  std::size_t invalid_alpha_digests = 0;
  std::size_t invalid_gamma_digests = 0;
  std::size_t missing_snapshots = 0;
  std::size_t missing_explanations = 0;
  for (const ReaderObservation& observation : observations) {
    total_iterations += observation.iterations;
    alpha_observations += observation.alpha_digests.size();
    gamma_observations += observation.gamma_digests.size();
    missing_snapshots += observation.missing_snapshots;
    missing_explanations += observation.missing_explanations;
    TEF_CHECK(observation.iterations >= 1);
    for (const std::string& digest : observation.alpha_digests) {
      if (digest != alpha_digest.hex()) ++invalid_alpha_digests;
    }
    for (const std::string& digest : observation.gamma_digests) {
      if (digest != gamma_committed_digest.hex() && digest != gamma_superseded_digest.hex()) {
        ++invalid_gamma_digests;
      }
    }
  }
  // The readers really were running: the coordinating thread waited for three
  // milestones before it stopped them.
  TEF_CHECK(total_iterations >= 3 * kMilestone);
  // A read-only query never observed a torn or rewritten record.
  TEF_CHECK_EQ(invalid_alpha_digests, std::size_t{0});
  TEF_CHECK_EQ(invalid_gamma_digests, std::size_t{0});
  TEF_CHECK_EQ(missing_snapshots, std::size_t{0});
  TEF_CHECK_EQ(missing_explanations, std::size_t{0});

  // The plan committed while the readers were running is exactly the record the
  // authoritative mutation wrote: concurrent queries changed nothing.
  const std::optional<Plan> final_beta = engine.get(beta_id);
  TEF_CHECK(final_beta.has_value());
  TEF_CHECK_EQ(final_beta->content_digest().hex(), beta_retired.value().content_digest().hex());
  TEF_CHECK_EQ(final_beta->state, PlanState::retired);
  TEF_CHECK(final_beta->authority == beta_committed.value().authority);
  TEF_CHECK_EQ(final_beta->allocation.digest().hex(), beta_committed.value().allocation.digest().hex());
  TEF_CHECK_EQ(final_beta->commit.str(), std::string("commit-beta"));
  TEF_CHECK_EQ(engine.get(alpha_id)->content_digest().hex(), alpha_digest.hex());

  // The statistics for commits equal the commits this test actually performed:
  // alpha, gamma and beta, with no hidden mutation from the query threads.
  const Engine::Stats stats = engine.stats();
  TEF_CHECK_EQ(stats.plans_created, std::uint64_t{3});
  TEF_CHECK_EQ(stats.plans_committed, std::uint64_t{3});
  TEF_CHECK_EQ(stats.plans_rejected, std::uint64_t{0});
  TEF_CHECK_EQ(stats.plans_stale, std::uint64_t{0});
  TEF_CHECK_EQ(stats.supersessions, std::uint64_t{1});
  TEF_CHECK_EQ(engine.list(PlanState::committed).size(), std::size_t{0});
  TEF_CHECK_EQ(engine.list(PlanState::retired).size(), std::size_t{2});
  std::printf("         [evidence] %zu concurrent query iterations, %zu alpha / %zu gamma digest observations\n",
              total_iterations, alpha_observations, gamma_observations);
}

// ===========================================================================
// 3. A policy generation advance races AUTHORIZE + COMMIT.
// ===========================================================================
TEF_TEST(policy_update_racing_commit) {
  constexpr int kRounds = 32;

  Engine engine(engine_config());
  std::uint64_t successful_commits = 0;
  std::uint64_t refused_commits = 0;
  std::uint64_t commits_ordered_before_the_advance = 0;

  for (int round = 0; round < kRounds; ++round) {
    const std::uint64_t base_generation = static_cast<std::uint64_t>(2 * round + 1);

    SnapshotSpec base_spec = simple_fabric();
    base_spec.policy = make_policy("policy-a", base_generation);
    const FabricSnapshot base = build_snapshot(base_spec);
    const Status base_submitted = engine.submit_snapshot(base);
    TEF_CHECK_MSG(base_submitted.ok(), base_submitted.format());
    const AuthorityVector bound_authority = authority_of(base);
    TEF_CHECK(bound_authority == engine.live_authority());

    const std::string plan_name = "plan-policy-" + std::to_string(round);
    const Result<Plan> proposed = prepare_plan(engine, plan_name, static_cast<std::uint64_t>(round + 1));
    TEF_CHECK_MSG(proposed.has_value(), proposed.has_value() ? "" : proposed.error().format());
    TEF_CHECK_EQ(proposed.value().state, PlanState::proposed);
    TEF_CHECK(proposed.value().authority == bound_authority);

    SnapshotSpec advanced_spec = simple_fabric();
    advanced_spec.policy = make_policy("policy-a", base_generation + 1);
    const FabricSnapshot advanced = build_snapshot(advanced_spec);

    std::latch release(2);
    Status advance_status;
    std::thread advancer([&engine, &advanced, &release, &advance_status]() {
      release.arrive_and_wait();
      advance_status = engine.submit_snapshot(advanced);
    });
    release.arrive_and_wait();
    const Result<Plan> authorized = engine.authorize(proposed.value().ref(), AuthorizeRequest{});
    std::optional<Result<Plan>> committed;
    if (authorized.has_value()) {
      committed = engine.commit(
          proposed.value().ref(),
          commit_intent("commit-policy-" + std::to_string(round), static_cast<std::uint64_t>(round + 1)));
    }
    // Ordering evidence: read the record and the live authority immediately
    // after the commit returned.
    const std::optional<Plan> state_after_commit = engine.get(proposed.value().ref());
    const AuthorityVector live_after_commit = engine.live_authority();
    advancer.join();
    TEF_CHECK_MSG(advance_status.ok(), advance_status.format());
    // The advance has certainly landed by now.
    TEF_CHECK(engine.live_authority() == authority_of(advanced));

    if (committed.has_value() && committed->has_value()) {
      ++successful_commits;
      const Plan& plan = committed->value();
      TEF_CHECK_EQ(plan.state, PlanState::committed);
      // The commit never rebinds the plan: it was committed against exactly the
      // authority it was solved against.
      TEF_CHECK(plan.authority == bound_authority);
      TEF_CHECK(state_after_commit.has_value());
      // This read races the advancer, so exactly two outcomes are legal: the
      // advance had not landed yet (the record is still committed), or it had
      // (the record is stale). Anything else would mean a plan changed state for
      // a reason other than the authority advance.
      TEF_CHECK(state_after_commit->state == PlanState::committed ||
                state_after_commit->state == PlanState::stale);
      if (state_after_commit->state == PlanState::stale) {
        TEF_CHECK_EQ(state_after_commit->applicability, PlanApplicability::stale);
        TEF_CHECK(!compare_authority(bound_authority, live_after_commit).identical());
      }
      TEF_CHECK(state_after_commit->authority == bound_authority);
      const AuthorityDelta delta = compare_authority(bound_authority, live_after_commit);
      TEF_CHECK(delta.regressed.empty());
      if (delta.identical()) {
        // The live authority was still the bound authority when the commit
        // returned: the commit was ordered strictly before the advance.
        ++commits_ordered_before_the_advance;
      } else {
        TEF_CHECK_EQ(delta.advanced.size(), std::size_t{1});
        TEF_CHECK_EQ(delta.advanced.at(0), AuthorityField::policy_generation);
      }
      TEF_CHECK_MSG(verify_allocation(base, plan.allocation).empty(),
                    "the committed allocation does not verify against the snapshot it is bound to");
      // The plan legitimately becomes inapplicable once the advance lands.
      const Result<PlanApplicability> applicability = engine.revalidate(proposed.value().ref());
      TEF_CHECK(applicability.has_value());
      TEF_CHECK_EQ(applicability.value(), PlanApplicability::stale);
      const std::optional<Plan> after = engine.get(proposed.value().ref());
      TEF_CHECK(after.has_value());
      TEF_CHECK_EQ(after->state, PlanState::stale);
      TEF_CHECK_EQ(after->applicability, PlanApplicability::stale);
    } else {
      ++refused_commits;
      const Error& error = authorized.has_value() ? committed->error() : authorized.error();
      TEF_CHECK_MSG(typed_authority_refusal(error.code()),
                    "unexpected refusal code " + error_text(error.code()) + ": " + error.detail());
      const std::optional<Plan> after = engine.get(proposed.value().ref());
      TEF_CHECK(after.has_value());
      TEF_CHECK_NE(after->state, PlanState::committed);
      TEF_CHECK(after->applicability != PlanApplicability::current);
    }
  }

  const Engine::Stats stats = engine.stats();
  TEF_CHECK_EQ(successful_commits + refused_commits, static_cast<std::uint64_t>(kRounds));
  TEF_CHECK_EQ(stats.plans_created, static_cast<std::uint64_t>(kRounds));
  TEF_CHECK_EQ(stats.plans_committed, successful_commits);
  std::printf("         [evidence] policy race: committed=%llu refused=%llu ordered-before-advance=%llu\n",
              static_cast<unsigned long long>(successful_commits),
              static_cast<unsigned long long>(refused_commits),
              static_cast<unsigned long long>(commits_ordered_before_the_advance));
}

// ===========================================================================
// 4. A capacity generation advance races AUTHORIZE + COMMIT.
// ===========================================================================
TEF_TEST(capacity_generation_update_racing_commit) {
  constexpr int kRounds = 32;
  const ResourceId target = ResourceId::parse("res-a").value();

  Engine engine(engine_config());
  std::uint64_t successful_commits = 0;
  std::uint64_t refused_commits = 0;
  std::uint64_t plans_that_do_not_fit_the_advanced_capacity = 0;

  for (int round = 0; round < kRounds; ++round) {
    const std::uint64_t base_generation = static_cast<std::uint64_t>(2 * round + 1);

    SnapshotSpec base_spec = simple_fabric();
    base_spec.resources[0].usable = 6000;  // res-a, still enough for both demands
    FabricSnapshot base = build_snapshot(base_spec);
    base.capacity.generation = CapacitySnapshotGeneration::parse(base_generation).value();
    const Status base_submitted = engine.submit_snapshot(base);
    TEF_CHECK_MSG(base_submitted.ok(), base_submitted.format());
    const AuthorityVector bound_authority = authority_of(base);
    TEF_CHECK(bound_authority == engine.live_authority());

    const std::string plan_name = "plan-capacity-" + std::to_string(round);
    const Result<Plan> proposed = prepare_plan(engine, plan_name, static_cast<std::uint64_t>(round + 1));
    TEF_CHECK_MSG(proposed.has_value(), proposed.has_value() ? "" : proposed.error().format());
    TEF_CHECK_EQ(proposed.value().state, PlanState::proposed);
    TEF_CHECK(proposed.value().authority == bound_authority);

    SnapshotSpec advanced_spec = simple_fabric();
    advanced_spec.resources[0].usable = 200;  // res-a collapses to a fraction
    FabricSnapshot advanced = build_snapshot(advanced_spec);
    advanced.capacity.generation = CapacitySnapshotGeneration::parse(base_generation + 1).value();

    // Deterministic fixture self-check, before anything races: the allocation
    // planned against the base capacity cannot be mistaken for one that fits
    // the advanced capacity, so binding a plan to the wrong authority is
    // observable rather than invisible.
    TEF_CHECK_MSG(!verify_allocation(advanced, proposed.value().allocation).empty(),
                  "fixture error: the advanced capacity does not discriminate");

    std::latch release(2);
    Status advance_status;
    std::thread advancer([&engine, &advanced, &release, &advance_status]() {
      release.arrive_and_wait();
      advance_status = engine.submit_snapshot(advanced);
    });
    release.arrive_and_wait();
    const Result<Plan> authorized = engine.authorize(proposed.value().ref(), AuthorizeRequest{});
    std::optional<Result<Plan>> committed;
    if (authorized.has_value()) {
      committed = engine.commit(
          proposed.value().ref(), commit_intent("commit-capacity-" + std::to_string(round),
                                                static_cast<std::uint64_t>(round + 1)));
    }
    const std::optional<Plan> state_after_commit = engine.get(proposed.value().ref());
    const AuthorityVector live_after_commit = engine.live_authority();
    advancer.join();
    TEF_CHECK_MSG(advance_status.ok(), advance_status.format());
    TEF_CHECK(engine.live_authority() == authority_of(advanced));

    if (committed.has_value() && committed->has_value()) {
      ++successful_commits;
      const Plan& plan = committed->value();
      TEF_CHECK_EQ(plan.state, PlanState::committed);
      TEF_CHECK(plan.authority == bound_authority);
      TEF_CHECK(state_after_commit.has_value());
      // Racy read: the record is either still committed (the advance had not
      // landed) or already stale (it had). Never anything else.
      TEF_CHECK(state_after_commit->state == PlanState::committed ||
                state_after_commit->state == PlanState::stale);
      if (state_after_commit->state == PlanState::stale) {
        TEF_CHECK_EQ(state_after_commit->applicability, PlanApplicability::stale);
        TEF_CHECK(!compare_authority(bound_authority, live_after_commit).identical());
      }
      TEF_CHECK(state_after_commit->authority == bound_authority);
      const AuthorityDelta delta = compare_authority(bound_authority, live_after_commit);
      TEF_CHECK(delta.regressed.empty());
      if (!delta.identical()) {
        TEF_CHECK_EQ(delta.advanced.size(), std::size_t{1});
        TEF_CHECK_EQ(delta.advanced.at(0), AuthorityField::capacity_snapshot_generation);
      }

      // The allocation must fit the authoritative capacity of the snapshot the
      // plan is bound to - checked field by field, not by trusting the solver.
      const std::map<ResourceId, std::int64_t> available = authoritative_available(base);
      const std::map<ResourceId, std::int64_t> usable = authoritative_usable(base);
      TEF_CHECK(!plan.allocation.resources.empty());
      for (const ResourceUtilization& utilization : plan.allocation.resources) {
        const auto available_it = available.find(utilization.resource);
        const auto usable_it = usable.find(utilization.resource);
        TEF_CHECK(available_it != available.end());
        TEF_CHECK(usable_it != usable.end());
        TEF_CHECK_MSG(utilization.allocated <= available_it->second,
                      "resource " + utilization.resource.str() + " allocates " +
                          std::to_string(utilization.allocated) +
                          " against an authoritative available capacity of " +
                          std::to_string(available_it->second));
        TEF_CHECK_EQ(utilization.usable_capacity, usable_it->second);
      }
      TEF_CHECK_MSG(verify_allocation(base, plan.allocation).empty(),
                    "the committed allocation does not verify against the snapshot it is bound to");

      // The commit did not rewrite the allocation, and that allocation provably
      // does not fit the advanced snapshot: had this plan been bound to the
      // advanced authority, the independent verifier would have rejected it.
      TEF_CHECK_EQ(plan.allocation.digest().hex(), proposed.value().allocation.digest().hex());
      ++plans_that_do_not_fit_the_advanced_capacity;
      TEF_CHECK(!verify_allocation(advanced, plan.allocation).empty());
      const std::map<ResourceId, std::int64_t> advanced_available = authoritative_available(advanced);
      std::int64_t allocated_on_target = 0;
      for (const ResourceUtilization& utilization : plan.allocation.resources) {
        if (utilization.resource == target) allocated_on_target = utilization.allocated;
      }
      const auto advanced_it = advanced_available.find(target);
      TEF_CHECK(advanced_it != advanced_available.end());
      TEF_CHECK_MSG(allocated_on_target > advanced_it->second,
                    "fixture error: the committed allocation fits the collapsed capacity");
    } else {
      ++refused_commits;
      const Error& error = authorized.has_value() ? committed->error() : authorized.error();
      TEF_CHECK_MSG(typed_authority_refusal(error.code()),
                    "unexpected refusal code " + error_text(error.code()) + ": " + error.detail());
      const std::optional<Plan> after = engine.get(proposed.value().ref());
      TEF_CHECK(after.has_value());
      TEF_CHECK_NE(after->state, PlanState::committed);
    }
  }

  const Engine::Stats stats = engine.stats();
  TEF_CHECK_EQ(successful_commits + refused_commits, static_cast<std::uint64_t>(kRounds));
  TEF_CHECK_EQ(stats.plans_created, static_cast<std::uint64_t>(kRounds));
  TEF_CHECK_EQ(stats.plans_committed, successful_commits);
  TEF_CHECK_EQ(plans_that_do_not_fit_the_advanced_capacity, successful_commits);
  std::printf("         [evidence] capacity race: committed=%llu refused=%llu unfittable-after-advance=%llu\n",
              static_cast<unsigned long long>(successful_commits),
              static_cast<unsigned long long>(refused_commits),
              static_cast<unsigned long long>(plans_that_do_not_fit_the_advanced_capacity));
}

// ===========================================================================
// 5. Recovery runs while four client threads use the same engine.
// ===========================================================================
TEF_TEST(restart_recovery_concurrent_with_clients) {
  constexpr std::size_t kClients = 4;
  constexpr std::size_t kIterations = 24;

  const std::string directory = temporary_directory("recovery-concurrency");
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const PlanId durable_id = PlanId::parse("plan-durable").value();
  const PlanId inflight_id = PlanId::parse("plan-inflight").value();
  const PlanId client_id = PlanId::parse("plan-client").value();

  PlanRepository::Options options;
  options.directory = directory;

  // ---- Durable history: eight records, two plan identities.
  {
    Engine engine(engine_config());
    Result<std::unique_ptr<PlanRepository>> opened = PlanRepository::open(options);
    TEF_CHECK_MSG(opened.has_value(), opened.has_value() ? "" : opened.error().format());
    std::shared_ptr<PlanRepository> repository(std::move(opened.value()));
    TEF_CHECK(engine.attach_repository(repository).ok());
    TEF_CHECK(engine.submit_snapshot(snapshot).ok());

    const Result<Plan> durable = prepare_plan(engine, "plan-durable", 1);
    TEF_CHECK_MSG(durable.has_value(), durable.has_value() ? "" : durable.error().format());
    TEF_CHECK(engine.authorize(durable.value().ref(), AuthorizeRequest{}).has_value());
    TEF_CHECK(engine.commit(durable.value().ref(), commit_intent("commit-durable", 1)).has_value());

    const Result<Plan> inflight = prepare_plan(engine, "plan-inflight", 2);
    TEF_CHECK_MSG(inflight.has_value(), inflight.has_value() ? "" : inflight.error().format());
    TEF_CHECK_EQ(inflight.value().state, PlanState::proposed);
    TEF_CHECK_EQ(repository->record_count(), std::size_t{8});
    TEF_CHECK(repository->close().ok());
  }

  // ---- Reopen, and race recovery against live clients.
  Result<std::unique_ptr<PlanRepository>> reopened = PlanRepository::open(options);
  TEF_CHECK_MSG(reopened.has_value(), reopened.has_value() ? "" : reopened.error().format());
  std::shared_ptr<PlanRepository> repository(std::move(reopened.value()));
  TEF_CHECK_EQ(repository->recovery().records_recovered, std::uint64_t{8});

  Engine engine(engine_config());
  TEF_CHECK(engine.attach_repository(repository).ok());

  struct ClientObservation {
    std::size_t submits = 0;
    std::size_t declares = 0;
    std::size_t invalid_authorities = 0;
    std::size_t untyped_declare_refusals = 0;
  };
  std::vector<ClientObservation> observations(kClients);
  std::latch release(kClients + 1);
  std::vector<std::thread> clients;
  clients.reserve(kClients);
  for (std::size_t i = 0; i < kClients; ++i) {
    clients.emplace_back([&engine, &snapshot, &observations, &release, i, durable_id, inflight_id,
                          client_id]() {
      ClientObservation& observation = observations[i];
      release.arrive_and_wait();
      for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        if (engine.submit_snapshot(snapshot).ok()) ++observation.submits;
        // A client reads current state before it offers work, exactly as a real
        // publisher does.
        (void)engine.list(PlanState::committed);
        (void)engine.live_authority();
        const Result<Plan> declared = engine.declare(declare_request("plan-client", i + 1));
        if (declared.has_value()) {
          ++observation.declares;
          if (engine.validate(declared.value().ref()).ok()) {
            (void)engine.solve(declared.value().ref());
          }
        } else {
          const ErrorCode code = declared.error().code();
          if (code != ErrorCode::stale_authority && code != ErrorCode::duplicate_identity) {
            ++observation.untyped_declare_refusals;
          }
        }
        // Read every plan identity the engine may hold, and every lifecycle
        // list: a partially recovered plan must never be observable.
        for (const PlanId& id : {durable_id, inflight_id, client_id}) {
          const std::optional<Plan> plan = engine.get(id);
          if (plan.has_value() && !plan->authority.valid()) ++observation.invalid_authorities;
        }
        for (const PlanState state : kAllPlanStates) {
          for (const Plan& plan : engine.list(state)) {
            if (!plan.authority.valid()) ++observation.invalid_authorities;
          }
        }
        (void)engine.live_authority();
        (void)engine.snapshot_copy();
      }
    });
  }

  // Both outcomes of this race are correct engine behaviour and both are
  // checked: recovery either gets the engine first (the plan map was still empty
  // when it ran, so its report is asserted exactly), or a client declaration
  // reaches the plan map first and recovery is refused with the documented
  // precondition error instead of half-recovering a live engine.
  release.arrive_and_wait();
  const Status recovered = engine.recover_from_repository();
  for (std::thread& client : clients) client.join();

  std::size_t invalid_authorities = 0;
  std::size_t untyped_refusals = 0;
  std::size_t submits = 0;
  std::size_t declares = 0;
  for (const ClientObservation& observation : observations) {
    invalid_authorities += observation.invalid_authorities;
    untyped_refusals += observation.untyped_declare_refusals;
    submits += observation.submits;
    declares += observation.declares;
    TEF_CHECK_EQ(observation.submits, kIterations);
  }
  TEF_CHECK_EQ(invalid_authorities, std::size_t{0});
  TEF_CHECK_EQ(untyped_refusals, std::size_t{0});
  TEF_CHECK_EQ(submits, kClients * kIterations);
  // The same plan identity was offered by every client thread; exactly the
  // clients that lost that race see duplicate_identity, and every refusal is
  // typed (asserted above), so at least one declaration must have landed.
  TEF_CHECK(declares >= 1);

  if (recovered.ok()) {
    const RecoverReport report = engine.recovery_report();
    TEF_CHECK_EQ(report.records_read, std::uint64_t{8});
    TEF_CHECK_EQ(report.plans_restored, std::uint64_t{2});
    TEF_CHECK_EQ(report.plans_marked_revalidation_required, std::uint64_t{1});
    TEF_CHECK_EQ(report.plans_marked_stale, std::uint64_t{1});
    TEF_CHECK_EQ(report.plans_skipped, std::uint64_t{0});

    const std::optional<Plan> durable = engine.get(durable_id);
    TEF_CHECK(durable.has_value());
    TEF_CHECK(durable->authority.valid());
    TEF_CHECK_EQ(durable->state, PlanState::committed);
    TEF_CHECK_EQ(durable->applicability, PlanApplicability::revalidation_required);
    const std::optional<Plan> inflight = engine.get(inflight_id);
    TEF_CHECK(inflight.has_value());
    TEF_CHECK(inflight->authority.valid());
    TEF_CHECK_EQ(inflight->state, PlanState::stale);
    TEF_CHECK_EQ(inflight->applicability, PlanApplicability::stale);
  } else {
    // A client declaration won the plan-map race, so recovery correctly refused
    // to run on a non-empty engine; that is a typed precondition refusal, not
    // corruption.
    TEF_CHECK_EQ(recovered.error().code(), ErrorCode::invalid_argument);
    Engine verifier(engine_config());
    TEF_CHECK(verifier.attach_repository(repository).ok());
    const Status verified = verifier.recover_from_repository();
    TEF_CHECK_MSG(verified.ok(), verified.format());
    const RecoverReport second = verifier.recovery_report();
    TEF_CHECK_EQ(second.records_read, static_cast<std::uint64_t>(repository->record_count()));
    TEF_CHECK_EQ(second.plans_skipped, std::uint64_t{0});
    TEF_CHECK_EQ(second.plans_restored,
                  second.plans_marked_revalidation_required + second.plans_marked_stale);
    TEF_CHECK_LE(second.plans_restored, std::uint64_t{3});
    for (const PlanState state : kAllPlanStates) {
      for (const Plan& plan : verifier.list(state)) {
        TEF_CHECK(plan.authority.valid());
      }
    }
  }
  std::printf("         [evidence] recovery %s with %zu submissions and %zu plan declarations\n",
              recovered.ok() ? "ran concurrently" : "was refused by a racing declare", submits, declares);
}

// ===========================================================================
// 6. Shutdown with idle and mid-protocol sessions attached.
// ===========================================================================
TEF_TEST(shutdown_with_idle_and_active_sessions) {
  constexpr std::size_t kClientCount = 5;
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());

  CoordinatorConfig config;
  config.bind_address = "127.0.0.1";
  config.port = 0;                 // ephemeral port
  config.data_directory.clear();   // in-memory: no durable state at all
  config.max_worker_threads = 8;

  Result<std::unique_ptr<Coordinator>> created = Coordinator::create(config);
  TEF_CHECK_MSG(created.has_value(), created.has_value() ? "" : created.error().format());
  std::unique_ptr<Coordinator> coordinator = std::move(created.value());
  const Status started = coordinator->start();
  TEF_CHECK_MSG(started.ok(), started.format());
  const std::uint16_t port = coordinator->port();
  TEF_CHECK_NE(port, std::uint16_t{0});

  std::vector<std::unique_ptr<PublisherClient>> clients;
  clients.reserve(kClientCount);
  for (std::size_t i = 0; i < kClientCount; ++i) {
    PublisherConfig publisher_config;
    publisher_config.address = "127.0.0.1";
    publisher_config.port = port;
    publisher_config.publisher = PublisherId::parse("publisher-" + std::to_string(i + 1)).value();
    publisher_config.boot = derive_boot_id(2026, i + 1);
    publisher_config.description = "concurrent shutdown client";
    std::string transport_error;
    Result<std::unique_ptr<PublisherClient>> client =
        PublisherClient::connect(publisher_config, transport_error);
    TEF_CHECK_MSG(client.has_value(), "connect failed: " + transport_error);
    clients.push_back(std::move(client.value()));
  }
  for (std::unique_ptr<PublisherClient>& client : clients) {
    const Status registered = client->register_publisher();
    TEF_CHECK_MSG(registered.ok(), registered.format());
    TEF_CHECK_NE(client->coordinator_epoch(), std::uint64_t{0});
  }
  const Coordinator::Stats before = coordinator->stats();
  TEF_CHECK(before.connections_accepted >= kClientCount);
  TEF_CHECK(before.frames_read > 0);
  TEF_CHECK_EQ(before.active_connections, static_cast<std::uint64_t>(kClientCount));

  // Client 1 and 2 stay idle. Client 3 has a real plan request in flight while
  // stop() runs. Client 4 is mid-handshake (hello sent, response unread).
  // Client 5 has an unsolicited ping frame its worker has already answered.
  std::latch release(2);
  std::optional<Result<PlanResponse>> mid_protocol_result;
  std::thread mid_protocol([&]() {
    release.arrive_and_wait();
    mid_protocol_result = clients[2]->submit_plan(snapshot, declare_request("plan-midprotocol", 1));
  });
  {
    std::string error;
    Frame hello;
    hello.version = static_cast<std::uint16_t>(kWireProtocolVersion);
    hello.type = static_cast<std::uint16_t>(MessageType::hello_request);
    HelloRequest request;
    request.protocol_version = static_cast<std::uint16_t>(kWireProtocolVersion);
    hello.payload = encode(request);
    TEF_CHECK_MSG(clients[3]->send_raw(hello, error), error);

    Frame ping;
    ping.version = static_cast<std::uint16_t>(kWireProtocolVersion);
    ping.type = static_cast<std::uint16_t>(MessageType::ping);
    TEF_CHECK_MSG(clients[4]->send_raw(ping, error), error);
  }

  release.arrive_and_wait();
  const Status first_stop = coordinator->stop();
  TEF_CHECK_MSG(first_stop.ok(), first_stop.format());
  mid_protocol.join();

  // The in-flight exchange returned: either the request was answered before the
  // session was torn down, or the session refused it with an explicit shutdown.
  // Either way the client never blocks and never sees an untyped failure.
  TEF_CHECK(mid_protocol_result.has_value());
  if (mid_protocol_result->has_value()) {
    const ErrorCode code = mid_protocol_result->value().code;
    TEF_CHECK_MSG(code == ErrorCode::ok || code == ErrorCode::shutdown,
                  "unexpected in-flight plan response code " + error_text(code));
  }

  const Coordinator::Stats after = coordinator->stats();
  TEF_CHECK_EQ(after.active_connections, std::uint64_t{0});

  // A second stop() is a no-op: it returns, changes nothing, and leaves the
  // coordinator in the same state.
  const Status second_stop = coordinator->stop();
  TEF_CHECK_MSG(second_stop.ok(), second_stop.format());
  const Coordinator::Stats after_second = coordinator->stats();
  TEF_CHECK_EQ(after_second.active_connections, std::uint64_t{0});
  TEF_CHECK_EQ(after_second.connections_accepted, after.connections_accepted);
  TEF_CHECK_EQ(after_second.frames_read, after.frames_read);
  TEF_CHECK_EQ(after_second.mutations_accepted, after.mutations_accepted);

  // The sessions that were live when stop() was called are gone: a torn-down
  // connection can no longer be served.
  const Result<QueryResponse> late = clients[0]->query(PlanRef{}, false);
  TEF_CHECK_MSG(!late.has_value(), "a stopped coordinator served a new request");
  if (!late.has_value()) TEF_CHECK_EQ(late.error().code(), ErrorCode::transport_failure);

  // Every client object is destroyed before the test returns.
  clients.clear();
  std::printf("         [evidence] coordinated shutdown: accepted=%llu frames_read=%llu mutations=%llu\n",
              static_cast<unsigned long long>(after.connections_accepted),
              static_cast<unsigned long long>(after.frames_read),
              static_cast<unsigned long long>(after.mutations_accepted));
}

// ===========================================================================
// 7. Repository appends from eight threads are serialised and chained.
// ===========================================================================
TEF_TEST(repository_appends_from_many_threads_are_serialised) {
  constexpr std::size_t kThreads = 8;
  constexpr std::size_t kPerThread = 64;
  constexpr std::size_t kTotal = kThreads * kPerThread;

  const std::string directory = temporary_directory("repository-concurrency");
  PlanRepository::Options options;
  options.directory = directory;

  Result<std::unique_ptr<PlanRepository>> opened = PlanRepository::open(options);
  TEF_CHECK_MSG(opened.has_value(), opened.has_value() ? "" : opened.error().format());
  std::unique_ptr<PlanRepository> repository = std::move(opened.value());
  TEF_CHECK_EQ(repository->record_count(), std::size_t{0});

  std::vector<std::size_t> appended(kThreads, 0);
  std::latch release(kThreads);
  std::vector<std::thread> writers;
  writers.reserve(kThreads);
  for (std::size_t i = 0; i < kThreads; ++i) {
    writers.emplace_back([&repository, &appended, &release, i]() {
      release.arrive_and_wait();
      for (std::size_t m = 0; m < kPerThread; ++m) {
        AuditRecord record;
        record.tick = static_cast<std::uint64_t>(i * kPerThread + m);
        record.event = "concurrent-append";
        record.detail = "thread " + std::to_string(i) + " record " + std::to_string(m);
        record.subject = Sha256::hash(record.detail);
        if (repository->append(RecordType::audit, encode_audit(record)).ok()) ++appended[i];
      }
    });
  }
  for (std::thread& writer : writers) writer.join();

  std::size_t accepted = 0;
  for (const std::size_t count : appended) accepted += count;
  TEF_CHECK_EQ(accepted, kTotal);
  TEF_CHECK_EQ(repository->record_count(), kTotal);
  TEF_CHECK_EQ(repository->last_sequence(), static_cast<std::uint64_t>(kTotal));

  const std::vector<DurableRecord> records = repository->records();
  TEF_CHECK_EQ(records.size(), kTotal);
  const Digest zero;
  for (std::size_t i = 0; i < records.size(); ++i) {
    TEF_CHECK_EQ(records[i].sequence, static_cast<std::uint64_t>(i + 1));
    const Digest& expected_previous = i == 0 ? zero : records[i - 1].record_digest;
    TEF_CHECK_MSG(records[i].previous_digest == expected_previous,
                  "the durable hash chain is broken at sequence " + std::to_string(records[i].sequence));
    TEF_CHECK(records[i].type == RecordType::audit);
    TEF_CHECK(!records[i].record_digest.is_zero());
    const Result<AuditRecord> decoded = decode_audit(records[i].payload);
    TEF_CHECK_MSG(decoded.has_value(), "record " + std::to_string(records[i].sequence) + " does not decode");
  }

  TEF_CHECK(repository->close().ok());
  Result<std::unique_ptr<PlanRepository>> reopened = PlanRepository::open(options);
  TEF_CHECK_MSG(reopened.has_value(), reopened.has_value() ? "" : reopened.error().format());
  std::unique_ptr<PlanRepository> recovered = std::move(reopened.value());
  TEF_CHECK_EQ(recovered->recovery().records_recovered, static_cast<std::uint64_t>(kTotal));
  TEF_CHECK_EQ(recovered->recovery().records_discarded, std::uint64_t{0});
  TEF_CHECK_EQ(recovered->record_count(), kTotal);
  const std::vector<DurableRecord> recovered_records = recovered->records();
  TEF_CHECK_EQ(recovered_records.size(), kTotal);
  for (std::size_t i = 0; i < recovered_records.size(); ++i) {
    TEF_CHECK_EQ(recovered_records[i].sequence, static_cast<std::uint64_t>(i + 1));
    const Digest& expected_previous = i == 0 ? zero : recovered_records[i - 1].record_digest;
    TEF_CHECK(recovered_records[i].previous_digest == expected_previous);
    TEF_CHECK(recovered_records[i].record_digest == records[i].record_digest);
  }
  TEF_CHECK(recovered->close().ok());
}

int main(int argc, char** argv) { return tef::test::run_all(argc, argv); }
