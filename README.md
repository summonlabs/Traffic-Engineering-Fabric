# Traffic Engineering Fabric

**Vendor-neutral global traffic-engineering intent runtime.** C++20, no external dependencies, no
commercial solver.

Traffic Engineering Fabric (TEF) computes a deterministic, policy-bounded **global traffic
allocation** across an exact authoritative fabric snapshot when many demands, candidate paths,
capacities, reservations, failure domains and policy constraints interact. It explains why that
allocation was selected, records it durably, and refuses to let it become authoritative once the
inputs it was computed from are no longer current.

TEF emits **desired allocation intent**. It never installs routes, never mutates path weights, never
programs a device, and never claims to know what is actually being forwarded.

* Version: 1.0.0
* License: Apache License 2.0
* Documentation: @docs/systems-boundary.md@, @docs/algorithm.md@, @docs/persistence.md@,
  @docs/concurrency.md@, @docs/validation.md@

```
Fabric Topology ---.
Link State Fabric -.
Path Authority ----.                        +-----------------------------+
Path Planner ------.   authoritative        |  derive()  -> residuals     |
Reservation Fabric -.  inputs               |  solve()   -> allocation    |
Admission Fabric --'                        |  verify_allocation()        |
                                            +--------------+--------------+
                                                           |  Allocation (desired intent)
                                                           v
                                            +-----------------------------+
                                            |  Plan lifecycle             |
                                            |  DECLARED ... COMMITTED     |
                                            +--------------+--------------+
                                                           |  committed plan (durable)
                                                           v
                                     Route Fabric / Weighted Path Fabric / executors
```

## Systems boundary

TEF owns **global traffic-engineering intent over already-authorized fabric resources**. Everything
else belongs to an adjacent runtime, and the repository is built so that this runtime physically
cannot do those jobs:

| Not owned by TEF | Owning system |
| --- | --- |
| fabric identity, topology discovery, topology truth | Fabric Topology |
| link-state truth | Link State Fabric |
| path legality | Path Authority |
| candidate-path computation | Path Planner |
| route lifecycle and route installation | Route Fabric |
| ECMP membership | ECMP Governor |
| low-level path-weight mutation | Weighted Path Fabric |
| adaptive evidence-driven local preference change | Adaptive Routing Fabric |
| route-convergence sequencing | Route Convergence |
| bandwidth reservation lifecycle | Bandwidth Broker / Bandwidth Reservation Fabric |
| admission | Network Admission Fabric |
| flow scheduling | Flow Scheduler |
| packet forwarding, queue control, congestion control | congestion runtimes and forwarding hardware |
| telemetry collection | telemetry systems |
| device configuration and switch programming | device/control-plane executors |

TEF never derives a path: candidate paths arrive with an explicit Path Authority generation, and an
allocation that references a path outside the submitted candidate set is rejected by the independent
verifier. TEF never treats capacity as a property of the world: capacity is usable only under the
exact `CapacitySnapshot` identity and generation that produced it. Full detail:
@docs/systems-boundary.md@.

### Why global traffic engineering differs from the systems around it

Route planning, path authority, weighting, adaptive routing, convergence, reservation lifecycle,
admission and scheduling are all **local** decisions: each reasons about one demand, one path, one
weight or one link at a time. Global traffic engineering is a different problem because demands
interact:

* two demands that are individually satisfiable can jointly oversubscribe a shared resource;
* two individually optimal path choices can be jointly worse than two jointly chosen ones;
* a locally beneficial change can move bandwidth across many demands and failure domains at once,
  which is an operational event rather than a routing detail.

TEF therefore optimises all admitted demands together against a single snapshot, accounts every
resource once across all demands, re-derives that accounting independently before publishing, and
separates **hard constraints** from **soft objectives** so that an infeasible state can never be
reported as merely suboptimal.

**Adaptive Routing Fabric** governs evidence-driven adaptation of local preference among legal
candidates at its own boundary. TEF governs the global allocation objective across interacting
demands. TEF has no per-path preference signal, no adaptation loop and no local feedback path, and
its churn control exists precisely so that a globally better solution does not silently become a
local preference change.

### Desired allocation versus applied forwarding state

`Allocation::granted` is **desired intent**. TEF never infers forwarding state from its own output.
The optional `observed_applied` / `applied_known` fields on a demand allocation exist only so an
external observer can record what is actually installed; this runtime never fills them in. A
committed plan is a governed statement of intent, not a route.

## Core principles

1. **Global optimisation is not authority.** A mathematically attractive allocation cannot become
   authoritative unless every bound topology, path, capacity, policy, reservation, demand and
   control-plane generation remains current.
2. **Legal path sets are inputs.** Path legality is never invented.
3. **Traffic engineering is multi-demand.** Interacting demands are solved together, never one at a
   time.
