// Traffic Engineering Fabric - seeded invariant and adversarial proofs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every population in this suite is produced by tef::test::random_fabric from an
// explicit integer seed, so a failure printed here is reproducible from the seed
// and the population triple that accompanies it. There is no <random>, no wall
// clock and no third-party dependency: the only entropy in this file is the seed
// arithmetic below.
//
// Seeded cases are numbered 1..64. The population tier is a pure function of the
// seed, so any printed seed reproduces one exact fabric:
//   seed  1..40 -> (demands=8,   paths=4,  resources=8)
//   seed 41..54 -> (demands=24,  paths=8,  resources=16)
//   seed 55..62 -> (demands=64,  paths=16, resources=32)
//   seed 63..64 -> (demands=128, paths=24, resources=48)
// The tiers are weighted rather than balanced so that the suite stays inside a
// few seconds: the largest population costs an order of magnitude more planning
// time per seed than the smallest one, and the widest configuration lives in the
// single worst-case fan-out test below.
//
// The invariant tests (1..11) are proofs over randomized populations; the
// adversarial tests (12..20) are hand-built hostile inputs. Neither set weakens
// the other: an adversarial case must produce a typed outcome or a typed
// ErrorCode, never a crash and never a silent default.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/fixtures.hpp"
#include "support/test_framework.hpp"
#include "tef/allocation.hpp"
#include "tef/authority.hpp"
#include "tef/derived.hpp"
#include "tef/feasibility.hpp"
#include "tef/limits.hpp"
#include "tef/model.hpp"
#include "tef/solver.hpp"

using namespace tef;
using tef::test::build_snapshot;
using tef::test::random_fabric;
using tef::test::SnapshotSpec;

