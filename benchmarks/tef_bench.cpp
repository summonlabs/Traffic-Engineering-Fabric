// Traffic Engineering Fabric - synthetic planning benchmark.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// This harness builds purely synthetic populations through the public API and
// measures COMPLETED PLANNING WORK: it times tef::solve(snapshot, options) only,
// and reports the time taken to build and canonicalize the snapshot in a
// separate column. Enqueue, submission and transport latency are never measured
// here and are never reported as planning throughput.
//
// Every configuration is solved more than once. Every solve of the same
// configuration must return an identical FeasibilityStatus and an identical
// allocation digest; any disagreement is reported as a DETERMINISM VIOLATION and
// the process exits non-zero. That check is the point of the harness as much as
// the timings are.
//
// The harness links only TrafficEngineeringFabric::tef, includes only the public
// headers, and depends on no third party code. Randomness comes from a local
// splitmix64 generator so that a seed reproduces a population exactly.

#include "tef/inspect.hpp"
#include "tef/tef.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tef::benchmark {
namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

// Lower median, so that an even repetition count stays deterministic and never
// invents a value that was not observed.
double median_of(std::vector<double> values) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  return values[(values.size() - 1) / 2];
}

double minimum_of(const std::vector<double>& values) {
  if (values.empty()) return 0.0;
  return *std::min_element(values.begin(), values.end());
}

std::string fixed3(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(3) << value;
  return out.str();
}

// ---------------------------------------------------------------------------
// Local splitmix64 PRNG. <random> is deliberately not used: the same seed must
// reproduce the same population on every platform and with every standard
// library.
// ---------------------------------------------------------------------------