4. **Capacity is explicit and generation-bound.**
5. **Reservations are obligations, not hints.** A reservation is displaced only when policy permits
   preemption *and* the reserving authority marked it preemptible.
6. **Hard constraints and soft objectives are distinct types**, so infeasible never looks like
   suboptimal.
7. **The output is explainable**: machine-readable and human-readable explanations name the binding
   constraints, saturated resources, policy exclusions, rejected alternatives, objective components,
   churn delta and the exact authority vector.
8. **Determinism is mandatory.** Equivalent canonical inputs produce identical allocations,
   orderings, explanations and digests.
9. **No hidden fallback.** If no valid allocation can be produced, the result is an explicit typed
   outcome. Traffic is never quietly moved onto a default path.
10. **Recompute does not imply commit.** Planning, validation, proposal, authorization, commit,
    supersession and application are separate stages.

## Authority vector and stale-plan semantics

Every plan is bound to an `AuthorityVector` containing the fabric epoch, topology generation,
link-state generation, path-authority generation, failure-domain generation, and the exact
identity **and** generation of the capacity snapshot, reservation snapshot, policy, objective profile
and candidate set - plus content digests of the demand set, candidate set, resource catalog,
reservation set, policy and objective profile.

Before authorization, and again immediately before commit, that vector is re-compared against the
live authority:

* **advanced** - the live input is strictly newer. The plan becomes `STALE` and can never mutate
  authoritative state. If a plan was already committed, it keeps its durable history but its
  applicability becomes stale.
* **regressed or conflicting** - the live input is older, or carries the same generation with
  different content. The result is `CONFLICTING_INPUT`, never a silent reinterpretation.

A snapshot advance also demotes every plan that was still advancing toward commit, so a plan can
never be authorized against inputs that have moved on.

## Objectives and constraints

Hard constraints are typed `ConstraintKind` values: resource capacity, policy utilisation cap,
demand minimum and maximum, path eligibility, forbidden paths, path-authority generation,
reservation obligations, failure-domain diversity, affinity/anti-affinity, tenant and service-class
policy, latency bounds, path-cost ceilings and path-count limits.

Soft objectives are weighted `ObjectiveTerm` values, combined lexicographically in the order the
profile declares them: `satisfy_minimums`, `minimize_max_utilization`,
`minimize_congestion_exposure`, `minimize_total_path_cost`, `minimize_churn`,
`preserve_reservations`, `preserve_priority`, `maximize_desired_bandwidth`,
`fairness_across_groups`, `minimize_failure_domain_concentration`, `minimize_path_count`.

The algorithm is documented in full in @docs/algorithm.md@: deterministic progressive allocation with
a **max-flow min-cut infeasibility certificate**, bounded deterministic repair, and a bounded
parametric search for the tightest utilisation ceiling. There is no third-party solver.

Typed outcomes: `FEASIBLE`, `FEASIBLE_DEGRADED`, `INFEASIBLE_CAPACITY`, `INFEASIBLE_POLICY`,
`INFEASIBLE_PATH_SET`, `INFEASIBLE_RESERVATION_CONFLICT`, `STALE_INPUT`,
`CONFLICTING_INPUT`, `UNSUPPORTED_OBJECTIVE` and `SOLVER_LIMIT_REACHED`. The last one is the
honest "neither accepted nor proven infeasible" answer; it is never reported as success.

## Churn control

A globally better solution does not automatically replace a stable incumbent. `compare_churn`
compares a proposal against an incumbent on the same objective, computed the same way, and applies
policy gates in a fixed order: required improvement in permille, moved bandwidth in permille,
affected demand count and a documented deterministic operational-risk composite. The decision is
`NO_INCUMBENT`, `ACCEPT`, `HOLD_INCUMBENT` or `REJECT_REGRESSION`; `Engine::authorize`
refuses a plan whose decision is `HOLD_INCUMBENT` or `REJECT_REGRESSION` with the typed code
`CHURN_BOUND`, and the rationale names every failing gate.

## Plan lifecycle

```
DECLARED -> VALIDATED -> SOLVING -> PROPOSED -> AUTHORIZED -> COMMITTED
                                \-> DEGRADED  /           \-> SUPERSEDED
   any live state -> REJECTED or STALE                  COMMITTED -> RETIRED
```

Transitions are checked against an explicit table; every other transition is refused with
`INVALID_TRANSITION`. A duplicate commit frame carrying the same commit identity and generation is
an idempotent replay; any other repeat is refused with `ALREADY_COMMITTED`. Supersession lineage is
cycle-checked. Applicability (`CURRENT`, `REVALIDATION_REQUIRED`, `STALE`, `SUPERSEDED`,
`RETIRED`) is tracked separately from lifecycle state, because a committed plan is a durable
historical fact while its applicability to the current fabric is a judgement that must be
re-established.

