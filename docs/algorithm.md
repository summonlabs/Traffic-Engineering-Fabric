# Planning algorithm

The built-in solver is called **LPMCF** (lexicographic progressive allocation with a min-cut
certificate). This document describes it exactly, states what it guarantees, and states what it does
not.

## Bandwidth units

Authoritative bandwidth is an integer count of **fabric bandwidth units (FBU)**, one unit being one
kilobit per second. Every arithmetic operation that can overflow is checked; overflow is reported as
a typed error rather than wrapping. Scores saturate at the documented numeric bound. Floating point
never participates in an allocation decision, an ordering, or a digest.

## Inputs derived before allocation

`tef::derive(snapshot)` performs pure accounting. It invents nothing.

1. **Resource residual.** For every resource:

   ```
   reserved(r)  = sum of bandwidth of active reservations bound to r that this plan may not displace
   available(r) = max(0, usable(r) - committed(r) - reserved(r))
   ```

2. **Reservation disposition.** For each reservation, in canonical order, exactly one disposition is
   chosen:

   | Condition | Disposition |
   | --- | --- |
   | the reservation's effective interval does not cover the evaluation tick | inactive; charged nowhere |
   | policy permits preemption **and** the reserving authority marked it preemptible (a `preemptible_with_authority` reservation must additionally be attached to a demand in this snapshot) | displaceable; charged nowhere, reported in `policy_exclusions` |
   | otherwise, the reservation has an owner demand in this snapshot or is listed in a demand's `reservation_bindings` | charged to that demand as a hard floor (deterministic: the lexicographically smallest claimant) |
   | otherwise | reserved: charged to the resource residual |

   A reservation is never double charged, and a reservation is never displaced without an explicit
   policy authorisation.

3. **Per-demand hard floor.** `floor(d) = max(minimum_bandwidth(d), reservation obligation(d))`.
   A demand whose floor exceeds its own maximum is reported as a reservation conflict.

4. **Eligibility.** A candidate path is eligible for a demand only if all of the following hold:

   * the path's candidate-set reference equals the demand's (identity **and** generation);
   * the path is in the demand's allow list when that list is non-empty;
   * the path is not in the demand's forbidden list;
   * the path's eligibility scope admits the demand's tenant or service class;
   * when the demand declares a latency bound, the path carries latency evidence **and** that
     evidence satisfies the bound;
   * when the demand declares a path-cost ceiling, the path's cost is within it;
   * the path traverses every failure domain the demand requires.

   Eligible paths are ordered by `(cost, latency, path id)`, with paths lacking latency evidence
   sorted last. When policy sets `max_paths_per_demand`, the list is truncated to that many paths in
   that order and the truncation is reported as a `path_count_limit` constraint.

## The min-cut certificate

Certification builds a deterministic max-flow relaxation:

```
S -> D_d      capacity floor(d)
D_d -> P_p    unbounded, for every path p eligible for d
P_p -> R_r    unbounded, for every resource r on p
R_r -> T      capacity ceiling(r)
```

Every legal path allocation induces a feasible flow in this network: route the rate on path `p`
through `p`'s node and split it across `p`'s resource arcs. Therefore

> **if the maximum flow is less than the sum of the hard floors, no legal allocation exists.**

This is a *sound* infeasibility proof, and the minimum cut names the resources that are structurally
short. The converse does not hold - the relaxation allows a path's rate to be reinterpreted across
resources - so a *feasible* certificate never authorises anything. Construction is always required as
well.

## Construction

The solver is a deterministic progressive allocation, not a black box:

1. **Processing order.** Active demands are processed by `(priority descending, demand id
   ascending)`.
2. **Hard minimums.** For each demand, its floor is placed by bottleneck water-filling over its
   preferred path order: for each path in turn, take
   `min(remaining need, min over the path's resources of (ceiling - allocated))`. A placement never
   exceeds the demand's authoritative maximum.
3. **Bounded repair.** When a floor cannot be placed, up to four deterministic repair rounds attempt
   to free capacity on the deficient resource by relocating *lower-priority* bandwidth onto those
   demands' alternative eligible paths. Every relocation is a deterministic choice in canonical
   order. The whole search is bounded by `SolveOptions::max_iterations`.
4. **Soft targets.** With every floor placed, remaining headroom is distributed toward each demand's
   target (`min(desired, maximum)`), in the same order - or, when the `fairness_across_groups`
   term is active, round-robin across tenants (one demand per tenant per round) so that no single
   tenant absorbs the residual headroom first.
5. **Consolidation.** When `minimize_path_count` is active, a bounded pass moves bandwidth from
   later-preferred paths onto earlier-preferred ones wherever headroom allows.
6. **Independent verification.** The constructed allocation is re-checked by `verify_allocation`,
   which re-derives residuals, eligibility and accounting from the snapshot without trusting the
   solver. A self-verification failure is reported as `CONFLICTING_INPUT`; a failed allocation is
   never offered as a plan.

### Utilisation ceiling

* The **policy cap** (`Policy::max_utilization_permille`) is a hard constraint. The achievable
  capacity of a resource for this plan is
  `min(available(r), max(0, usable(r) * cap / 1000 - committed(r) - reserved(r)))`.
* When the objective profile contains `minimize_max_utilization`, a bounded parametric search over
  integer permille values finds the **tightest** ceiling at which the hard minimums remain feasible
  *in the relaxation*. Because the relaxation is a lower bound, that ceiling can still defeat the
  constructive placement; the solver therefore widens the ceiling with a bounded deterministic search
  until construction succeeds. It never widens past the policy cap, and it never ignores an unmet
  floor.