namespace {

// ---------------------------------------------------------------------------
// Deterministic seeded populations
// ---------------------------------------------------------------------------

struct Population {
  std::size_t demands;
  std::size_t paths;
  std::size_t resources;
};

constexpr std::size_t kPopulationCount = 4;
constexpr Population kPopulations[kPopulationCount] = {
    {8, 4, 8},
    {24, 8, 16},
    {64, 16, 32},
    {128, 24, 48},
};
// 64 deterministic seeds per invariant test; every tier is exercised.
constexpr std::size_t kTierSeeds[kPopulationCount] = {40, 14, 8, 2};
constexpr std::uint64_t kSeededCases = 64;
static_assert(kTierSeeds[0] + kTierSeeds[1] + kTierSeeds[2] + kTierSeeds[3] == kSeededCases,
              "the seed tiers must cover every seeded case");

Population population_for(std::uint64_t seed) {
  std::uint64_t upper = 0;
  for (std::size_t tier = 0; tier < kPopulationCount; ++tier) {
    upper += kTierSeeds[tier];
    if (seed <= upper) return kPopulations[tier];
  }
  return kPopulations[kPopulationCount - 1];
}

std::string context(std::uint64_t seed) {
  const Population population = population_for(seed);
  return "seed=" + std::to_string(seed) +
         " population=(demands=" + std::to_string(population.demands) +
         ",paths=" + std::to_string(population.paths) +
         ",resources=" + std::to_string(population.resources) + ")";
}

template <class Body>
void for_every_seeded_spec(Body&& body) {
  for (std::uint64_t seed = 1; seed <= kSeededCases; ++seed) {
    const Population population = population_for(seed);
    body(random_fabric(seed, population.demands, population.paths, population.resources), seed);
  }
}

template <class Body>
void for_every_seeded_fabric(Body&& body) {
  for_every_seeded_spec([&body](SnapshotSpec spec, std::uint64_t seed) {
    body(build_snapshot(spec), seed);
  });
}

// The same populations with authoritative reservations attached, so that the
// reservation accounting of the solver is exercised by randomized inputs and
// not only by hand-written fixtures.
SnapshotSpec with_reservations(const SnapshotSpec& base, std::uint64_t seed) {
  SnapshotSpec spec = base;
  const std::size_t resources = spec.resources.size();
  const std::size_t demands = spec.demands.size();

  tef::test::ReservationSpec residual;
  residual.id = "reservation-residual";
  residual.resources = {spec.resources[seed % resources].id};
  residual.bandwidth = 200 + static_cast<std::int64_t>(seed % 7) * 50;
  residual.preemptibility = Preemptibility::not_preemptible;
  residual.active = true;
  spec.reservations.push_back(residual);

  // Preemption is denied by the active policy, so even a preemptible reservation
  // remains a non-displaceable obligation of the resource residual.
  tef::test::ReservationSpec preemptible = residual;
  preemptible.id = "reservation-preemptible";
  preemptible.resources = {spec.resources[(seed + 1) % resources].id};
  preemptible.preemptibility = Preemptibility::preemptible;
  spec.reservations.push_back(preemptible);

  // A reservation bound to a demand is charged to that demand and must not be
  // double charged to the resource residual.
  tef::test::ReservationSpec bound = residual;
  bound.id = "reservation-bound";
  bound.resources = {spec.resources[(seed + 2) % resources].id};
  bound.bandwidth = 100 + static_cast<std::int64_t>(seed % 5) * 25;
  bound.preemptibility = Preemptibility::not_preemptible;
  spec.demands[seed % demands].reservation_bindings.push_back(bound.id);
  spec.reservations.push_back(bound);

  return spec;
}

template <class Body>
void for_every_reserved_fabric(Body&& body) {
  for_every_seeded_spec([&body](SnapshotSpec spec, std::uint64_t seed) {
    body(build_snapshot(with_reservations(spec, seed)), seed);
  });
}

// ---------------------------------------------------------------------------
// Lookup and arithmetic helpers
// ---------------------------------------------------------------------------

const Demand* find_demand(const FabricSnapshot& snapshot, const DemandId& id) {
  for (const auto& demand : snapshot.demands) {
    if (demand.id == id) return &demand;
  }
  return nullptr;
}

const CandidatePath* find_path(const FabricSnapshot& snapshot, const PathId& id) {
  for (const auto& path : snapshot.paths) {
    if (path.id == id) return &path;
  }
  return nullptr;
}

const DemandDerivation* derived_demand(const Derivation& derivation, const DemandId& id) {
  for (const auto& demand : derivation.demands) {
    if (demand.demand == id) return &demand;
  }
  return nullptr;
}

bool bandwidth_in_range(std::int64_t value) {
  return value >= 0 && value <= Limits::max_bandwidth;
}

// True when a + b <= Limits::max_bandwidth without any intermediate overflow.
bool bandwidth_sum_fits(std::int64_t a, std::int64_t b) {
  if (a < 0 || b < 0) return false;
  return a <= Limits::max_bandwidth - b;
}

// True when a + b + c <= bound without any intermediate signed overflow.
bool sum_at_most(std::int64_t a, std::int64_t b, std::int64_t c, std::int64_t bound) {
  if (a < 0 || b < 0 || c < 0 || bound < 0) return false;
  if (a > bound - b) return false;
  if (a + b > bound - c) return false;
  return true;
}

// max(0, usable - committed - reserved) with no signed overflow; -1 reports a
// negative input, which is itself a defect.
std::int64_t residual_available(std::int64_t usable, std::int64_t committed, std::int64_t reserved) {
  if (usable < 0 || committed < 0 || reserved < 0) return -1;
  if (committed > Limits::max_bandwidth - reserved) return 0;
  const std::int64_t claimed = committed + reserved;
  if (claimed >= usable) return 0;
  return usable - claimed;
}

std::string describe_bindings(const std::vector<BindingConstraint>& bindings) {
  constexpr std::size_t kReported = 4;
  std::string out = "[";
  for (std::size_t i = 0; i < bindings.size() && i < kReported; ++i) {
    if (i != 0) out += ", ";
    out += std::string(to_string(bindings[i].kind)) + ":" + bindings[i].subject;
  }
  if (bindings.size() > kReported) out += ", ...";
  out += "]";
  return out;
}

SolveOutcome run_solver(const FabricSnapshot& snapshot, const SolveOptions& options,
                        const std::string& where) {
  const Result<SolveOutcome> result = solve(snapshot, options);
  if (!result.has_value()) {
    TEF_FAIL(where + ": solve refused with " + result.error().format());
  }
  return result.value();
}

// Every bandwidth quantity the allocation reports must be a bounded,
// non-negative integer; utilization is a permille ratio and is bounded too.
void check_allocation_bounds(const Allocation& allocation, const std::string& where) {
  const auto require = [&where](std::int64_t value, const std::string& subject,
                                std::string_view field) {
    TEF_CHECK_MSG(bandwidth_in_range(value),
                  where + " " + subject + " " + std::string(field) + "=" + std::to_string(value) +
                      " is outside [0, tef::Limits::max_bandwidth]");
  };

  require(allocation.total_granted, "allocation", "total_granted");
  require(allocation.total_reserved, "allocation", "total_reserved");
  TEF_CHECK_MSG(allocation.total_reserved <= allocation.total_granted,
                where + " total_reserved=" + std::to_string(allocation.total_reserved) +
                    " exceeds total_granted=" + std::to_string(allocation.total_granted));

  for (const auto& demand : allocation.demands) {
    const std::string subject = "demand " + demand.demand.str();
    require(demand.minimum, subject, "minimum");
    require(demand.desired, subject, "desired");
    require(demand.maximum, subject, "maximum");
    require(demand.granted, subject, "granted");
    require(demand.reserved, subject, "reserved");
    require(demand.effective, subject, "effective");
    require(demand.observed_applied, subject, "observed_applied");
    require(demand.shortfall_against_minimum, subject, "shortfall_against_minimum");
    require(demand.shortfall_against_desired, subject, "shortfall_against_desired");
    TEF_CHECK_MSG(demand.reserved <= demand.granted,
                  where + " " + subject + " reserved=" + std::to_string(demand.reserved) +
                      " exceeds granted=" + std::to_string(demand.granted));
    TEF_CHECK_MSG(demand.granted <= demand.maximum,
                  where + " " + subject + " granted=" + std::to_string(demand.granted) +
                      " exceeds maximum=" + std::to_string(demand.maximum));
    for (const auto& share : demand.shares) {
      const std::string path_subject = subject + " path " + share.path.str();
      require(share.minimum, path_subject, "minimum");
      require(share.desired, path_subject, "desired");
      require(share.granted, path_subject, "granted");
      require(share.reserved, path_subject, "reserved");
      TEF_CHECK_MSG(share.delta_from_incumbent >= -Limits::max_bandwidth &&
                        share.delta_from_incumbent <= Limits::max_bandwidth,
                    where + " " + path_subject +
                        " delta_from_incumbent=" + std::to_string(share.delta_from_incumbent) +
                        " is outside [-max_bandwidth, max_bandwidth]");
    }
  }

  for (const auto& resource : allocation.resources) {
    const std::string subject = "resource " + resource.resource.str();
    require(resource.usable_capacity, subject, "usable_capacity");
    require(resource.committed_load, subject, "committed_load");
    require(resource.reserved, subject, "reserved");
    require(resource.available, subject, "available");
    require(resource.allocated, subject, "allocated");
    require(resource.headroom, subject, "headroom");
    // Utilization is a ratio, not a bandwidth: it reads above 1000 permille only
    // when the authoritative input already commits more than the usable capacity
    // of the resource (a state the plan may report but must never add to).
    TEF_CHECK_MSG(resource.utilization_permille >= 0,
                  where + " " + subject + " utilization_permille=" +
                      std::to_string(resource.utilization_permille) + " is negative");
    if (sum_at_most(resource.committed_load, resource.reserved, resource.allocated,
                    resource.usable_capacity)) {
      TEF_CHECK_MSG(resource.utilization_permille <= 1000,
                    where + " " + subject + " utilization_permille=" +
                        std::to_string(resource.utilization_permille) +
                        " exceeds 1000 permille while committed + reserved + allocated stays "
                        "inside usable capacity");
    }
  }
}

bool reports_field(const AuthorityDelta& delta, AuthorityField field) {
  const auto has = [field](const std::vector<AuthorityField>& fields) {
    return std::find(fields.begin(), fields.end(), field) != fields.end();
  };
  return has(delta.advanced) || has(delta.regressed) || has(delta.conflicting);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Resource capacity is never exceeded
// ---------------------------------------------------------------------------

TEF_TEST(no_resource_exceeds_its_authoritative_available_capacity) {
  for_every_reserved_fabric([](const FabricSnapshot& snapshot, std::uint64_t seed) {
    const SolveOutcome outcome = run_solver(snapshot, SolveOptions{}, context(seed));
    for (const auto& resource : outcome.allocation.resources) {
      const std::string where = context(seed) + " resource " + resource.resource.str();
      TEF_CHECK_MSG(resource.committed_load >= 0 && resource.reserved >= 0 &&
                        resource.allocated >= 0,
                    where + " carries a negative quantity: committed=" +
                        std::to_string(resource.committed_load) +
                        " reserved=" + std::to_string(resource.reserved) +
                        " allocated=" + std::to_string(resource.allocated));
      // An authoritative input may already commit more load than the resource can
      // carry. The plan may report that state, but it must never add to it: either
      // the whole occupancy stays inside usable capacity, or nothing at all is
      // allocated on the over-committed resource.
      const bool input_fits =
          sum_at_most(resource.committed_load, resource.reserved, std::int64_t{0},
                      resource.usable_capacity);
      if (input_fits) {
        TEF_CHECK_MSG(sum_at_most(resource.committed_load, resource.reserved, resource.allocated,
                                  resource.usable_capacity),
                      where + ": committed=" + std::to_string(resource.committed_load) +
                          " + reserved=" + std::to_string(resource.reserved) +
                          " + allocated=" + std::to_string(resource.allocated) +
                          " exceeds usable=" + std::to_string(resource.usable_capacity));
      } else {
        TEF_CHECK_MSG(resource.allocated == 0,
                      where + ": committed=" + std::to_string(resource.committed_load) +
                          " + reserved=" + std::to_string(resource.reserved) +
                          " already exceeds usable=" + std::to_string(resource.usable_capacity) +
                          " and the plan still allocated " + std::to_string(resource.allocated));
      }
      TEF_CHECK_MSG(resource.allocated <= resource.available,
                    where + ": allocated=" + std::to_string(resource.allocated) +
                        " exceeds available=" + std::to_string(resource.available));
    }
  });
}

// ---------------------------------------------------------------------------
// 2. Hard minimums are satisfied or explicitly reported
// ---------------------------------------------------------------------------

TEF_TEST(hard_minimums_are_either_satisfied_or_explicitly_reported) {
  for_every_seeded_fabric([](const FabricSnapshot& snapshot, std::uint64_t seed) {
    const SolveOutcome outcome = run_solver(snapshot, SolveOptions{}, context(seed));
    const Result<Derivation> derived = derive(snapshot);
    if (!derived.has_value()) {
      TEF_FAIL(context(seed) + ": derive refused with " + derived.error().format());
    }
    for (const auto& demand : derived.value().demands) {
      if (!demand.active) continue;
      const DemandAllocation* entry = outcome.allocation.find_demand(demand.demand);
      TEF_CHECK_MSG(entry != nullptr, context(seed) + " demand " + demand.demand.str() +
                                          " is missing from the allocation");
      if (entry == nullptr) continue;
      if (entry->granted >= demand.floor) continue;
      TEF_CHECK_MSG(outcome.status != FeasibilityStatus::feasible,
                    context(seed) + " demand " + demand.demand.str() +
                        " is FEASIBLE but granted=" + std::to_string(entry->granted) +
                        " is below its hard floor=" + std::to_string(demand.floor));
      bool named = false;
      for (const auto& binding : outcome.feasibility.binding) {
        if (binding.demand == demand.demand ||
            binding.subject.find(demand.demand.str()) != std::string::npos) {
          named = true;
          break;
        }
      }
      TEF_CHECK_MSG(named, context(seed) + " demand " + demand.demand.str() +
                               " is short of its floor (granted=" + std::to_string(entry->granted) +
                               ", floor=" + std::to_string(demand.floor) +
                               ") but status=" + std::string(to_string(outcome.status)) +
                               " does not name it in the binding set " +
                               describe_bindings(outcome.feasibility.binding));
    }
    for (const auto& demand : derived.value().demands) {
      if (demand.active) continue;
      const DemandAllocation* entry = outcome.allocation.find_demand(demand.demand);
      if (entry == nullptr) continue;
      TEF_CHECK_MSG(entry->granted == 0, context(seed) + " inactive demand " +
                                             demand.demand.str() + " was granted " +
                                             std::to_string(entry->granted));
    }
  });
}

// ---------------------------------------------------------------------------
// 3. Only authorized candidate paths may carry traffic
// ---------------------------------------------------------------------------

TEF_TEST(no_allocation_uses_a_path_outside_the_authorized_candidate_set) {
  for_every_seeded_fabric([](const FabricSnapshot& snapshot, std::uint64_t seed) {
    const SolveOutcome outcome = run_solver(snapshot, SolveOptions{}, context(seed));
    const Result<Derivation> derived = derive(snapshot);
    if (!derived.has_value()) {
      TEF_FAIL(context(seed) + ": derive refused with " + derived.error().format());
    }
    for (const auto& entry : outcome.allocation.demands) {
      const DemandDerivation* demand = derived_demand(derived.value(), entry.demand);
      TEF_CHECK_MSG(demand != nullptr, context(seed) + " allocation carries demand " +
                                           entry.demand.str() + " which is not in the snapshot");
      if (demand == nullptr) continue;
      const Demand* declared = find_demand(snapshot, entry.demand);
      TEF_CHECK_MSG(declared != nullptr, context(seed) + " demand " + entry.demand.str() +
                                             " is not declared by the snapshot");
      if (!demand->active) {
        TEF_CHECK_MSG(entry.shares.empty(),
                      context(seed) + " inactive demand " + entry.demand.str() + " holds " +
                          std::to_string(entry.shares.size()) + " path shares");
      }
      for (const auto& share : entry.shares) {
        const std::string where =
            context(seed) + " demand " + entry.demand.str() + " path " + share.path.str();
        TEF_CHECK_MSG(share.granted >= 0,
                      where + " carries negative granted bandwidth " + std::to_string(share.granted));
        const CandidatePath* path = find_path(snapshot, share.path);
        TEF_CHECK_MSG(path != nullptr, where + " is absent from the snapshot candidate set");
        if (path == nullptr) continue;
        TEF_CHECK_MSG(path->candidate_set == snapshot.candidate_set,
                      where + " belongs to candidate set " + path->candidate_set.id.str() + "/" +
                          std::to_string(path->candidate_set.generation.value()) +
                          " but the snapshot declares " + snapshot.candidate_set.id.str() + "/" +
                          std::to_string(snapshot.candidate_set.generation.value()));
        TEF_CHECK_MSG(share.generation == path->generation,
                      where + " binds path generation " +
                          std::to_string(share.generation.value()) + " instead of " +
                          std::to_string(path->generation.value()));
        TEF_CHECK_MSG(std::find(demand->eligible_paths.begin(), demand->eligible_paths.end(),
                                share.path) != demand->eligible_paths.end(),
                      where + " is outside the derived eligible path set");
        if (declared == nullptr) continue;
        TEF_CHECK_MSG(declared->allowed_paths.empty() ||
                          std::find(declared->allowed_paths.begin(), declared->allowed_paths.end(),
                                    share.path) != declared->allowed_paths.end(),
                      where + " is not in the demand allow list");
        TEF_CHECK_MSG(std::find(declared->forbidden_paths.begin(), declared->forbidden_paths.end(),
                                share.path) == declared->forbidden_paths.end(),
                      where + " is explicitly forbidden for the demand");
        if (path->scope == EligibilityScope::tenant) {
          TEF_CHECK_MSG(path->scope_tenant == declared->tenant,
                        where + " is tenant scoped to " + path->scope_tenant.str() +
                            " but the demand tenant is " + declared->tenant.str());
        }
        if (path->scope == EligibilityScope::service_class) {
          TEF_CHECK_MSG(path->scope_service_class == declared->service_class,
                        where + " is service-class scoped to " + path->scope_service_class.str() +
                            " but the demand service class is " + declared->service_class.str());
        }
        if (declared->latency_bound_micros.has_value()) {
          TEF_CHECK_MSG(path->latency_micros.has_value() &&
                            *path->latency_micros <= *declared->latency_bound_micros,
                        where + " violates the demand latency bound");
        }
        if (declared->path_cost_ceiling.has_value()) {
          TEF_CHECK_MSG(path->cost <= *declared->path_cost_ceiling,
                        where + " costs " + std::to_string(path->cost) +
                            " which exceeds the demand cost ceiling " +
                            std::to_string(*declared->path_cost_ceiling));
        }
      }
    }
  });
}

// ---------------------------------------------------------------------------
// 4. Granted bandwidth accounting closes exactly
// ---------------------------------------------------------------------------

TEF_TEST(total_granted_accounting_closes_exactly) {
  for_every_seeded_fabric([](const FabricSnapshot& snapshot, std::uint64_t seed) {
    const SolveOutcome outcome = run_solver(snapshot, SolveOptions{}, context(seed));
    std::int64_t total = 0;
    for (const auto& entry : outcome.allocation.demands) {
      std::int64_t share_total = 0;
      for (const auto& share : entry.shares) {
        TEF_CHECK_MSG(bandwidth_sum_fits(share_total, share.granted),
                      context(seed) + " demand " + entry.demand.str() +
                          " path share grants overflow the bandwidth bound");
        share_total += share.granted;
      }
      TEF_CHECK_MSG(share_total == entry.granted,
                    context(seed) + " demand " + entry.demand.str() + ": granted=" +
                        std::to_string(entry.granted) + " but the path shares sum to " +
                        std::to_string(share_total));
      TEF_CHECK_MSG(bandwidth_sum_fits(total, entry.granted),
                    context(seed) + " demand grants overflow the bandwidth bound");
      total += entry.granted;
    }
    TEF_CHECK_MSG(total == outcome.allocation.total_granted,
                  context(seed) + ": allocation.total_granted=" +
                      std::to_string(outcome.allocation.total_granted) +
                      " but the demand grants sum to " + std::to_string(total));
  });
}

// ---------------------------------------------------------------------------
// 5. Reservation obligations survive according to policy
// ---------------------------------------------------------------------------

TEF_TEST(reserved_obligations_are_preserved_according_to_policy) {
  const auto check = [](const FabricSnapshot& snapshot, std::uint64_t seed,
                        const std::string& variant) {
    const SolveOutcome outcome =
        run_solver(snapshot, SolveOptions{}, context(seed) + " " + variant);
    const Result<Derivation> derived = derive(snapshot);
    if (!derived.has_value()) {
      TEF_FAIL(context(seed) + " " + variant + ": derive refused with " + derived.error().format());
    }

    std::map<std::string, std::vector<ResourceId>> reservation_resources;
    for (const auto& reservation : snapshot.reservations.reservations) {
      reservation_resources[reservation.id.str()] = reservation.resources;
    }
    std::map<std::string, std::int64_t> expected_reserved;
    for (const auto& accounting : derived.value().reservations) {
      if (!accounting.active || accounting.displaceable || accounting.charged_to.valid()) continue;
      const auto it = reservation_resources.find(accounting.reservation.str());
      if (it == reservation_resources.end()) continue;
      for (const auto& resource : it->second) {
        expected_reserved[resource.str()] += accounting.bandwidth;
      }
    }

    for (const auto& resource : outcome.allocation.resources) {
      const std::string where =
          context(seed) + " " + variant + " resource " + resource.resource.str();
      const auto it = expected_reserved.find(resource.resource.str());
      const std::int64_t expected = it == expected_reserved.end() ? 0 : it->second;
      TEF_CHECK_MSG(resource.reserved == expected,
                    where + ": reserved=" + std::to_string(resource.reserved) +
                        " but the non-displaceable obligations sum to " +
                        std::to_string(expected));
      const std::int64_t computed =
          residual_available(resource.usable_capacity, resource.committed_load, resource.reserved);
      TEF_CHECK_MSG(computed >= 0 && resource.available == computed,
                    where + ": available=" + std::to_string(resource.available) +
                        " but max(0, usable - committed - reserved)=" + std::to_string(computed));
    }
  };

  for_every_reserved_fabric([&check](const FabricSnapshot& snapshot, std::uint64_t seed) {
    check(snapshot, seed, "preemption-denied");
  });
  for_every_seeded_spec([&check](SnapshotSpec spec, std::uint64_t seed) {
    SnapshotSpec open = with_reservations(spec, seed);
    open.policy.allow_preemption = true;
    check(build_snapshot(open), seed, "preemption-permitted");
  });
}

// ---------------------------------------------------------------------------
// 6. The independent verifier accepts every solver output it is offered
// ---------------------------------------------------------------------------

TEF_TEST(verify_allocation_accepts_every_solver_output) {
  for_every_seeded_fabric([](const FabricSnapshot& snapshot, std::uint64_t seed) {
    const SolveOutcome outcome = run_solver(snapshot, SolveOptions{}, context(seed));
    const std::vector<BindingConstraint> violations =
        verify_allocation(snapshot, outcome.allocation);
    if (carries_allocation(outcome.status)) {
      TEF_CHECK_MSG(violations.empty(),
                    context(seed) + " status=" + std::string(to_string(outcome.status)) +
                        " carries an allocation that the verifier rejects: " +
                        describe_bindings(violations));
    }
    if (outcome.status == FeasibilityStatus::feasible) {
      TEF_CHECK_MSG(outcome.verified,
                    context(seed) + " reported FEASIBLE without being independently verified");
      TEF_CHECK_MSG(violations.empty(),
                    context(seed) + " FEASIBLE allocation fails verification: " +
                        describe_bindings(violations));
    }
  });
}

// ---------------------------------------------------------------------------
// 7. Repeated solves are identical
// ---------------------------------------------------------------------------

TEF_TEST(determinism_across_repeated_solves) {
  for_every_seeded_fabric([](const FabricSnapshot& snapshot, std::uint64_t seed) {
    const SolveOutcome first = run_solver(snapshot, SolveOptions{}, context(seed) + " first solve");
    const SolveOutcome second =
        run_solver(snapshot, SolveOptions{}, context(seed) + " second solve");
    TEF_CHECK_EQ(first.status, second.status);
    TEF_CHECK_MSG(first.score == second.score,
                  context(seed) + ": score " + std::to_string(first.score) +
                      " != " + std::to_string(second.score));
    TEF_CHECK_MSG(first.allocation_digest.hex() == second.allocation_digest.hex(),
                  context(seed) + ": allocation digest " + first.allocation_digest.hex() +
                      " != " + second.allocation_digest.hex());
    TEF_CHECK_EQ(first.allocation_digest.hex(), second.allocation_digest.hex());
  });
}

// ---------------------------------------------------------------------------
// 8. Input permutation does not change the result
// ---------------------------------------------------------------------------

TEF_TEST(determinism_under_input_permutation) {
  for_every_seeded_spec([](SnapshotSpec spec, std::uint64_t seed) {
    const FabricSnapshot canonical = build_snapshot(spec);
    std::reverse(spec.demands.begin(), spec.demands.end());
    std::reverse(spec.paths.begin(), spec.paths.end());
    std::reverse(spec.resources.begin(), spec.resources.end());
    const FabricSnapshot permuted = build_snapshot(spec);

    const SolveOutcome first =
        run_solver(canonical, SolveOptions{}, context(seed) + " canonical order");
    const SolveOutcome second =
        run_solver(permuted, SolveOptions{}, context(seed) + " reversed order");
    TEF_CHECK_EQ(first.status, second.status);
    TEF_CHECK_MSG(first.allocation_digest.hex() == second.allocation_digest.hex(),
                  context(seed) + ": reversed inputs changed the allocation digest from " +
                      first.allocation_digest.hex() + " to " + second.allocation_digest.hex());
  });
}

// ---------------------------------------------------------------------------
// 9. No negative or overflowed bandwidth
// ---------------------------------------------------------------------------

TEF_TEST(no_negative_or_overflowed_values) {
  for_every_reserved_fabric([](const FabricSnapshot& snapshot, std::uint64_t seed) {
    const SolveOutcome outcome = run_solver(snapshot, SolveOptions{}, context(seed));
    check_allocation_bounds(outcome.allocation, context(seed));

    std::int64_t total = 0;
    bool fits = true;
    for (const auto& demand : outcome.allocation.demands) {
      if (!bandwidth_sum_fits(total, demand.granted)) {
        fits = false;
        break;
      }
      total += demand.granted;
    }
    TEF_CHECK_MSG(fits, context(seed) + " total_granted accounting overflows the bandwidth bound");
    TEF_CHECK_MSG(fits && total == outcome.allocation.total_granted,
                  context(seed) + ": total_granted=" +
                      std::to_string(outcome.allocation.total_granted) +
                      " does not equal the saturating demand total " + std::to_string(total));
  });
}

// ---------------------------------------------------------------------------
// 10. Saturation reporting is consistent with headroom
// ---------------------------------------------------------------------------

TEF_TEST(saturated_resources_are_reported_consistently) {
  for_every_seeded_fabric([](const FabricSnapshot& snapshot, std::uint64_t seed) {
    const SolveOutcome outcome = run_solver(snapshot, SolveOptions{}, context(seed));
    for (const auto& resource : outcome.allocation.resources) {
      const std::string where = context(seed) + " resource " + resource.resource.str();
      TEF_CHECK_MSG(resource.headroom == resource.available - resource.allocated,
                    where + ": headroom=" + std::to_string(resource.headroom) + " but available=" +
                        std::to_string(resource.available) + " minus allocated=" +
                        std::to_string(resource.allocated) + " is " +
                        std::to_string(resource.available - resource.allocated));
      if (resource.headroom == 0) {
        TEF_CHECK_MSG(resource.saturated,
                      where + " has no headroom (available=" +
                          std::to_string(resource.available) + ", allocated=" +
                          std::to_string(resource.allocated) + ") but is not flagged saturated");
      }
      if (resource.headroom > 0 && resource.available > 0) {
        TEF_CHECK_MSG(!resource.saturated,
                      where + " has headroom=" + std::to_string(resource.headroom) +
                          " and available=" + std::to_string(resource.available) +
                          " but is flagged saturated");
      }
    }
  });
}

// ---------------------------------------------------------------------------
// 11. Generation advancement invalidates dependent plans
// ---------------------------------------------------------------------------

TEF_TEST(generation_advancement_invalidates_dependent_plans) {
  const SnapshotSpec base = tef::test::simple_fabric();
  const FabricSnapshot snapshot = build_snapshot(base);
  const AuthorityVector bound = authority_of(snapshot);
  TEF_CHECK_MSG(bound.valid(), "the bound authority vector is incomplete");
  TEF_CHECK_MSG(compare_authority(bound, bound).identical(),
                "a plan compared against its own authority must be identical");

  // (a) The topology generation advanced: the plan is stale, and the reverse
  // comparison is a contradiction, not an advancement.
  {
    SnapshotSpec advanced = base;
    advanced.topology_generation = base.topology_generation + 1;
    const AuthorityVector live = authority_of(build_snapshot(advanced));
    const AuthorityDelta delta = compare_authority(bound, live);
    TEF_CHECK_MSG(delta.stale(), "advanced topology generation was not reported: " + delta.describe());
    TEF_CHECK_MSG(std::find(delta.advanced.begin(), delta.advanced.end(),
                            AuthorityField::topology_generation) != delta.advanced.end(),
                  "advanced topology generation is missing from the delta: " + delta.describe());
    const AuthorityDelta reverse = compare_authority(live, bound);
    TEF_CHECK_MSG(reverse.contradictory(),
                  "the reverse comparison is not a contradiction: " + reverse.describe());
    TEF_CHECK_MSG(std::find(reverse.regressed.begin(), reverse.regressed.end(),
                            AuthorityField::topology_generation) != reverse.regressed.end(),
                  "the reverse comparison does not report a regression: " + reverse.describe());
  }

  // (b) The capacity snapshot generation advanced.
  {
    FabricSnapshot advanced = snapshot;
    advanced.capacity.generation = advanced.capacity.generation.next();
    const AuthorityVector live = authority_of(advanced);
    const AuthorityDelta delta = compare_authority(bound, live);
    TEF_CHECK_MSG(delta.stale(),
                  "advanced capacity snapshot generation was not reported: " + delta.describe());
    TEF_CHECK_MSG(reports_field(delta, AuthorityField::capacity_snapshot_generation),
                  "the capacity snapshot generation is missing from the delta: " +
                      delta.describe());
    const AuthorityDelta reverse = compare_authority(live, bound);
    TEF_CHECK_MSG(reverse.contradictory(),
                  "the reverse comparison is not a contradiction: " + reverse.describe());
  }

  // (c) The demand set content changed under an unchanged generation. The
  // authority vector carries the demand set as content plus the snapshot
  // generations that scope it, so a same-generation content change is a
  // contradiction rather than an advancement - but it is never identical, and a
  // plan bound to the old content can never be reused.
  {
    SnapshotSpec revised = base;
    revised.demand_generation = base.demand_generation + 1;
    revised.demands[0].desired = base.demands[0].desired + 1000;
    revised.demands[0].maximum = base.demands[0].maximum + 1000;
    const AuthorityVector live = authority_of(build_snapshot(revised));
    const AuthorityDelta delta = compare_authority(bound, live);
    TEF_CHECK_MSG(!delta.identical(),
                  "a changed demand set compared identical: " + delta.describe());
    TEF_CHECK_MSG(reports_field(delta, AuthorityField::demand_set),
                  "the changed demand set content is not reported: " + delta.describe());
    TEF_CHECK_MSG(!compare_authority(live, bound).identical(),
                  "the reverse demand set comparison is identical");
  }

  // (d) A demand set published under an advanced epoch is strictly newer, so the
  // bound plan is stale, and the reverse direction is a contradiction.
  {
    SnapshotSpec revised = base;
    revised.fabric_epoch = base.fabric_epoch + 1;
    revised.demands[0].desired = base.demands[0].desired + 1000;
    revised.demands[0].maximum = base.demands[0].maximum + 1000;
    const AuthorityVector live = authority_of(build_snapshot(revised));
    const AuthorityDelta delta = compare_authority(bound, live);
    TEF_CHECK_MSG(delta.stale(),
                  "an advanced demand set revision was not reported as stale: " + delta.describe());
    TEF_CHECK_MSG(reports_field(delta, AuthorityField::demand_set),
                  "the advanced demand set content is not reported: " + delta.describe());
    const AuthorityDelta reverse = compare_authority(live, bound);
    TEF_CHECK_MSG(reverse.contradictory(),
                  "the reverse comparison is not a contradiction: " + reverse.describe());
  }
}

// ---------------------------------------------------------------------------
// 12. Zero capacity and disconnected populations never carry traffic
// ---------------------------------------------------------------------------

TEF_TEST(zero_capacity_and_disconnected_populations_never_get_traffic) {
  for (std::uint64_t seed = 1; seed <= kSeededCases; ++seed) {
    const Population population = population_for(seed);

    // (a) Every resource has zero usable capacity.
    SnapshotSpec zeroed = random_fabric(seed, population.demands, population.paths,
                                        population.resources);
    for (auto& resource : zeroed.resources) {
      resource.usable = 0;
      resource.committed = 0;
    }
    for (auto& demand : zeroed.demands) {
      demand.minimum = 100;
      demand.desired = std::max(demand.desired, demand.minimum);
      demand.maximum = std::max(demand.maximum, demand.desired);
    }
    const FabricSnapshot empty = build_snapshot(zeroed);
    const SolveOutcome empty_outcome =
        run_solver(empty, SolveOptions{}, context(seed) + " zero-capacity");
    TEF_CHECK_MSG(is_definitively_infeasible(empty_outcome.status),
                  context(seed) + " zero-capacity fabric returned status=" +
                      std::string(to_string(empty_outcome.status)) + " instead of an infeasibility");
    TEF_CHECK_MSG(empty_outcome.allocation.total_granted == 0,
                  context(seed) + " zero-capacity fabric granted " +
                      std::to_string(empty_outcome.allocation.total_granted));
    for (const auto& resource : empty_outcome.allocation.resources) {
      TEF_CHECK_MSG(resource.allocated == 0 && resource.available == 0,
                    context(seed) + " zero-capacity resource " + resource.resource.str() +
                        " reports available=" + std::to_string(resource.available) +
                        " allocated=" + std::to_string(resource.allocated));
    }
    for (const auto& demand : empty_outcome.allocation.demands) {
      TEF_CHECK_MSG(demand.granted == 0 && demand.shares.empty(),
                    context(seed) + " zero-capacity demand " + demand.demand.str() +
                        " was granted " + std::to_string(demand.granted));
    }

    // (b) One demand is forbidden every candidate path.
    SnapshotSpec forbidden = random_fabric(seed, population.demands, population.paths,
                                           population.resources);
    const std::string blocked_id = forbidden.demands[0].id;
    for (const auto& path : forbidden.paths) {
      forbidden.demands[0].forbidden_paths.push_back(path.id);
    }
    forbidden.demands[0].minimum = 100;
    forbidden.demands[0].desired = std::max(forbidden.demands[0].desired, std::int64_t{100});
    forbidden.demands[0].maximum =
        std::max(forbidden.demands[0].maximum, forbidden.demands[0].desired);
    const FabricSnapshot disconnected = build_snapshot(forbidden);
    const SolveOutcome disconnected_outcome =
        run_solver(disconnected, SolveOptions{}, context(seed) + " forbidden-every-path");
    TEF_CHECK_MSG(is_definitively_infeasible(disconnected_outcome.status),
                  context(seed) + " demand " + blocked_id + " has no eligible path but status=" +
                      std::string(to_string(disconnected_outcome.status)) +
                      " is not a typed infeasibility");
    const auto blocked = DemandId::parse(blocked_id);
    TEF_CHECK(blocked.has_value());
    if (blocked.has_value()) {
      const DemandAllocation* entry = disconnected_outcome.allocation.find_demand(*blocked);
      TEF_CHECK_MSG(entry != nullptr, context(seed) + " demand " + blocked_id +
                                          " is missing from the allocation");
      if (entry != nullptr) {
        TEF_CHECK_MSG(entry->granted == 0 && entry->shares.empty(),
                      context(seed) + " demand " + blocked_id + " was granted " +
                          std::to_string(entry->granted) + " on " +
                          std::to_string(entry->shares.size()) + " forbidden paths");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 13. Enormous declared bandwidth is bounded
// ---------------------------------------------------------------------------

TEF_TEST(enormous_declared_bandwidth_is_bounded) {
  SnapshotSpec spec;
  spec.resources = {{"res-tiny-a", 1000, 0, {"domain-1"}},
                    {"res-tiny-b", 1000, 0, {"domain-2"}}};
  spec.paths = {{"path-tiny-a", {"res-tiny-a"}, {"domain-1"}, 1, true, 100},
                {"path-tiny-b", {"res-tiny-b"}, {"domain-2"}, 1, true, 100}};
  spec.demands = {{"demand-enormous", "tenant-a", "class-gold", Limits::max_bandwidth,
                   Limits::max_bandwidth, Limits::max_bandwidth, 255}};
  const FabricSnapshot snapshot = build_snapshot(spec);
  TEF_CHECK_MSG(validate_structure(snapshot).ok(), validate_structure(snapshot).format());

  const Result<SolveOutcome> result = solve(snapshot, SolveOptions{});
  if (!result.has_value()) {
    const ErrorCode code = result.error().code();
    TEF_CHECK_MSG(code == ErrorCode::numeric_overflow || code == ErrorCode::numeric_invalid ||
                      code == ErrorCode::limit_exceeded || code == ErrorCode::capacity_exhausted,
                  "enormous declared bandwidth was refused with an unexpected code: " +
                      result.error().format());
    return;
  }
  const SolveOutcome& outcome = result.value();
  TEF_CHECK_MSG(!outcome.feasibility.ok(),
                "a demand of Limits::max_bandwidth was reported feasible on 1000 units of "
                "capacity: status=" + std::string(to_string(outcome.status)));
  TEF_CHECK_MSG(is_definitively_infeasible(outcome.status),
                "enormous declared bandwidth produced status=" +
                    std::string(to_string(outcome.status)) + " instead of a typed infeasibility");
  check_allocation_bounds(outcome.allocation, "enormous declared bandwidth");
}

// ---------------------------------------------------------------------------
// 14. Duplicate identities are rejected before planning
// ---------------------------------------------------------------------------

TEF_TEST(duplicate_path_and_demand_identities_are_rejected) {
  const SnapshotSpec base = tef::test::simple_fabric();
  TEF_CHECK_MSG(validate_structure(build_snapshot(base)).ok(),
                "the unmodified fixture must pass structural validation");

  SnapshotSpec duplicate_path = base;
  duplicate_path.paths.push_back(base.paths.front());
  const FabricSnapshot with_duplicate_path = build_snapshot(duplicate_path);
  TEF_CHECK_EQ(with_duplicate_path.paths.size(), base.paths.size() + 1);
  const Status path_status = validate_structure(with_duplicate_path);
  TEF_CHECK_MSG(path_status.code() == ErrorCode::duplicate_identity,
                "a duplicated candidate path identity returned " + path_status.format());

  SnapshotSpec duplicate_demand = base;
  duplicate_demand.demands.push_back(base.demands.front());
  const FabricSnapshot with_duplicate_demand = build_snapshot(duplicate_demand);
  TEF_CHECK_EQ(with_duplicate_demand.demands.size(), base.demands.size() + 1);
  const Status demand_status = validate_structure(with_duplicate_demand);
  TEF_CHECK_MSG(demand_status.code() == ErrorCode::duplicate_identity,
                "a duplicated demand identity returned " + demand_status.format());
}

// ---------------------------------------------------------------------------
// 15. Conflicting generations are rejected
// ---------------------------------------------------------------------------

TEF_TEST(conflicting_generations_are_rejected) {
  const FabricSnapshot snapshot = build_snapshot(tef::test::simple_fabric());
  TEF_CHECK_MSG(validate_structure(snapshot).ok(), validate_structure(snapshot).format());

  FabricSnapshot capacity_conflict = snapshot;
  capacity_conflict.capacity.fabric_epoch = snapshot.fabric_epoch.next();
  const Status capacity_status = validate_structure(capacity_conflict);
  TEF_CHECK_MSG(capacity_status.code() == ErrorCode::conflicting_authority,
                "a capacity snapshot with a foreign fabric epoch returned " +
                    capacity_status.format());

  FabricSnapshot reservation_conflict = snapshot;
  reservation_conflict.reservations.fabric_epoch = snapshot.fabric_epoch.next();
  const Status reservation_status = validate_structure(reservation_conflict);
  TEF_CHECK_MSG(reservation_status.code() == ErrorCode::conflicting_authority,
                "a reservation snapshot with a foreign fabric epoch returned " +
                    reservation_status.format());
}

// ---------------------------------------------------------------------------
// 16. Arithmetic saturation is safe
// ---------------------------------------------------------------------------

TEF_TEST(arithmetic_saturation_is_safe) {
  SnapshotSpec spec;
  spec.resources = {{"res-huge", Limits::max_bandwidth, 0, {"domain-1"}}};
  spec.paths = {{"path-huge", {"res-huge"}, {"domain-1"}, Limits::max_cost, true,
                 Limits::max_latency_micros}};
  spec.demands = {{"demand-huge", "tenant-a", "class-gold", Limits::max_bandwidth,
                   Limits::max_bandwidth, Limits::max_bandwidth, 255}};

  // A single demand exactly saturates the resource: the arithmetic must neither
  // overflow nor truncate.
  const SolveOutcome exact = run_solver(build_snapshot(spec), SolveOptions{},
                                        "arithmetic saturation (single demand)");
  TEF_CHECK_EQ(exact.status, FeasibilityStatus::feasible);
  TEF_CHECK_MSG(exact.verified, "the saturated allocation was not independently verified");
  TEF_CHECK_MSG(exact.allocation.total_granted == Limits::max_bandwidth,
                "a fully saturated demand was granted " +
                    std::to_string(exact.allocation.total_granted) + " instead of " +
                    std::to_string(Limits::max_bandwidth));
  check_allocation_bounds(exact.allocation, "arithmetic saturation (single demand)");
  for (const auto& resource : exact.allocation.resources) {
    TEF_CHECK_MSG(resource.allocated <= resource.usable_capacity,
                  "saturated resource " + resource.resource.str() + " allocated " +
                      std::to_string(resource.allocated) + " of " +
                      std::to_string(resource.usable_capacity));
    TEF_CHECK_MSG(resource.utilization_permille <= 1000,
                  "saturated resource " + resource.resource.str() + " reports " +
                      std::to_string(resource.utilization_permille) + " permille");
  }

  // Two demands of Limits::max_bandwidth on one resource of Limits::max_bandwidth
  // cannot both be served; the outcome must be typed and bounded.
  SnapshotSpec shared = spec;
  shared.demands.push_back({"demand-huge-two", "tenant-b", "class-gold", Limits::max_bandwidth,
                            Limits::max_bandwidth, Limits::max_bandwidth, 100});
  const SolveOutcome saturated =
      run_solver(build_snapshot(shared), SolveOptions{}, "arithmetic saturation (shared resource)");
  TEF_CHECK_MSG(saturated.status != FeasibilityStatus::feasible,
                "two demands of Limits::max_bandwidth were both reported feasible on one "
                "resource of Limits::max_bandwidth");
  TEF_CHECK_MSG(is_definitively_infeasible(saturated.status),
                "the saturated fabric returned status=" + std::string(to_string(saturated.status)));
  check_allocation_bounds(saturated.allocation, "arithmetic saturation (shared resource)");
}

// ---------------------------------------------------------------------------
// 17. A bounded solver budget is honoured
// ---------------------------------------------------------------------------

TEF_TEST(bounded_solver_budget_is_honoured) {
  for (std::uint64_t seed = 1; seed <= kSeededCases; ++seed) {
    const Population population = population_for(seed);
    const FabricSnapshot snapshot =
        build_snapshot(random_fabric(seed, population.demands, population.paths,
                                     population.resources));
    SolveOptions options;
    options.max_iterations = 1;
    const std::string where = context(seed) + " max_iterations=1";

    const Result<SolveOutcome> result = solve(snapshot, options);
    TEF_CHECK_MSG(result.has_value(),
                  where + ": solve did not return a typed outcome: " + result.error().format());
    if (!result.has_value()) continue;
    const SolveOutcome& outcome = result.value();
    check_allocation_bounds(outcome.allocation, where);
    // A bounded call must be reproducible: the same snapshot and the same budget
    // produce the same status and the same allocation, never a different answer
    // on a different run.
    const Result<SolveOutcome> repeated = solve(snapshot, options);
    TEF_CHECK_MSG(repeated.has_value(),
                  where + ": the repeated bounded solve did not return a typed outcome: " +
                      repeated.error().format());
    if (repeated.has_value()) {
      TEF_CHECK_EQ(outcome.status, repeated.value().status);
      TEF_CHECK_MSG(outcome.allocation_digest.hex() == repeated.value().allocation_digest.hex(),
                    where + ": the bounded solve is not reproducible (" +
                        outcome.allocation_digest.hex() + " != " +
                        repeated.value().allocation_digest.hex() + ")");
    }
    if (carries_allocation(outcome.status)) {
      const std::vector<BindingConstraint> violations =
          verify_allocation(snapshot, outcome.allocation);
      TEF_CHECK_MSG(violations.empty(),
                    where + " status=" + std::string(to_string(outcome.status)) +
                        " carries an allocation the verifier rejects: " +
                        describe_bindings(violations));
    }
    if (outcome.status == FeasibilityStatus::feasible) {
      TEF_CHECK_MSG(outcome.verified, where + " reported FEASIBLE without being verified");
    }
  }
}

// ---------------------------------------------------------------------------
// 18. Worst-case fan-out inside the documented supported bounds
// ---------------------------------------------------------------------------

TEF_TEST(worst_case_fanout_within_supported_limits) {
  const SnapshotSpec spec = random_fabric(20260101, 256, 16, 256);
  TEF_CHECK_EQ(spec.demands.size(), std::size_t{256});
  TEF_CHECK_EQ(spec.paths.size(), std::size_t{16});
  TEF_CHECK_EQ(spec.resources.size(), std::size_t{256});
  const FabricSnapshot snapshot = build_snapshot(spec);
  TEF_CHECK_MSG(validate_structure(snapshot).ok(), validate_structure(snapshot).format());

  const SolveOutcome outcome =
      run_solver(snapshot, SolveOptions{}, "worst-case fan-out (256/16/256)");
  TEF_CHECK_MSG(outcome.verified || is_definitively_infeasible(outcome.status),
                "worst-case fan-out produced status=" + std::string(to_string(outcome.status)) +
                    " with verified=" + std::string(outcome.verified ? "true" : "false"));
  if (outcome.verified) {
    TEF_CHECK_MSG(verify_allocation(snapshot, outcome.allocation).empty(),
                  "the worst-case allocation fails independent verification");
  }
  check_allocation_bounds(outcome.allocation, "worst-case fan-out (256/16/256)");
  TEF_CHECK_MSG(outcome.iterations <= Limits::max_solver_iterations,
                "the worst-case solve exceeded the documented iteration bound");
}

// ---------------------------------------------------------------------------
// 19. Malformed snapshot payloads are rejected
// ---------------------------------------------------------------------------

TEF_TEST(malformed_snapshot_payloads_are_rejected) {
  const FabricSnapshot snapshot = build_snapshot(tef::test::simple_fabric());
  const std::vector<std::byte> payload = encode_snapshot(snapshot);
  TEF_CHECK_MSG(payload.size() > 64, "the encoded snapshot is implausibly small");

  // Round-trip stability: a well-formed payload decodes and re-encodes verbatim.
  {
    const Result<FabricSnapshot> decoded = decode_snapshot(payload);
    TEF_CHECK_MSG(decoded.has_value(), "the fixture payload did not decode: " +
                                           (decoded.has_value() ? std::string()
                                                                : decoded.error().format()));
    if (decoded.has_value()) {
      TEF_CHECK_MSG(encode_snapshot(decoded.value()) == payload,
                    "the encoded snapshot is not stable across a decode/encode round trip");
    }
  }

  // Truncation at every offset must be rejected; a shorter payload can never be
  // a partially valid snapshot.
  for (std::size_t length = 0; length < payload.size(); ++length) {
    const std::span<const std::byte> truncated(payload.data(), length);
    const Result<FabricSnapshot> decoded = decode_snapshot(truncated);
    TEF_CHECK_MSG(!decoded.has_value(),
                  "a payload truncated to " + std::to_string(length) + " of " +
                      std::to_string(payload.size()) + " bytes decoded successfully");
  }

  // Trailing bytes are a structural defect, not a tolerated suffix.
  for (std::size_t extra = 1; extra <= 8; ++extra) {
    std::vector<std::byte> extended = payload;
    extended.insert(extended.end(), extra, std::byte{0x5A});
    const Result<FabricSnapshot> decoded = decode_snapshot(extended);
    TEF_CHECK_MSG(!decoded.has_value(),
                  "a payload with " + std::to_string(extra) + " trailing bytes decoded");
    TEF_CHECK_MSG(decoded.error().code() == ErrorCode::trailing_input,
                  "trailing bytes returned " + decoded.error().format());
  }

  // Format-version corruption is never mistaken for a newer or older format.
  for (std::size_t offset = 0; offset < 2; ++offset) {
    std::vector<std::byte> mangled = payload;
    mangled[offset] = static_cast<std::byte>(std::to_integer<unsigned>(mangled[offset]) ^ 0xFFu);
    const Result<FabricSnapshot> decoded = decode_snapshot(mangled);
    TEF_CHECK_MSG(!decoded.has_value(),
                  "a corrupted format version decoded successfully");
    TEF_CHECK_MSG(decoded.error().code() == ErrorCode::unsupported_version,
                  "a corrupted format version returned " + decoded.error().format());
  }

  // Single-byte corruption anywhere must either be rejected or decode into a
  // snapshot that still passes full structural validation - never a partially
  // valid one.
  const std::size_t stride = payload.size() > 4096 ? 16 : 1;
  for (std::size_t offset = 2; offset < payload.size(); offset += stride) {
    std::vector<std::byte> mangled = payload;
    mangled[offset] = static_cast<std::byte>(std::to_integer<unsigned>(mangled[offset]) ^ 0x01u);
    const Result<FabricSnapshot> decoded = decode_snapshot(mangled);
    if (decoded.has_value()) {
      TEF_CHECK_MSG(validate_structure(decoded.value()).ok(),
                    "corruption at byte " + std::to_string(offset) +
                        " decoded into a structurally invalid snapshot: " +
                        validate_structure(decoded.value()).format());
    }
  }
}

// ---------------------------------------------------------------------------
// 20. Missing provenance is rejected and never promoted
// ---------------------------------------------------------------------------

TEF_TEST(missing_provenance_is_rejected_but_never_promoted) {
  const FabricSnapshot snapshot = build_snapshot(tef::test::simple_fabric());
  TEF_CHECK_MSG(validate_structure(snapshot).ok(), validate_structure(snapshot).format());
  TEF_CHECK_MSG(!snapshot.demands.empty(), "the fixture declares no demand");

  FabricSnapshot stripped = snapshot;
  stripped.demands.front().provenance.source = SourceSystemId{};  // demands are in canonical order
  const std::string demand_id = stripped.demands.front().id.str();

  const Status structural = validate_structure(stripped);
  TEF_CHECK_MSG(structural.code() == ErrorCode::missing_provenance,
                "demand " + demand_id + " without provenance returned " + structural.format());
  TEF_CHECK_MSG(!structural.ok(), "an unlabelled demand was accepted as structurally valid");

  const Result<FabricSnapshot> decoded = decode_snapshot(encode_snapshot(stripped));
  TEF_CHECK_MSG(!decoded.has_value(),
                "an unlabelled demand set was decoded into a usable snapshot");
  TEF_CHECK_MSG(validate_structure(snapshot).ok(),
                "the unmodified fixture no longer validates, so the rejection is not attributable "
                "to the cleared provenance");
}

int main(int argc, char** argv) { return tef::test::run_all(argc, argv); }