## Persistence and recovery

A versioned, integrity-checked durable log: a manifest, an atomically-replaced snapshot, and an
append-only journal. Every record carries a format version, a monotonic sequence number, the digest
of the previous record (an unbroken hash chain), a CRC-32C over its payload, and a SHA-256 over the
whole frame. Only state that belongs to this runtime is persisted: committed plans, supersession
lineage, provenance and audit records, policies and objective profiles, fencing records and
coordinator epoch state.

Dynamic evidence and liveness are never persisted and never restored: capacity, reservations, path
authority results, candidate sets, demand sets, publisher registrations, leases and sessions must all
be re-submitted by their owner after a restart. A restored plan reopens as
`REVALIDATION_REQUIRED` and only becomes `CURRENT` after the exact authority it was bound to is
submitted again. A plan that was still in flight when the process died is reopened as `STALE`,
never resumed. Details and the crash-window table: @docs/persistence.md@.

## Distributed runtime

An optional coordinator and publisher runtime over **real TCP** with a framed, CRC-checked protocol:

* publisher identity is bound at the protocol level to a publisher **boot identity**; every mutation
  carries publisher, boot, coordinator incarnation, coordinator epoch and attempt;
* a replaced boot identity is fenced **permanently**, and fencing is durable;
* the coordinator epoch is persisted and advanced on every start, so frames from a previous
  coordinator incarnation are rejected before they can touch state;
* liveness is not durable: a restarted coordinator requires every publisher to re-register under the
  new epoch.

## Build, test, install

Requirements: CMake 3.20+, a C++20 compiler, and threads. No third-party dependency is used or
downloaded. Validated with MSVC 19.44 (Visual Studio 2022 Build Tools) on Windows and with GCC/Clang
on POSIX; the transport has both a Winsock and a BSD-socket implementation.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && ctest --output-on-failure
```

Options: @TEF_BUILD_TESTS@, @TEF_BUILD_BENCHMARKS@, @TEF_BUILD_TOOLS@ (all default @ON@),
@TEF_WARNINGS_AS_ERRORS@ (default @ON@) and @TEF_ENABLE_SANITIZERS@ (default @OFF@).

Install and consume the exported package:

```sh
cmake --install build --prefix /some/prefix

# consumer/CMakeLists.txt
find_package(TrafficEngineeringFabric 1.0 REQUIRED)
target_link_libraries(my_app PRIVATE TrafficEngineeringFabric::tef)
```

@consumer/@ in this repository is exactly such an independent consumer; it is configured, built and
run against the installed package as part of release validation.

## Using the library

```cpp
#include "tef/tef.hpp"

tef::Engine engine;                       // EngineConfig::coordinator_incarnation defaults to first()
engine.submit_snapshot(authoritative_snapshot);   // validated and canonicalized

tef::DeclareRequest declare;
declare.plan_id = tef::PlanId::parse("plan-1").value();
declare.attempt = tef::AttemptId::parse("attempt-1").value();
declare.attempt_generation = tef::AttemptGeneration::first();
declare.publisher = tef::PublisherId::parse("publisher-1").value();
declare.publisher_boot = tef::derive_boot_id(1, 1);

auto declared = engine.declare(declare);
engine.validate(declared.value().ref());
auto proposed = engine.solve(declared.value().ref());
engine.authorize(proposed.value().ref(), tef::AuthorizeRequest{});

tef::CommitIntent intent;
intent.commit = tef::CommitId::parse("commit-1").value();
intent.commit_generation = tef::CommitGeneration::first();
auto committed = engine.commit(proposed.value().ref(), intent);