## Typed outcomes

| Status | Meaning |
| --- | --- |
| `FEASIBLE` | every active demand was granted at least its hard floor, and the allocation was independently verified |
| `FEASIBLE_DEGRADED` | at least one floor was not placed and policy (`require_minimums = false`) plus the caller permit a degraded result |
| `INFEASIBLE_CAPACITY` | a minimum cut proves the declared hard minimums cannot all be carried |
| `INFEASIBLE_POLICY` | the policy utilisation cap, tenant list or service-class list makes the request unsatisfiable while it would be satisfiable without that policy |
| `INFEASIBLE_PATH_SET` | at least one demand has no eligible candidate path |
| `INFEASIBLE_RESERVATION_CONFLICT` | non-displaceable reservation obligations exceed authoritative usable capacity |
| `STALE_INPUT` / `CONFLICTING_INPUT` | a bound generation advanced, regressed, or disagrees |
| `UNSUPPORTED_OBJECTIVE` | the objective profile asks for something the solver will not do (for example a negative weight) |
| `SOLVER_LIMIT_REACHED` | the bounded search could not construct a solution **and** no certificate proves infeasibility |

`SOLVER_LIMIT_REACHED` is the honest "not proven either way" outcome. It is never reported as
success.

## Objectives and deterministic tie-breaking

Each objective term contributes `weight * raw`, reported as an `ObjectiveComponent`. Lower total
score is better.

| Term | Raw magnitude |
| --- | --- |
| `satisfy_minimums` | total shortfall below the hard floors (FBU) |
| `minimize_max_utilization` | worst resource utilisation (permille) |
| `minimize_congestion_exposure` | sum over resources of `allocated * permille(allocated, available) / 1000` |
| `minimize_total_path_cost` | sum over shares of `granted * path cost` (saturating) |
| `minimize_churn` | moved bandwidth versus the incumbent (FBU) |
| `preserve_reservations` | number of active reservations the plan is permitted to displace |
| `preserve_priority` | sum over demands of `(256 - priority) * shortfall_below_desired` |
| `maximize_desired_bandwidth` | total shortfall below the desired bandwidth (FBU) |
| `fairness_across_groups` | spread in permille between the best and worst tenant attainment ratio |
| `minimize_failure_domain_concentration` | largest total allocation through any single failure domain (FBU) |
| `minimize_path_count` | number of (demand, path) shares carrying traffic |

Tie-breaking is total and deterministic:

1. objective term order is the profile's lexicographic priority and is preserved exactly by
   canonicalization;
2. demands are ordered by `(priority descending, demand id ascending)`;
3. paths are ordered by `(cost, latency, path id)`, with `minimize_churn` first preferring paths
   the incumbent already uses and `minimize_failure_domain_concentration` first preferring the
   currently least-loaded domain;
4. every remaining tie is broken by the canonical byte order of identities.

## Churn control

`compare_churn` compares a proposal against an incumbent on the same objective, computed the same
way, and produces a `ChurnReport`. The decision is one of:

| Decision | Meaning |
| --- | --- |
| `NO_INCUMBENT` | nothing to compare against |
| `ACCEPT` | the improvement cleared every policy gate |
| `HOLD_INCUMBENT` | a gate failed: improvement below `churn_improvement_threshold_permille`, moved bandwidth above `churn_max_moved_bandwidth_permille`, more than `churn_max_affected_demands` demands changed, or operational risk above `churn_max_operational_risk_permille` |
| `REJECT_REGRESSION` | the proposal scores worse than the incumbent |

`Engine::authorize` refuses a plan whose churn decision is `HOLD_INCUMBENT` or
`REJECT_REGRESSION` with the typed code `CHURN_BOUND`. The gates are evaluated in a fixed order
and every failing gate is named in the rationale, so the decision is reproducible and auditable.

The operational risk score is a documented deterministic composite:

```
risk_permille = min(1000,
                    (moved_permille * 60 + demand_share_permille * 25 + domain_share_permille * 15) / 100)
```

where `demand_share_permille` is the share of demands whose path shares changed and
`domain_share_permille` is the share of failure domains whose allocated bandwidth changed.

## Documented limitations

* **The built-in solver is not an exact LP solver.** It always respects every hard constraint and it
  proves infeasibility when the relaxation proves it, but it is not guaranteed to construct an
  allocation merely because one exists. Such a case is reported as `SOLVER_LIMIT_REACHED` (or as
  `FEASIBLE_DEGRADED` when policy permits degradation) - never as success and never as proof of
  infeasibility.
* **No external solver is required or bundled.** `solve()` is a free function with a single
  deterministic implementation; replacing it with a different solver is a source-level decision and
  would have to preserve the typed outcomes and the determinism guarantees.
* **Problem shape.** Path variables are whole-path: a rate placed on a path consumes that rate from
  every resource on the path atomically. The relaxation used for certification does not preserve that
  atomicity, which is exactly why a feasible certificate is not treated as an authorization.
* **Worst-case cost.** Eligibility derivation is `O(demands * paths * resources-per-path)` and the
  certificate is a Dinic max-flow bounded by `Limits::max_certificate_edges`. Population sizes above
  `Limits::max_demands`, `Limits::max_total_candidate_paths` or `Limits::max_resources` are
  rejected with `LIMIT_EXCEEDED` rather than attempted.
* **No wall-clock dependence.** Time enters only through `FabricSnapshot::evaluation_tick` and the
  reservation and demand effective intervals. The solver never reads a clock.