class Splitmix64 {
 public:
  explicit Splitmix64(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  std::uint64_t below(std::uint64_t bound) noexcept {
    if (bound == 0) return 0;
    return next() % bound;
  }

  // Inclusive range [low, high].
  std::uint64_t range(std::uint64_t low, std::uint64_t high) noexcept {
    if (high <= low) return low;
    return low + below(high - low + 1u);
  }

 private:
  std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// Synthetic population generator (local to the benchmark: the test-support
// fixtures are not linked here).
// ---------------------------------------------------------------------------

template <class IdT>
IdT make_id(std::string_view text) {
  return IdT::parse(text).value();
}

Provenance provenance_of(std::string_view system, std::uint64_t generation) {
  Provenance value;
  value.source = SourceSystemId::parse(system).value();
  value.source_generation = generation;
  value.observed_tick = generation;
  value.evidence_digest = Sha256::hash(std::string(system) + "#" + std::to_string(generation));
  value.detail = std::string(system) + " synthetic evidence";
  return value;
}

constexpr std::size_t kTermTableSize = 5;
constexpr std::size_t kTenantCount = 4;
constexpr std::size_t kServiceClassCount = 3;

const char* const kTenantNames[kTenantCount] = {"tenant-alpha", "tenant-beta", "tenant-gamma",
                                                "tenant-delta"};
const char* const kServiceClassNames[kServiceClassCount] = {"class-gold", "class-silver",
                                                             "class-bronze"};

// Fixed, ordered objective-term table. The objective-complexity axis takes the
// first N entries; every term is a real ObjectiveTerm from the public model.
struct TermChoice {
  ObjectiveTerm term;
  std::int64_t weight;
};

const TermChoice kTermTable[kTermTableSize] = {
    {ObjectiveTerm::satisfy_minimums, 1000},
    {ObjectiveTerm::maximize_desired_bandwidth, 10},
    {ObjectiveTerm::minimize_total_path_cost, 1},
    {ObjectiveTerm::minimize_congestion_exposure, 1},
    {ObjectiveTerm::fairness_across_groups, 1},
};

Policy synthetic_policy() {
  Policy policy;
  policy.id = make_id<PolicyId>("policy-synthetic");
  policy.generation = PolicyGeneration::parse(1).value();
  policy.provenance = provenance_of("tef-benchmark-config", 1);
  policy.allow_preemption = false;
  policy.require_minimums = true;
  policy.allow_degraded_commit = false;
  policy.require_failure_domain_diversity = false;
  policy.max_utilization_permille = 1000;
  policy.churn_improvement_threshold_permille = 0;
  policy.churn_max_moved_bandwidth_permille = 1000;
  policy.churn_max_operational_risk_permille = 1000;
  policy.churn_max_affected_demands = 0;
  policy.max_paths_per_demand = 0;
  return policy;
}

ObjectiveProfile synthetic_objective(std::size_t term_count, bool churn_aware) {
  ObjectiveProfile profile;
  profile.id = make_id<ObjectiveProfileId>("objective-synthetic");
  profile.generation = ObjectiveProfileGeneration::parse(1).value();
  profile.provenance = provenance_of("tef-benchmark-config", 1);
  const std::size_t count = std::min<std::size_t>(term_count, kTermTableSize);
  for (std::size_t i = 0; i < count; ++i) {
    profile.terms.push_back(ObjectiveWeight{kTermTable[i].term, kTermTable[i].weight});
  }
  if (churn_aware) profile.terms.push_back(ObjectiveWeight{ObjectiveTerm::minimize_churn, 1});
  return profile;
}

struct PopulationSpec {
  std::size_t demand_count = 1;
  std::size_t path_count = 1;
  std::size_t resource_count = 1;
  std::size_t reservation_percent = 0;
  std::size_t objective_terms = 1;
  std::uint64_t seed = 1;
  std::uint64_t epoch = 1;
  std::int64_t capacity_permille = 1000;
  std::uint64_t cost_shift = 0;
  bool churn_objective = false;
};

// Builds one synthetic fabric snapshot. Every element carries valid provenance,
// the identity alphabet is respected, and the result is canonicalized before it
// is returned. The fabric is deliberately over-provisioned: each resource can
// carry the whole aggregate hard floor of the population, so the sweep measures
// planning work rather than infeasibility certification.
FabricSnapshot build_synthetic_snapshot(const PopulationSpec& spec) {
  FabricSnapshot snapshot;
  snapshot.fabric_epoch = FabricEpoch::parse(spec.epoch).value();
  snapshot.topology_generation = TopologyGeneration::parse(1).value();
  snapshot.link_state_generation = LinkStateGeneration::parse(1).value();
  snapshot.path_authority_generation = PathAuthorityGeneration::parse(1).value();
  snapshot.failure_domain_generation = FailureDomainGeneration::parse(1).value();
  snapshot.provenance = provenance_of("fabric-topology", spec.epoch);
  snapshot.evaluation_tick = 0;

  snapshot.candidate_set.id = make_id<CandidateSetId>("candidates-synthetic");
  snapshot.candidate_set.generation = CandidateSetGeneration::parse(1).value();

  snapshot.capacity.id = make_id<CapacitySnapshotId>("capacity-synthetic");
  snapshot.capacity.generation = CapacitySnapshotGeneration::parse(1).value();
  snapshot.capacity.fabric_epoch = snapshot.fabric_epoch;
  snapshot.capacity.provenance = provenance_of("link-state-fabric", spec.epoch);

  snapshot.reservations.id = make_id<ReservationSnapshotId>("reservations-synthetic");
  snapshot.reservations.generation = ReservationSnapshotGeneration::parse(1).value();
  snapshot.reservations.fabric_epoch = snapshot.fabric_epoch;
  snapshot.reservations.provenance = provenance_of("bandwidth-reservation-fabric", spec.epoch);

  snapshot.policy = synthetic_policy();
  snapshot.objective = synthetic_objective(spec.objective_terms, spec.churn_objective);

  const std::size_t demand_count = spec.demand_count == 0 ? 1 : spec.demand_count;
  const std::size_t path_count = spec.path_count == 0 ? 1 : spec.path_count;
  const std::size_t resource_count = spec.resource_count == 0 ? 1 : spec.resource_count;

  struct DemandShape {
    std::int64_t minimum = 0;
    std::int64_t desired = 0;
    std::int64_t maximum = 0;
    std::uint8_t priority = 0;
    std::size_t tenant = 0;
    std::size_t service_class = 0;
  };

  // Demand bandwidths are drawn first so that resource capacity can be sized to
  // the population's aggregate hard floor.
  Splitmix64 demand_rng(spec.seed ^ 0x1F3A5C7D9B0E2468ull);
  std::vector<DemandShape> shapes;
  shapes.reserve(demand_count);
  std::int64_t total_floor = 0;
  for (std::size_t i = 0; i < demand_count; ++i) {
    DemandShape shape;
    shape.desired = 200 + static_cast<std::int64_t>(demand_rng.range(0, 800));
    const std::int64_t quarter = shape.desired / 4;
    shape.minimum =
        quarter + static_cast<std::int64_t>(demand_rng.range(0, static_cast<std::uint64_t>(quarter)));
    shape.maximum = shape.desired + static_cast<std::int64_t>(demand_rng.range(
                                       0, static_cast<std::uint64_t>(shape.desired / 2)));
    shape.priority = static_cast<std::uint8_t>(demand_rng.range(1, 254));
    shape.tenant = static_cast<std::size_t>(demand_rng.range(0, kTenantCount - 1));
    shape.service_class = static_cast<std::size_t>(demand_rng.range(0, kServiceClassCount - 1));
    total_floor += shape.minimum;
    shapes.push_back(shape);
  }

  // Capacity sizing. The fabric is provisioned so that the aggregate available
  // bandwidth comfortably covers the aggregate hard floor, while no single
  // candidate path can swallow the whole population. That keeps every synthetic
  // population feasible (so the sweep measures planning work, not infeasibility
  // certification) and still makes the solver spread load across several paths.
  const std::int64_t floor_total = std::max<std::int64_t>(total_floor, 1);
  const std::int64_t carry = static_cast<std::int64_t>(
      std::min<std::size_t>(std::max<std::size_t>(path_count, 1), 3));
  const std::int64_t resources_i = static_cast<std::int64_t>(resource_count);
  const std::int64_t per_resource = std::max((2 * floor_total + carry - 1) / carry,
                                             (2 * floor_total + resources_i - 1) / resources_i);
  const std::int64_t scaled = per_resource * spec.capacity_permille / 1000;

  Splitmix64 resource_rng(spec.seed ^ 0x2B7E151628AED2A6ull);
  for (std::size_t i = 0; i < resource_count; ++i) {
    FabricResource resource;
    resource.id = make_id<ResourceId>("res-" + std::to_string(i));
    resource.generation = ResourceGeneration::parse(1).value();
    resource.provenance = provenance_of("link-state-fabric", spec.epoch);
    const std::int64_t jitter = static_cast<std::int64_t>(
        resource_rng.range(0, static_cast<std::uint64_t>(std::max<std::int64_t>(scaled / 4, 1))));
    resource.usable_capacity = scaled + jitter;
    resource.committed_load = resource.usable_capacity / 20;
    resource.failure_domains.push_back(
        make_id<FailureDomainId>("domain-" + std::to_string(resource_rng.range(0, 3))));
    snapshot.capacity.resources.push_back(std::move(resource));
  }

  // Non-preemptible resource reservations: exactly floor(resources * percent /
  // 100) of them, selected by a deterministic partial Fisher-Yates shuffle.
  const std::size_t reservation_target = resource_count * spec.reservation_percent / 100;
  const std::size_t reservation_count = std::min(reservation_target, resource_count);
  std::vector<std::size_t> order(resource_count);
  for (std::size_t i = 0; i < resource_count; ++i) order[i] = i;
  Splitmix64 reservation_rng(spec.seed ^ 0x3C6EF372FE94F82Bull);
  for (std::size_t i = 0; i < reservation_count; ++i) {
    const std::size_t pick = i + static_cast<std::size_t>(reservation_rng.range(
                                     0, static_cast<std::uint64_t>(resource_count - 1 - i)));
    std::swap(order[i], order[pick]);
  }
  std::vector<std::size_t> chosen(order.begin(),
                                  order.begin() + static_cast<std::ptrdiff_t>(reservation_count));
  std::sort(chosen.begin(), chosen.end());
  for (const std::size_t index : chosen) {
    Reservation reservation;
    reservation.id = make_id<ReservationId>("resv-r" + std::to_string(index));
    reservation.generation = ReservationGeneration::parse(1).value();
    reservation.provenance = provenance_of("bandwidth-reservation-fabric", spec.epoch);
    reservation.resources.push_back(snapshot.capacity.resources[index].id);
    reservation.bandwidth = snapshot.capacity.resources[index].usable_capacity / 4;
    reservation.preemptibility = Preemptibility::not_preemptible;
    reservation.priority = 200;
    reservation.effective.start_tick = 0;
    reservation.effective.end_tick = 0;
    reservation.effective.open_ended = true;
    snapshot.reservations.reservations.push_back(std::move(reservation));
  }

  Splitmix64 path_rng(spec.seed ^ 0x4D5A6E7F8A9B0C1Dull);
  for (std::size_t i = 0; i < path_count; ++i) {
    CandidatePath path;
    path.id = make_id<PathId>("path-" + std::to_string(i));
    path.generation = PathGeneration::parse(1).value();
    path.authority.id = path.id;
    path.authority.generation = snapshot.path_authority_generation;
    path.candidate_set = snapshot.candidate_set;
    path.provenance = provenance_of("path-authority", 1);
    path.cost = 1 + static_cast<std::int64_t>((static_cast<std::uint64_t>(i) + spec.cost_shift) % 32u);
    path.latency_micros = 100 + static_cast<std::int64_t>(path_rng.range(0, 9000));
    path.scope = EligibilityScope::fabric_wide;
    const std::size_t member_count = 1 + static_cast<std::size_t>(path_rng.range(0, 2));
    for (std::size_t m = 0; m < member_count; ++m) {
      const std::size_t index =
          static_cast<std::size_t>(path_rng.range(0, static_cast<std::uint64_t>(resource_count - 1)));
      const ResourceId candidate = snapshot.capacity.resources[index].id;
      if (std::find(path.resources.begin(), path.resources.end(), candidate) ==
          path.resources.end()) {
        path.resources.push_back(candidate);
      }
    }
    if (path.resources.empty()) path.resources.push_back(snapshot.capacity.resources[0].id);
    path.failure_domains.push_back(
        make_id<FailureDomainId>("domain-" + std::to_string(path_rng.range(0, 3))));
    snapshot.paths.push_back(std::move(path));
  }

  for (std::size_t i = 0; i < demand_count; ++i) {
    const DemandShape& shape = shapes[i];
    Demand demand;
    demand.id = make_id<DemandId>("demand-" + std::to_string(i));
    demand.generation = DemandGeneration::parse(1).value();
    demand.provenance = provenance_of("network-admission-fabric", 1);
    demand.tenant = make_id<TenantId>(kTenantNames[shape.tenant]);
    demand.service_class = make_id<ServiceClassId>(kServiceClassNames[shape.service_class]);
    demand.minimum_bandwidth = shape.minimum;
    demand.desired_bandwidth = shape.desired;
    demand.maximum_bandwidth = shape.maximum;
    demand.priority = shape.priority;
    demand.candidate_set = snapshot.candidate_set;
    demand.effective.start_tick = 0;
    demand.effective.end_tick = 0;
    demand.effective.open_ended = true;
    demand.preemptibility = Preemptibility::not_preemptible;
    snapshot.demands.push_back(std::move(demand));
  }

  (void)canonicalize(snapshot);
  return snapshot;
}

// ---------------------------------------------------------------------------
// Configuration model and bounded clamping
// ---------------------------------------------------------------------------

struct Config {
  std::size_t demand_count = 1;
  std::size_t path_count = 1;
  std::size_t resource_count = 1;
  std::size_t reservation_percent = 0;
  std::size_t objective_terms = 1;
  bool churn_aware_objective = false;
  bool supply_incumbent = false;
};

// Clamps a requested configuration to the library's published bounds. Every
// clamp is reported through 'notes' so that the output always says when a
// requested size was reduced.
Config clamp_config(const Config& requested, std::vector<std::string>& notes) {
  Config clamped = requested;
  if (clamped.demand_count < 1) {
    notes.push_back("demands requested=0 clamped_to=1 (a population must declare at least one demand)");
    clamped.demand_count = 1;
  }
  if (clamped.demand_count > Limits::max_demands) {
    notes.push_back("demands requested=" + std::to_string(requested.demand_count) +
                    " clamped_to=" + std::to_string(Limits::max_demands) +
                    " (tef::Limits::max_demands)");
    clamped.demand_count = Limits::max_demands;
  }
  if (clamped.path_count < 1) {
    notes.push_back("paths_per_demand requested=0 clamped_to=1 (a candidate set must hold at least one path)");
    clamped.path_count = 1;
  }
  if (clamped.path_count > Limits::max_paths_per_demand) {
    notes.push_back("paths_per_demand requested=" + std::to_string(requested.path_count) +
                    " clamped_to=" + std::to_string(Limits::max_paths_per_demand) +
                    " (tef::Limits::max_paths_per_demand)");
    clamped.path_count = Limits::max_paths_per_demand;
  }
  if (clamped.resource_count < 1) {
    notes.push_back("resources requested=0 clamped_to=1 (a capacity snapshot must hold at least one resource)");
    clamped.resource_count = 1;
  }
  if (clamped.resource_count > Limits::max_resources) {
    notes.push_back("resources requested=" + std::to_string(requested.resource_count) +
                    " clamped_to=" + std::to_string(Limits::max_resources) +
                    " (tef::Limits::max_resources)");
    clamped.resource_count = Limits::max_resources;
  }
  if (clamped.reservation_percent > 100) {
    notes.push_back("reservation_percent requested=" + std::to_string(requested.reservation_percent) +
                    " clamped_to=100 (a resource cannot carry more than one synthetic reservation)");
    clamped.reservation_percent = 100;
  }
  if (clamped.objective_terms < 1) {
    notes.push_back("terms requested=0 clamped_to=1 (an objective profile must declare at least one term)");
    clamped.objective_terms = 1;
  }
  if (clamped.objective_terms > kTermTableSize) {
    notes.push_back("terms requested=" + std::to_string(requested.objective_terms) +
                    " clamped_to=" + std::to_string(kTermTableSize) +
                    " (the benchmark's fixed objective-term table)");
    clamped.objective_terms = kTermTableSize;
  }
  return clamped;
}

PopulationSpec spec_for(const Config& config, std::uint64_t seed) {
  PopulationSpec spec;
  spec.demand_count = config.demand_count;
  spec.path_count = config.path_count;
  spec.resource_count = config.resource_count;
  spec.reservation_percent = config.reservation_percent;
  spec.objective_terms = config.objective_terms;
  spec.seed = seed;
  spec.epoch = 2;
  spec.capacity_permille = 1000;
  spec.cost_shift = 0;
  spec.churn_objective = config.churn_aware_objective;
  return spec;
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

struct HarnessOptions {
  bool quick = false;
  std::size_t repeats = 3;
  std::uint64_t seed = 1;
  bool help = false;
  std::string error;
  std::string repeat_note;
};

struct RowResult {
  std::size_t demand_count = 0;
  std::size_t path_count = 0;
  std::size_t resource_count = 0;
  std::size_t reservation_count = 0;
  std::size_t objective_terms = 0;
  bool supply_incumbent = false;
  std::uint64_t iterations = 0;
  FeasibilityStatus status = FeasibilityStatus::feasible;
  double plan_ms_median = 0.0;
  double plan_ms_min = 0.0;
  double build_ms_median = 0.0;
  double demands_per_sec = 0.0;
  std::size_t solves = 0;
  std::vector<std::string> notes;
};

struct RunOutcome {
  RowResult row;
  std::string error;      // non-empty: the harness cannot continue
  std::string violation;  // non-empty: determinism violation
};

// Bounded rendering of an infeasibility explanation, for the (synthetic, but
// possible) configurations that are not feasible.
std::string feasibility_note(const FeasibilityResult& feasibility) {
  std::istringstream lines(render_feasibility(feasibility));
  std::string line;
  std::string out;
  std::size_t emitted = 0;
  while (std::getline(lines, line)) {
    if (line.empty()) continue;
    if (emitted == 3) {
      out += " | ...";
      break;
    }
    if (!out.empty()) out += " | ";
    out += line;
    ++emitted;
  }
  return out;
}

std::string describe_config(const Config& config) {
  std::ostringstream out;
  out << "demands=" << config.demand_count << " paths_per_demand=" << config.path_count
      << " resources=" << config.resource_count
      << " reservation_percent=" << config.reservation_percent
      << " terms=" << config.objective_terms
      << " churn=" << (config.supply_incumbent ? "incumbent" : "none");
  return out.str();
}

std::string describe(const RowResult& row) {
  std::ostringstream out;
  out << "demands=" << row.demand_count << " paths_per_demand=" << row.path_count
      << " resources=" << row.resource_count << " reservations=" << row.reservation_count
      << " terms=" << row.objective_terms
      << " churn=" << (row.supply_incumbent ? "incumbent" : "none");
  return out.str();
}

std::string format_row(const RowResult& row) {
  std::ostringstream out;
  out << describe(row) << " iterations=" << row.iterations
      << " status=" << to_string(row.status) << " plan_ms=" << fixed3(row.plan_ms_median)
      << " plan_ms_min=" << fixed3(row.plan_ms_min) << " build_ms=" << fixed3(row.build_ms_median)
      << " demands_per_sec=" << static_cast<std::uint64_t>(row.demands_per_sec);
  return out.str();
}

RunOutcome execute_config(const Config& config, const HarnessOptions& options) {
  RunOutcome result;
  const PopulationSpec spec = spec_for(config, options.seed);

  // The incumbent, when one is requested, is planned from a synthetic previous
  // epoch (different path cost order, 75% of the nominal capacity). That plan is
  // preparation, not measured work, so it is deliberately excluded from every
  // timing below.
  Allocation incumbent;
  bool have_incumbent = false;
  if (config.supply_incumbent) {
    PopulationSpec previous = spec;
    previous.epoch = 1;
    previous.capacity_permille = 750;
    previous.cost_shift = 7;
    previous.churn_objective = false;
    const FabricSnapshot previous_snapshot = build_synthetic_snapshot(previous);
    SolveOptions previous_options;
    previous_options.max_iterations = Limits::max_solver_iterations;
    previous_options.has_incumbent = false;
    const Result<SolveOutcome> previous_outcome = solve(previous_snapshot, previous_options);
    if (!previous_outcome.has_value()) {
      result.error = "planning the synthetic previous-epoch incumbent failed: " +
                     previous_outcome.error().format();
      return result;
    }
    if (!carries_allocation(previous_outcome.value().status)) {
      result.error = "the synthetic previous-epoch incumbent is unusable: status=" +
                     std::string(to_string(previous_outcome.value().status));
      return result;
    }
    incumbent = previous_outcome.value().allocation;
    have_incumbent = true;
  }

  SolveOptions plan_options;
  plan_options.max_iterations = Limits::max_solver_iterations;
  plan_options.has_incumbent = have_incumbent;
  if (have_incumbent) plan_options.incumbent = incumbent;

  std::vector<double> plan_times;
  std::vector<double> build_times;
  std::vector<FeasibilityStatus> statuses;
  std::vector<Digest> digests;
  std::vector<std::uint64_t> iteration_counts;
  plan_times.reserve(options.repeats);
  build_times.reserve(options.repeats);

  FabricSnapshot snapshot;
  SolveOutcome first;
  bool have_first = false;
  bool structure_reported = false;

  const auto plan_once = [&](bool timed, const std::string& label) -> bool {
    const Clock::time_point plan_start = Clock::now();
    const Result<SolveOutcome> outcome = solve(snapshot, plan_options);
    const Clock::time_point plan_end = Clock::now();
    if (timed) plan_times.push_back(elapsed_ms(plan_start, plan_end));
    if (!outcome.has_value()) {
      result.error = "tef::solve failed (" + label + "): " + outcome.error().format();
      return false;
    }
    statuses.push_back(outcome.value().status);
    digests.push_back(outcome.value().allocation_digest);
    iteration_counts.push_back(outcome.value().iterations);
    if (!have_first) {
      first = outcome.value();
      have_first = true;
    }
    return true;
  };

  for (std::size_t rep = 0; rep < options.repeats; ++rep) {
    const Clock::time_point build_start = Clock::now();
    snapshot = build_synthetic_snapshot(spec);
    const Status structure = validate_structure(snapshot);
    const Clock::time_point build_end = Clock::now();
    build_times.push_back(elapsed_ms(build_start, build_end));
    if (!structure.ok()) {
      if (!structure_reported) {
        result.error = "the synthetic population failed structural validation: " + structure.format();
      }
      return result;
    }
    structure_reported = true;
    if (!plan_once(true, "timed")) return result;
  }

  // Determinism: two further solves of the same snapshot must reproduce the
  // status and the allocation digest exactly. Together with the timed solves this
  // gives 'repeats + 2' independent planning runs per configuration.
  if (!plan_once(false, "determinism-1")) return result;
  if (!plan_once(false, "determinism-2")) return result;

  for (std::size_t i = 1; i < statuses.size(); ++i) {
    if (statuses[i] != statuses[0] || !(digests[i] == digests[0])) {
      std::ostringstream out;
      out << describe_config(config) << " solve_1_status=" << to_string(statuses[0])
          << " solve_" << (i + 1) << "_status=" << to_string(statuses[i])
          << " solve_1_digest=" << digests[0].hex() << " solve_" << (i + 1)
          << "_digest=" << digests[i].hex();
      result.violation = out.str();
      return result;
    }
  }

  RowResult& row = result.row;
  row.demand_count = config.demand_count;
  row.path_count = config.path_count;
  row.resource_count = config.resource_count;
  row.reservation_count = snapshot.reservations.reservations.size();
  row.objective_terms = snapshot.objective.terms.size();
  row.supply_incumbent = config.supply_incumbent;
  row.iterations = first.iterations;
  row.status = statuses[0];
  row.plan_ms_median = median_of(plan_times);
  row.plan_ms_min = minimum_of(plan_times);
  row.build_ms_median = median_of(build_times);
  row.solves = statuses.size();
  row.demands_per_sec =
      row.plan_ms_median > 0.0
          ? (static_cast<double>(row.demand_count) * 1000.0 / row.plan_ms_median)
          : 0.0;

  for (std::size_t i = 1; i < iteration_counts.size(); ++i) {
    if (iteration_counts[i] != iteration_counts[0]) {
      row.notes.push_back("iteration count differed between identical solves: " +
                          std::to_string(iteration_counts[0]) + " vs " +
                          std::to_string(iteration_counts[i]));
      break;
    }
  }
  if (!carries_allocation(row.status)) {
    row.notes.push_back("infeasible outcome: " + feasibility_note(first.feasibility));
  }
  if (!first.verified) {
    row.notes.push_back("the allocation did NOT pass independent verification");
  }
  return result;
}

// ---------------------------------------------------------------------------
// Harness driver
// ---------------------------------------------------------------------------

const char* const kHeaderRow =
    "demands paths_per_demand resources reservations terms churn iterations status plan_ms "
    "plan_ms_min build_ms demands_per_sec";

Config baseline_config() {
  Config config;
  config.demand_count = 64;
  config.path_count = 16;
  config.resource_count = 64;
  config.reservation_percent = 10;
  config.objective_terms = 3;
  config.churn_aware_objective = false;
  config.supply_incumbent = false;
  return config;
}

struct SweepValues {
  std::vector<std::size_t> demands;
  std::vector<std::size_t> paths;
  std::vector<std::size_t> resources;
  std::vector<std::size_t> reservation_percent;
  std::vector<std::size_t> terms;
};

SweepValues sweep_values(bool quick) {
  SweepValues values;
  if (quick) {
    values.demands = {16, 64};
    values.paths = {1, 4, 16};
    values.resources = {8, 64};
    values.reservation_percent = {0, 50};
    values.terms = {1, 3};
  } else {
    values.demands = {16, 64, 256, 1024, 4096};
    values.paths = {1, 4, 16, 64};
    values.resources = {8, 64, 512, 4096};
    values.reservation_percent = {0, 10, 50};
    values.terms = {1, 3, 5};
  }
  return values;
}

struct Section {
  std::string title;
  std::string note;
  std::vector<Config> configs;
};

class Harness {
 public:
  explicit Harness(HarnessOptions options) : options_(std::move(options)) {}

  int run() {
    print_banner();
    int code = print_limits_section();
    if (code != 0) return code;
    code = print_baseline_population();
    if (code != 0) return code;

    const Clock::time_point harness_start = Clock::now();
    for (const Section& section : build_sections()) {
      code = run_section(section);
      if (code != 0) return code;
    }
    const double total_ms = elapsed_ms(harness_start, Clock::now());

    std::cout << "\n=== summary ===\n";
    std::cout << "configurations=" << configurations_ << " repeats=" << options_.repeats
              << " solves_per_configuration=" << (options_.repeats + 2)
              << " clamped_requests=" << clamped_ << " total_wall_ms=" << fixed3(total_ms) << "\n";
    std::cout << "result=OK\n";
    return 0;
  }

 private:
  void print_banner() const {
    std::cout << "=======================================================================\n";
    std::cout << "Traffic Engineering Fabric - synthetic planning benchmark\n";
    std::cout << "=======================================================================\n";
    std::cout << "*** SYNTHETIC DATA ONLY. Every population below is generated in        ***\n";
    std::cout << "*** process by a local splitmix64 generator. NONE of these numbers are ***\n";
    std::cout << "*** physical-network measurements; they characterise this harness, not ***\n";
    std::cout << "*** any real fabric.                                                   ***\n";
    std::cout << "=======================================================================\n";
    std::cout << "mode=" << (options_.quick ? "quick" : "full") << " seed=" << options_.seed
              << " repeats=" << options_.repeats << "\n";
    if (!options_.repeat_note.empty()) std::cout << "NOTE: " << options_.repeat_note << "\n";
    std::cout << "timing=std::chrono::steady_clock, single threaded, no watchdog, no timeout\n";
    std::cout << "plan_ms=median wall time of tef::solve(snapshot, options) ONLY "
                 "(completed planning work)\n";
    std::cout << "plan_ms_min=fastest of the same timed solves; build_ms=median time to construct + "
                 "canonicalize + structurally validate the snapshot\n";
    std::cout << "demands_per_sec=demands / (plan_ms / 1000), computed from the median planning "
                 "time\n";
    std::cout << "determinism=every configuration is solved repeats+2 times; every solve must "
                 "return an identical status and an identical allocation digest\n";
    std::cout << "iterations=SolveOutcome::iterations; it counts solver-internal repair and "
                 "augmentation steps, so a feasible population whose demands are all eligible on "
                 "every path can legitimately read 0\n";
    std::cout << "legend=paths_per_demand is the shared candidate-path catalog size and every demand "
                 "may use every path in it; reservations=floor(resources*percent/100) "
                 "non-preemptible resource reservations; churn=incumbent supplies an allocation "
                 "planned from a synthetic previous epoch\n";
  }

  int print_limits_section() const {
    std::cout << "\n=== SECTION limits ===\n";
    std::cout << "limits max_demands=" << Limits::max_demands
              << " max_paths_per_demand=" << Limits::max_paths_per_demand
              << " max_total_candidate_paths=" << Limits::max_total_candidate_paths
              << " max_resources=" << Limits::max_resources
              << " max_reservations=" << Limits::max_reservations
              << " max_solver_iterations=" << Limits::max_solver_iterations << "\n";
    Config probe;
    probe.demand_count = Limits::max_demands * 2;
    probe.path_count = Limits::max_paths_per_demand * 2;
    probe.resource_count = Limits::max_resources * 2;
    probe.reservation_percent = 250;
    probe.objective_terms = 9;
    std::vector<std::string> notes;
    const Config clamped = clamp_config(probe, notes);
    for (const std::string& note : notes) {
      std::cout << "NOTE: " << note << "\n";
    }
    std::cout << describe(RowResult{clamped.demand_count, clamped.path_count, clamped.resource_count,
                                    0, clamped.objective_terms, false, 0,
                                    FeasibilityStatus::feasible, 0.0, 0.0, 0.0, 0.0, 0, {}})
              << "\n";
    std::cout << "clamping=applied to every configuration below; a clamp is reported as NOTE\n";
    return 0;
  }

  int print_baseline_population() const {
    std::vector<std::string> notes;
    const Config baseline = clamp_config(baseline_config(), notes);
    const FabricSnapshot snapshot = build_synthetic_snapshot(spec_for(baseline, options_.seed));
    const Status structure = validate_structure(snapshot);
    if (!structure.ok()) {
      std::cerr << "ERROR: the baseline synthetic population failed structural validation: "
                << structure.format() << "\n";
      return 3;
    }
    std::cout << "\n=== SECTION baseline population (synthetic, seed=" << options_.seed << ") ===\n";
    std::istringstream lines(render_snapshot_summary(snapshot));
    std::string line;
    while (std::getline(lines, line)) {
      if (!line.empty()) std::cout << "# " << line << "\n";
    }
    for (const std::string& note : notes) std::cout << "NOTE: " << note << "\n";
    return 0;
  }

  std::vector<Section> build_sections() const {
    const Config baseline = baseline_config();
    const SweepValues values = sweep_values(options_.quick);
    std::vector<Section> sections;

    {
      Section section;
      section.title = "demands";
      section.note = "varies the demand count; every other axis stays at the baseline";
      for (const std::size_t demand_count : values.demands) {
        Config config = baseline;
        config.demand_count = demand_count;
        section.configs.push_back(config);
      }
      sections.push_back(std::move(section));
    }
    {
      Section section;
      section.title = "paths_per_demand";
      section.note = "varies the shared candidate-path catalog size; every other axis stays at the baseline";
      for (const std::size_t path_count : values.paths) {
        Config config = baseline;
        config.path_count = path_count;
        section.configs.push_back(config);
      }
      sections.push_back(std::move(section));
    }
    {
      Section section;
      section.title = "resources";
      section.note = "varies the shared-resource catalog size; every other axis stays at the baseline";
      for (const std::size_t resource_count : values.resources) {
        Config config = baseline;
        config.resource_count = resource_count;
        section.configs.push_back(config);
      }
      sections.push_back(std::move(section));
    }
    {
      Section section;
      section.title = "reservations";
      section.note = "varies the share of resources carrying a non-preemptible reservation; every other axis stays at the baseline";
      for (const std::size_t percent : values.reservation_percent) {
        Config config = baseline;
        config.reservation_percent = percent;
        section.configs.push_back(config);
      }
      sections.push_back(std::move(section));
    }
    {
      Section section;
      section.title = "objective_terms";
      section.note = "varies the number of objective terms; every other axis stays at the baseline";
      for (const std::size_t terms : values.terms) {
        Config config = baseline;
        config.objective_terms = terms;
        section.configs.push_back(config);
      }
      sections.push_back(std::move(section));
    }
    {
      Section section;
      section.title = "churn";
      section.note = "compares planning with no incumbent against planning with an incumbent allocation "
                     "produced from a synthetic previous epoch (that preparation plan is not timed); "
                     "both rows carry the same churn-aware objective, so only the incumbent differs";
      Config without_incumbent = baseline;
      without_incumbent.churn_aware_objective = true;
      without_incumbent.supply_incumbent = false;
      section.configs.push_back(without_incumbent);
      Config with_incumbent = baseline;
      with_incumbent.churn_aware_objective = true;
      with_incumbent.supply_incumbent = true;
      section.configs.push_back(with_incumbent);
      sections.push_back(std::move(section));
    }
    if (!options_.quick) {
      Section section;
      section.title = "combined";
      section.note = "stress row: every axis at its largest swept value at once (full mode only)";
      Config config;
      config.demand_count = Limits::max_demands;
      config.path_count = Limits::max_paths_per_demand;
      config.resource_count = 4096;
      config.reservation_percent = 50;
      config.objective_terms = kTermTableSize;
      config.churn_aware_objective = false;
      config.supply_incumbent = false;
      section.configs.push_back(config);
      sections.push_back(std::move(section));
    }
    return sections;
  }

  int run_section(const Section& section) {
    std::cout << "\n=== SECTION " << section.title << " ===\n";
    std::cout << "note: " << section.note << "\n";
    std::cout << kHeaderRow << "\n";
    for (const Config& requested : section.configs) {
      std::vector<std::string> notes;
      const Config config = clamp_config(requested, notes);
      for (const std::string& note : notes) {
        std::cout << "NOTE: " << note << "\n";
        ++clamped_;
      }
      const RunOutcome outcome = execute_config(config, options_);
      if (!outcome.error.empty()) {
        std::cerr << "ERROR: " << outcome.error << "\n";
        return 3;
      }
      if (!outcome.violation.empty()) {
        std::cout << "DETERMINISM VIOLATION " << outcome.violation << "\n";
        std::cerr << "DETERMINISM VIOLATION: identical inputs produced different outcomes; "
                     "the benchmark result is void\n";
        return 1;
      }
      std::cout << format_row(outcome.row) << "\n";
      for (const std::string& note : outcome.row.notes) std::cout << "# " << note << "\n";
      ++configurations_;
    }
    return 0;
  }

  HarnessOptions options_;
  std::size_t configurations_ = 0;
  std::size_t clamped_ = 0;
};

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

bool parse_u64(std::string_view text, std::uint64_t& out) {
  if (text.empty()) return false;
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) return false;
    value = value * 10u + digit;
  }
  out = value;
  return true;
}

std::string usage_text() {
  return std::string(
      "usage: tef_bench [--quick | --full] [--repeats=N] [--seed=N]\n"
      "\n"
      "  --quick      small sweeps (fast smoke run)\n"
      "  --full       large sweeps (default)\n"
      "  --repeats=N  timed repetitions per configuration (default 3, clamped to 1..1000)\n"
      "  --seed=N     population seed (default 1); the same seed reproduces the same populations\n"
      "  -h, --help   print this help\n"
      "\n"
      "Every population is synthetic. The harness measures tef::solve() only and exits\n"
      "non-zero if two solves of the same configuration disagree on status or allocation digest.\n");
}

bool parse_arguments(int argc, char** argv, HarnessOptions& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument == "--quick") {
      options.quick = true;
      continue;
    }
    if (argument == "--full") {
      options.quick = false;
      continue;
    }
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      continue;
    }
    if (argument.rfind("--repeats=", 0) == 0) {
      std::uint64_t value = 0;
      if (!parse_u64(argument.substr(10), value)) {
        options.error = "invalid --repeats value: " + std::string(argument);
        return false;
      }
      if (value == 0) {
        options.repeat_note = "--repeats=0 clamped to 1";
        value = 1;
      } else if (value > 1000) {
        options.repeat_note = "--repeats=" + std::to_string(value) + " clamped to 1000";
        value = 1000;
      }
      options.repeats = static_cast<std::size_t>(value);
      continue;
    }
    if (argument.rfind("--seed=", 0) == 0) {
      std::uint64_t value = 0;
      if (!parse_u64(argument.substr(7), value)) {
        options.error = "invalid --seed value: " + std::string(argument);
        return false;
      }
      options.seed = value;
      continue;
    }
    options.error = "unknown argument: " + std::string(argument);
    return false;
  }
  return true;
}

}  // namespace

// Entry point. Returns 0 on success, 1 on a determinism violation, 2 on a usage
// error and 3 when the harness itself cannot continue.
int run(int argc, char** argv) {
  HarnessOptions options;
  if (!parse_arguments(argc, argv, options)) {
    std::cerr << "error: " << options.error << "\n\n" << usage_text() << "\n";
    return 2;
  }
  if (options.help) {
    std::cout << usage_text() << "\n";
    return 0;
  }
  Harness harness(std::move(options));
  return harness.run();
}

}  // namespace tef::benchmark

int main(int argc, char** argv) { return tef::benchmark::run(argc, argv); }