auto explanation = engine.explain(proposed.value().ref());
std::printf("%s", explanation->to_text().c_str());
std::printf("%s", explanation->to_json().c_str());
```

A complete runnable version is @examples/example_plan.cpp@. Planning without the lifecycle is simply
`tef::solve(snapshot, options)`; scoring an existing allocation against the active objective is
`tef::score_allocation(...)`; re-checking an allocation without trusting the solver is
`tef::verify_allocation(snapshot, allocation)`.

### Public API surface

| Area | Entry points |
| --- | --- |
| demands, paths, snapshots | @tef/model.hpp@: `canonicalize`, `validate_structure`, `encode_snapshot`, `decode_snapshot`, the collection digests |
| planning | @tef/solver.hpp@: `solve`, `score_allocation`; @tef/derived.hpp@: `derive` |
| verification | @tef/allocation.hpp@: `verify_allocation`, `compare_churn` |
| lifecycle | @tef/engine.hpp@: `Engine` (declare, validate, solve, authorize, commit, supersede, mark_stale, retire, revalidate, recover_from_repository, queries) |
| explanations | @tef/explain.hpp@: `build_explanation`, `Explanation::to_json`, `Explanation::to_text` |
| authority | @tef/authority.hpp@: `authority_of`, `compare_authority` |
| persistence | @tef/persist.hpp@: `PlanRepository` and the record codecs |
| distributed | @tef/coordinator.hpp@, @tef/publisher.hpp@, @tef/protocol.hpp@, @tef/net.hpp@ |
| tooling | @tef/inspect.hpp@ renderers |

## Tools

```sh
tef-cli demo                                   # end-to-end synthetic planning demonstration
tef-cli plan --demands=64 --paths-per-demand=8 --resources=64 --seed=7
tef-cli compare --incumbent-runs=2 --seed=11    # churn comparison against an incumbent
tef-cli inspect --state-dir DIR                 # deterministic report over durable state
tef-cli selfcheck                               # in-process end-to-end assertions

tef-node coordinator --info-file PATH [--state-dir DIR]
tef-node publisher --address A --port P --publisher ID --boot HI:LO --scenario NAME --out PATH
```

## Benchmarks

`benchmarks/tef_bench.cpp` measures **completed planning work** over clearly labelled **synthetic**
populations, sweeping demand count, candidate paths per demand, shared resources, reservation
density, objective complexity and churn comparison. It times only `tef::solve` and reports snapshot
construction separately; it never reports enqueue or submission latency, and it re-solves every
configuration to assert that the status and allocation digest are identical each time.

```sh
./build/bin/tef_bench --quick      # small sweep
./build/bin/tef_bench --full       # default sweep
```

All numbers it prints are synthetic. They are **not** physical-network throughput and must not be
read as such.

## Validation: REAL vs SYNTHETIC

**REAL** (genuine OS, filesystem and network behaviour): real TCP sockets; real independent OS
processes launched from the shipped `tef-node` binary; real @TerminateProcess` / `SIGKILL` of a
live coordinator and a mid-protocol publisher; coordinator restart with epoch advancement and stale
epoch/boot replay rejection; real close/reopen of durable state; torn, truncated, corrupted and
garbage durable files; real threads with latch/barrier release and real joins; `cmake --install`
plus an independent `find_package` consumer.

**SYNTHETIC**: every allocation, feasibility, capacity-accounting, reservation, staleness, churn and
throughput claim is exercised on labelled synthetic populations generated by a documented splitmix64
PRNG, so any seed reproduces a fabric exactly.

**UNSUPPORTED**: no physical multi-switch, multi-rack, multi-site, RDMA, optical, NIC, DPU or
hardware-forwarding validation of any kind was performed, and no GPU or accelerator test is present
because none is materially relevant to this boundary. No third-party solver is used.

See @docs/validation.md@ for the full inventory.

## Limitations

* The built-in solver always respects every hard constraint and proves infeasibility when the
  min-cut relaxation proves it, but it is **not an exact LP solver**: it is not guaranteed to
  construct an allocation merely because one exists. Such a case is reported as
  `SOLVER_LIMIT_REACHED` (or `FEASIBLE_DEGRADED` when policy permits) - never as success and
  never as proof of infeasibility.
* Path variables are whole-path: a rate placed on a path consumes that rate from every resource on
  the path atomically. The certification relaxation does not preserve that atomicity, which is why a
  feasible certificate is never treated as an authorization on its own.
* `SolveOptions::max_iterations` bounds the deterministic search at loop granularity, not per
  micro-step; it is a safety bound, not an exact operation count.
* Population sizes above `tef::Limits` are rejected with `LIMIT_EXCEEDED` rather than attempted.
* Time enters only through `FabricSnapshot::evaluation_tick` and effective intervals; the solver
  never reads a clock, so a caller that wants wall-clock scheduling must supply the tick.
* The runtime is a library plus a small coordinator/publisher protocol. It is not a control plane: it
  does not install anything, and it deliberately has no configuration-file format or daemon
  supervisor.
* `observed_applied` is an input slot for an external observer. This repository does not include
  that observer, so an installed-state comparison is available to a consumer but is not produced
  here.

## Repository layout

```
include/tef/     public headers (the library's entire public surface)
src/             library implementation
tests/           proof obligations: unit, model, solver, invariant/adversarial, engine,
                 persistence, concurrency, distributed, multiprocess
benchmarks/      synthetic planning benchmark
tools/           tef-node (cluster process), tef-cli (inspection)
examples/        runnable planning example
consumer/        independent installed-package consumer project
docs/            systems boundary, algorithm, persistence, concurrency, validation
cmake/           package configuration template
```

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
