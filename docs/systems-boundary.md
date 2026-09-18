# Systems boundary

Traffic Engineering Fabric (TEF) owns **global traffic-engineering intent** over already-authorized
fabric resources. Given an exact authoritative fabric snapshot and a set of admitted demands, it
decides what the *global* traffic allocation should be, explains why, and records that decision
durably.

TEF answers exactly one question:

> Given an exact authoritative fabric snapshot, a set of admitted traffic demands, legal candidate
> paths, capacities, reservations, service constraints, policy and current generations, what global
> traffic-allocation intent should exist now, why is that allocation selected, what constraints are
> binding, and when must the result be rejected, degraded, superseded, fenced, or recomputed as
> stale?

## What TEF does not own

TEF never performs any of the following, and no API in this repository can perform them:

| Responsibility | Owning system |
| --- | --- |
| fabric identity | Fabric Topology |
| topology discovery and topology truth | Fabric Topology |
| link-state truth | Link State Fabric |
| path legality | Path Authority |
| candidate-path computation | Path Planner |
| route lifecycle and route installation | Route Fabric |
| ECMP membership | ECMP Governor |
| low-level path-weight mutation semantics | Weighted Path Fabric |
| adaptive evidence-driven local preference change | Adaptive Routing Fabric |
| route-convergence sequencing | Route Convergence |
| bandwidth reservation lifecycle | Bandwidth Broker / Bandwidth Reservation Fabric |
| admission | Network Admission Fabric |
| flow scheduling | Flow Scheduler |
| packet forwarding | congestion runtimes and forwarding hardware |
| telemetry collection | telemetry systems |
| queue control | queue-management systems |
| congestion-control algorithms | congestion-control runtimes |
| device configuration and switch programming | device/control-plane executors |

Concretely:

* TEF never derives a path. `CandidatePath` values arrive with an explicit Path Authority
  generation; if a path is not in the submitted candidate set it cannot be used, and
  `verify_allocation` rejects an allocation that references one.
* TEF never treats capacity as a property of the world. Capacity arrives inside a
  `CapacitySnapshot` with its own snapshot identity, generation and fabric epoch, and is usable
  only under that exact evidence.
* TEF never installs anything. `Allocation::granted` is desired intent. The optional
  `observed_applied` / `applied_known` fields are only ever populated by an external observer;
  TEF never infers forwarding state from its own output.
* TEF never admits a demand. A `Demand` is already-admitted input whose provenance names the
  admitting system.
* TEF never creates, renews, or releases a reservation. Reservations are consumed as obligations.

## Why global traffic engineering is a distinct problem

Route planning, path authority, weighting, adaptive routing, convergence, reservation lifecycle,
admission and scheduling are all *local* decisions: each one reasons about one demand, one path, one
weight, or one link at a time. A globally attractive allocation is a different object, because
demands interact:

* Two demands that look individually satisfiable can jointly oversubscribe a shared resource. Only a
  multi-demand view sees this.
* Two individually optimal path selections can be globally worse than two jointly chosen ones.
* A change that is locally beneficial can move bandwidth across many demands and failure domains at
  once, which is an operational event rather than a routing detail.

TEF therefore:

* optimises **all admitted demands together** against one snapshot (`solve` takes the whole
  `FabricSnapshot`, never a single demand);
* accounts every resource once, across all demands, and re-derives the accounting independently in
  `verify_allocation`;
* distinguishes **hard constraints** (typed `ConstraintKind`) from **soft objectives**
  (`ObjectiveTerm` with weights), so an infeasible state can never be reported as merely
  suboptimal;
* treats **churn** as a first-class, policy-bounded decision rather than a side effect.

## Desired allocation versus applied forwarding state

```
adjacent systems                         Traffic Engineering Fabric
-----------------                        --------------------------
Fabric Topology  ------------.
Link State Fabric -----------.
Path Authority   ------------.           +-----------------------+
Path Planner     ------------.  inputs   |  derive()             |
Reservation Fab  ------------. --------> |  solve()              |
Admission Fabric ------------'           |  verify_allocation()  |
                                         +-----------+-----------+
                                                     |  Allocation (desired intent)
                                                     v
                                         +-----------------------+
                                         |  Plan lifecycle       |
                                         |  DECLARED .. COMMITTED|
                                         +-----------+-----------+
                                                     |  committed plan (durable)
                                                     v
                                   Route Fabric / Weighted Path Fabric / executors
                                                     |
                                                     v
                                          actual forwarding state
                                                     |
                                    (optional, external) observed_applied
```

The arrow from a committed plan into route installation is **not** implemented here. A committed
plan is a governed statement of intent; turning it into forwarding state is the job of the systems
that own route lifecycle and path weighting.

## Separation from Adaptive Routing Fabric

Adaptive Routing governs evidence-driven adaptation of local preference among legal candidates at its
own boundary. TEF governs the global allocation objective across interacting demands. The two are
deliberately kept apart:

* TEF has no notion of a per-path preference signal, no adaptation loop, and no local feedback path.
  It plans against an immutable snapshot and produces a new plan; it never nudges weights.
* TEF churn control exists so that a globally better solution does not silently become a local
  preference change. It compares a proposal against an incumbent and refuses to displace the
  incumbent unless policy-defined thresholds are crossed.
* The `minimize_churn` objective term and the `ChurnReport` are the only places where TEF
  reasons about change, and both are bounded by explicit policy numbers.

## Authority and staleness

Every plan is bound to an `AuthorityVector`: the fabric epoch, topology generation, link-state
generation, path-authority generation, failure-domain generation, the exact capacity, reservation,
policy, objective-profile and candidate-set identities *and* generations, and content digests of the
demand set, candidate set, resource catalog, reservation set, policy and objective profile.

Before a plan may be authorised and again before it may be committed, that vector is re-compared
against the live authority. A plan whose bound inputs have advanced becomes `STALE` and can never
mutate authoritative state. A plan whose bound inputs have *regressed* or whose identities disagree
is reported as a `CONFLICTING_INPUT` rather than silently reinterpreted.

## Missing evidence stays missing

`UNKNOWN` is never promoted into capability, legality or success:

* a demand that declares a latency bound makes any candidate path without latency evidence
  ineligible (the path cannot be *proven* to satisfy the bound);
* an absent generation is invalid and is rejected, never treated as current;
* a candidate path without usable capacity evidence carries no traffic;
* a request the solver cannot construct and cannot certify is reported as `SOLVER_LIMIT_REACHED`,
  which is neither acceptance nor proof of infeasibility.

## Deterministic operation

Two inputs that differ only in the ordering of their collections produce byte-identical allocations,
scores and digests. Every collection is canonicalized into a total order over a narrow,
locale-independent entity-name alphabet; authoritative bandwidth arithmetic is integer-only in
fabric bandwidth units (one unit = one kilobit per second); and floating point is never used for an
authoritative comparison, ordering or digest.
