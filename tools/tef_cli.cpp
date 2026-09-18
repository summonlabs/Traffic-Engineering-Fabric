// Traffic Engineering Fabric - inspection and demonstration CLI.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// tef-cli is the shipped inspection and demonstration entry point for the
// library. It is deliberately a *client* of the public API: it never reaches
// into library internals, it never throws, and every failure path reports a
// typed tef::Error instead of a raw exception.
//
//   tef-cli demo
//   tef-cli plan --demands=N --paths-per-demand=N --resources=N --seed=N [--print-judgement]
//   tef-cli compare --incumbent-runs=N --seed=N
//   tef-cli inspect --state-dir=DIR
//   tef-cli selfcheck
//
// Every subcommand is deterministic: identical arguments produce byte-identical
// stdout. The population generator uses splitmix64, spelled out below, so a seed
// reproduces a population exactly on every platform and standard library
// (<random> is never used).
//
// The fabric populations used by demo, plan, compare and selfcheck are
// SYNTHETIC. They describe no real network, no real traffic and no real tenant.
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "tef/inspect.hpp"
#include "tef/tef.hpp"

using namespace tef;

namespace {

// ---------------------------------------------------------------------------
// Exit codes and output
// ---------------------------------------------------------------------------

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

constexpr const char* kProgramName = "tef-cli";

// The durable file names of PlanRepository. They are part of the on-disk format
// and are used here only to decide whether a directory holds a repository
// before it is opened: opening an uninitialised directory would create durable
// files, and "inspect" must not write.
constexpr const char* kManifestFileName = "manifest.tefm";
constexpr const char* kSnapshotFileName = "snapshot.tefs";
constexpr const char* kJournalFileName = "journal.tefj";

// Bounds for the inspect listing so that the report stays readable.
constexpr std::size_t kInspectListingLimit = 32;

void write_out(const std::string& text) {
  if (text.empty()) return;
  const std::size_t written = std::fwrite(text.data(), 1, text.size(), stdout);
  (void)written;
}

void write_error(const std::string& text) {
  if (text.empty()) return;
  const std::size_t written = std::fwrite(text.data(), 1, text.size(), stderr);
  (void)written;
}

std::string size_text(std::size_t value) { return std::to_string(value); }
std::string u64_text(std::uint64_t value) { return std::to_string(value); }
std::string i64_text(std::int64_t value) { return std::to_string(value); }
std::string bool_text(bool value) { return value ? "true" : "false"; }

// Reports a typed failure and returns the tool's failure exit code.
int report_failure(std::string_view context, const Error& error) {
  write_error(std::string(kProgramName) + ": " + std::string(context) + ": " + error.format() + "\n");
  return kExitFailure;
}

int report_usage_error(const std::string& detail) {
  write_error(std::string(kProgramName) + ": " + detail + "\n");
  return kExitUsage;
}

// ---------------------------------------------------------------------------
// Deterministic population randomness
// ---------------------------------------------------------------------------

// splitmix64. Fully specified here so that a seed reproduces a population on any
// platform: the library itself never generates a population, the tool does.
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  // Inclusive range. The modulo bias is irrelevant for a synthetic population
  // and keeps the generator free of floating point.
  std::uint64_t range(std::uint64_t low, std::uint64_t high) noexcept {
    if (high <= low) return low;
    return low + (next() % (high - low + 1));
  }

 private:
  std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// Small construction helpers
// ---------------------------------------------------------------------------

// Identity construction never throws here: an unparsable name yields an invalid
// identity, which validate_structure then rejects as a typed error.
template <class IdT>
IdT make_id(std::string_view name) {
  const std::optional<IdT> parsed = IdT::parse(name);
  return parsed.has_value() ? parsed.value() : IdT{};
}

template <class GenT>
GenT make_generation(std::uint64_t value) {
  const std::optional<GenT> parsed = GenT::parse(value);
  return parsed.has_value() ? parsed.value() : GenT{};
}

std::size_t digit_width(std::size_t highest) {
  std::size_t width = 1;
  while (highest >= 10) {
    highest /= 10;
    ++width;
  }
  return width;
}

// Zero padded so that lexical order equals numeric order, which keeps the
// canonical (sorted) order of the model identical to the generation order.
std::string indexed_name(std::string_view prefix, std::size_t index, std::size_t width) {
  const std::string digits = std::to_string(index);
  std::string padding;
  for (std::size_t i = digits.size(); i < width; ++i) padding.push_back('0');
  return std::string(prefix) + "-" + padding + digits;
}

Provenance make_provenance(std::string_view system, std::uint64_t generation) {
  Provenance value;
  value.source = make_id<SourceSystemId>(system);
  value.source_generation = generation;
  value.observed_tick = generation;
  value.evidence_digest = Sha256::hash(std::string(system) + "#" + std::to_string(generation));
  value.detail = std::string(system) + " evidence";
  return value;
}

// ---------------------------------------------------------------------------
// Synthetic fabric population
// ---------------------------------------------------------------------------

struct SyntheticFabricSpec {
  std::size_t demands = 8;
  std::size_t paths_per_demand = 3;
  std::size_t resources = 8;
  std::uint64_t seed = 1;
  std::uint64_t generation = 1;
};

constexpr std::size_t kFailureDomainCount = 4;

// Every cardinality is clamped into tef::Limits before it reaches the model: an
// oversized request is bounded, never an allocation failure.
SyntheticFabricSpec clamp_fabric_spec(const SyntheticFabricSpec& request) {
  SyntheticFabricSpec spec = request;
  spec.resources =
      std::min<std::size_t>(std::max<std::size_t>(spec.resources, 1), Limits::max_resources);
  spec.paths_per_demand = std::min<std::size_t>(std::max<std::size_t>(spec.paths_per_demand, 1),
                                                Limits::max_paths_per_demand);
  spec.demands =
      std::min<std::size_t>(std::max<std::size_t>(spec.demands, 1), Limits::max_demands);
  // The candidate-path bound is a product bound: demands * paths_per_demand must
  // stay inside max_total_candidate_paths.
  const std::size_t demands_by_paths =
      std::max<std::size_t>(Limits::max_total_candidate_paths / spec.paths_per_demand, 1);
  spec.demands = std::min<std::size_t>(spec.demands, demands_by_paths);
  if (spec.generation == 0) spec.generation = 1;  // zero is never a legal generation
  return spec;
}

// Builds a small, self-contained, structurally valid fabric. The population is a
// pure function of (demands, paths_per_demand, resources, seed): the topology is
// a deterministic ring-ish spread, every path carries two to four resources, and
// the first two demands carry a small reservation obligation.
Result<FabricSnapshot> build_synthetic_fabric(const SyntheticFabricSpec& request) {
  const SyntheticFabricSpec spec = clamp_fabric_spec(request);

  FabricSnapshot snapshot;
  snapshot.fabric_epoch = make_generation<FabricEpoch>(spec.generation);
  snapshot.topology_generation = make_generation<TopologyGeneration>(spec.generation);
  snapshot.link_state_generation = make_generation<LinkStateGeneration>(spec.generation);
  snapshot.path_authority_generation = make_generation<PathAuthorityGeneration>(spec.generation);
  snapshot.failure_domain_generation = make_generation<FailureDomainGeneration>(spec.generation);
  snapshot.provenance = make_provenance("fabric-topology", spec.generation);
  snapshot.evaluation_tick = 0;

  snapshot.candidate_set.id = make_id<CandidateSetId>("candidates-synthetic");
  snapshot.candidate_set.generation = make_generation<CandidateSetGeneration>(spec.generation);

  snapshot.capacity.id = make_id<CapacitySnapshotId>("capacity-synthetic");
  snapshot.capacity.generation = make_generation<CapacitySnapshotGeneration>(spec.generation);
  snapshot.capacity.fabric_epoch = snapshot.fabric_epoch;
  snapshot.capacity.provenance = make_provenance("link-state-fabric", spec.generation);

  snapshot.reservations.id = make_id<ReservationSnapshotId>("reservations-synthetic");
  snapshot.reservations.generation =
      make_generation<ReservationSnapshotGeneration>(spec.generation);
  snapshot.reservations.fabric_epoch = snapshot.fabric_epoch;
  snapshot.reservations.provenance =
      make_provenance("bandwidth-reservation-fabric", spec.generation);

  snapshot.policy.id = make_id<PolicyId>("policy-synthetic");
  snapshot.policy.generation = make_generation<PolicyGeneration>(spec.generation);
  snapshot.policy.provenance = make_provenance("tef-config", spec.generation);
  snapshot.policy.allow_preemption = false;
  snapshot.policy.require_minimums = true;
  snapshot.policy.allow_degraded_commit = false;
  snapshot.policy.max_utilization_permille = 1000;
  snapshot.policy.churn_improvement_threshold_permille = 0;
  snapshot.policy.churn_max_moved_bandwidth_permille = 1000;
  snapshot.policy.churn_max_operational_risk_permille = 1000;

  snapshot.objective.id = make_id<ObjectiveProfileId>("objective-synthetic");
  snapshot.objective.generation = make_generation<ObjectiveProfileGeneration>(spec.generation);
  snapshot.objective.provenance = make_provenance("tef-config", spec.generation);
  snapshot.objective.terms = {{ObjectiveTerm::satisfy_minimums, 1000},
                              {ObjectiveTerm::maximize_desired_bandwidth, 1},
                              {ObjectiveTerm::minimize_total_path_cost, 1}};

  SplitMix64 rng(spec.seed);

  const std::size_t resource_width = digit_width(spec.resources - 1);
  for (std::size_t index = 0; index < spec.resources; ++index) {
    FabricResource resource;
    resource.id = make_id<ResourceId>(indexed_name("res", index, resource_width));
    resource.generation = make_generation<ResourceGeneration>(spec.generation);
    resource.usable_capacity = static_cast<std::int64_t>(4000 + rng.range(0, 24000));
    resource.committed_load = static_cast<std::int64_t>(
        rng.range(0, static_cast<std::uint64_t>(resource.usable_capacity) / 8));
    resource.failure_domains = {make_id<FailureDomainId>(
        indexed_name("domain", index % kFailureDomainCount, 1))};
    resource.provenance = make_provenance("link-state-fabric", spec.generation);
    snapshot.capacity.resources.push_back(std::move(resource));
  }

  const std::size_t path_count = spec.demands * spec.paths_per_demand;
  const std::size_t path_width = digit_width(path_count - 1);
  for (std::size_t demand_index = 0; demand_index < spec.demands; ++demand_index) {
    for (std::size_t slot = 0; slot < spec.paths_per_demand; ++slot) {
      const std::size_t path_index = demand_index * spec.paths_per_demand + slot;

      CandidatePath path;
      path.id = make_id<PathId>(indexed_name("path", path_index, path_width));
      path.generation = make_generation<PathGeneration>(spec.generation);
      path.authority.id = path.id;
      path.authority.generation = snapshot.path_authority_generation;
      path.candidate_set = snapshot.candidate_set;
      path.provenance = make_provenance("path-authority", spec.generation);

      const std::size_t member_count = 2 + (slot % 3);  // two to four resources per path
      std::vector<std::size_t> member_indices;
      for (std::size_t member = 0; member < member_count; ++member) {
        const std::size_t member_index =
            (demand_index * (spec.paths_per_demand + 3) + slot * 5 + member * 11) % spec.resources;
        if (std::find(member_indices.begin(), member_indices.end(), member_index) !=
            member_indices.end()) {
          continue;
        }
        member_indices.push_back(member_index);
        path.resources.push_back(
            make_id<ResourceId>(indexed_name("res", member_index, resource_width)));
      }
      if (member_indices.empty()) {
        // Unreachable (member_count >= 2 and resources >= 1); kept so that the
        // structural rule "a path declares at least one resource" always holds.
        member_indices.push_back(0);
        path.resources.push_back(make_id<ResourceId>(indexed_name("res", 0, resource_width)));
      }
      for (const std::size_t member_index : member_indices) {
        const FailureDomainId domain = make_id<FailureDomainId>(
            indexed_name("domain", member_index % kFailureDomainCount, 1));
        if (std::find(path.failure_domains.begin(), path.failure_domains.end(), domain) ==
            path.failure_domains.end()) {
          path.failure_domains.push_back(domain);
        }
      }

      path.cost = static_cast<std::int64_t>(1 + rng.range(0, 50));
      path.latency_micros = static_cast<std::int64_t>(100 + rng.range(0, 9000));
      snapshot.paths.push_back(std::move(path));
    }
  }

  const std::size_t demand_width = digit_width(spec.demands - 1);
  const std::size_t reservation_count = std::min<std::size_t>(spec.demands, 2);
  std::vector<ReservationId> reservation_ids;
  reservation_ids.reserve(reservation_count);
  for (std::size_t index = 0; index < reservation_count; ++index) {
    const CandidatePath& host = snapshot.paths[index * spec.paths_per_demand];
    Reservation reservation;
    reservation.id = make_id<ReservationId>(
        indexed_name("reservation", index, digit_width(reservation_count - 1)));
    reservation.generation = make_generation<ReservationGeneration>(spec.generation);
    reservation.provenance = make_provenance("bandwidth-reservation-fabric", spec.generation);
    reservation.paths = {host.id};
    reservation.resources = host.resources;
    reservation.bandwidth = 100;
    reservation.owner = make_id<DemandId>(indexed_name("demand", index, demand_width));
    reservation.preemptibility = Preemptibility::not_preemptible;
    reservation.effective.start_tick = 0;
    reservation.effective.end_tick = 0;
    reservation.effective.open_ended = true;
    reservation_ids.push_back(reservation.id);
    snapshot.reservations.reservations.push_back(std::move(reservation));
  }

  for (std::size_t index = 0; index < spec.demands; ++index) {
    Demand demand;
    demand.id = make_id<DemandId>(indexed_name("demand", index, demand_width));
    demand.generation = make_generation<DemandGeneration>(spec.generation);
    demand.provenance = make_provenance("network-admission-fabric", spec.generation);
    demand.tenant = make_id<TenantId>(indexed_name("tenant", index % 3, 1));
    demand.service_class = make_id<ServiceClassId>(indexed_name("class", index % 3, 1));
    const std::int64_t minimum = static_cast<std::int64_t>(200 + rng.range(0, 800));
    const std::int64_t desired =
        minimum + static_cast<std::int64_t>(rng.range(0, static_cast<std::uint64_t>(minimum)));
    const std::int64_t maximum = desired + static_cast<std::int64_t>(rng.range(0, 2000));
    demand.minimum_bandwidth = minimum;
    demand.desired_bandwidth = desired;
    demand.maximum_bandwidth = maximum;
    demand.priority = static_cast<std::uint8_t>(1 + rng.range(0, 254));
    demand.candidate_set = snapshot.candidate_set;
    demand.effective.start_tick = 0;
    demand.effective.end_tick = 0;
    demand.effective.open_ended = true;
    // Each demand is eligible for the paths generated for it, which is what
    // makes --paths-per-demand meaningful.
    for (std::size_t slot = 0; slot < spec.paths_per_demand; ++slot) {
      demand.allowed_paths.push_back(snapshot.paths[index * spec.paths_per_demand + slot].id);
    }
    if (index < reservation_count) demand.reservation_bindings.push_back(reservation_ids[index]);
    snapshot.demands.push_back(std::move(demand));
  }

  canonicalize(snapshot);
  const Status structural = validate_structure(snapshot);
  if (!structural.ok()) {
    return fail_as<FabricSnapshot>(structural.code(),
                                   "the generated population is structurally invalid: " +
                                       structural.error().detail());
  }
  return snapshot;
}

// ---------------------------------------------------------------------------
// Shared pipeline and formatting helpers
// ---------------------------------------------------------------------------

std::string format_engine_stats(const Engine::Stats& stats) {
  std::string out;
  out += "plans_created=" + u64_text(stats.plans_created) + "\n";
  out += "plans_committed=" + u64_text(stats.plans_committed) + "\n";
  out += "plans_rejected=" + u64_text(stats.plans_rejected) + "\n";
  out += "plans_stale=" + u64_text(stats.plans_stale) + "\n";
  out += "supersessions=" + u64_text(stats.supersessions) + "\n";
  out += "commits_revalidated=" + u64_text(stats.commits_revalidated) + "\n";
  return out;
}

// Runs the whole engine pipeline: submit, declare, validate, solve, authorize,
// commit. Returns the committed plan or the first typed error. When 'trace' is
// non-null it receives one deterministic line per stage.
Result<Plan> run_pipeline(Engine& engine, const FabricSnapshot& snapshot, const PlanId& plan_id,
                          std::uint64_t tick, std::string* trace) {
  const auto record = [trace](const std::string& line) {
    if (trace == nullptr) return;
    *trace += line;
    *trace += "\n";
  };

  const Status submitted = engine.submit_snapshot(snapshot);
  if (!submitted.ok()) {
    record("submit_snapshot: " + submitted.format());
    return Result<Plan>(submitted.error());
  }
  record("submit_snapshot: OK (" + size_text(snapshot.demands.size()) + " demands, " +
         size_text(snapshot.paths.size()) + " candidate paths, " +
         size_text(snapshot.capacity.resources.size()) + " resources, " +
         size_text(snapshot.reservations.reservations.size()) + " reservations)");

  DeclareRequest declare;
  declare.plan_id = plan_id;
  declare.attempt = make_id<AttemptId>("attempt-" + plan_id.str());
  declare.attempt_generation = AttemptGeneration::first();
  declare.publisher = make_id<PublisherId>("publisher-tef-cli");
  declare.publisher_boot = derive_boot_id(0x7EF1ULL, 0x0001ULL);
  declare.tick = tick;

  const Result<Plan> declared = engine.declare(declare);
  if (!declared.has_value()) {
    record("declare: " + declared.error().format());
    return declared;
  }
  record("declare: OK (plan " + declared.value().id.str() + " generation " +
         u64_text(declared.value().generation.value()) + ")");
  const PlanRef ref = declared.value().ref();

  const Status validated = engine.validate(ref);
  if (!validated.ok()) {
    record("validate: " + validated.format());
    return Result<Plan>(validated.error());
  }
  record("validate: OK");

  const Result<Plan> solved = engine.solve(ref);
  if (!solved.has_value()) {
    record("solve: " + solved.error().format());
    return solved;
  }
  record("solve: OK (state " + std::string(to_string(solved.value().state)) + ", " +
         std::string(to_string(solved.value().feasibility.status)) + ", score " +
         i64_text(solved.value().objective_score) + ")");

  AuthorizeRequest authorization;
  authorization.tick = tick;
  authorization.note = "tef-cli pipeline authorization";
  const Result<Plan> authorized = engine.authorize(ref, authorization);
  if (!authorized.has_value()) {
    record("authorize: " + authorized.error().format());
    return authorized;
  }
  record("authorize: OK");

  CommitIntent intent;
  intent.commit = make_id<CommitId>("commit-" + plan_id.str());
  intent.commit_generation = CommitGeneration::first();
  intent.tick = tick;
  const Result<Plan> committed = engine.commit(ref, intent);
  if (!committed.has_value()) {
    record("commit: " + committed.error().format());
    return committed;
  }
  record("commit: OK (" + committed.value().commit.str() + "@" +
         u64_text(committed.value().commit_generation.value()) + ")");
  return committed;
}

EngineConfig make_engine_config() {
  EngineConfig config;
  config.coordinator_id = make_id<CoordinatorId>("tef-coordinator-cli");
  config.coordinator_incarnation = CoordinatorIncarnation::first();
  config.max_records = Limits::max_audit_records;
  return config;
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

struct CliArguments {
  std::string command;
  std::vector<std::pair<std::string, std::string>> options;

  bool has(std::string_view key) const {
    for (const auto& option : options) {
      if (option.first == key) return true;
    }
    return false;
  }

  const std::string* value(std::string_view key) const {
    for (const auto& option : options) {
      if (option.first == key) return &option.second;
    }
    return nullptr;
  }
};

struct ParsedCommandLine {
  bool ok = false;
  std::string error;
  CliArguments arguments;
};

bool looks_like_option(const std::string& token) {
  return token.size() >= 2 && token[0] == '-' && token[1] == '-';
}

ParsedCommandLine parse_command_line(int argc, char** argv) {
  ParsedCommandLine parsed;
  if (argc < 2) {
    parsed.error = "no subcommand given";
    return parsed;
  }
  parsed.arguments.command = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string token = argv[index];
    if (!looks_like_option(token)) {
      parsed.error = "unexpected argument '" + token + "'";
      return parsed;
    }
    const std::size_t equals = token.find('=');
    if (equals != std::string::npos) {
      parsed.arguments.options.emplace_back(token.substr(2, equals - 2), token.substr(equals + 1));
      continue;
    }
    std::string value;
    if (index + 1 < argc) {
      const std::string next = argv[index + 1];
      if (!looks_like_option(next)) {
        value = next;
        ++index;
      }
    }
    parsed.arguments.options.emplace_back(token.substr(2), std::move(value));
  }
  parsed.ok = true;
  return parsed;
}

bool reject_unknown_options(const CliArguments& arguments,
                            const std::vector<std::string_view>& allowed, std::string& error) {
  for (const auto& option : arguments.options) {
    const std::string_view key(option.first);
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
      error = "unknown option --" + option.first + " for subcommand " + arguments.command;
      return false;
    }
  }
  return true;
}

struct ParseOutcome {
  bool ok = false;
  std::uint64_t value = 0;
};

ParseOutcome parse_unsigned(std::string_view text) {
  ParseOutcome outcome;
  if (text.empty()) return outcome;
  std::uint64_t value = 0;
  const char* const begin = text.data();
  const char* const end = text.data() + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, value, 10);
  if (result.ec != std::errc() || result.ptr != end) return outcome;
  outcome.ok = true;
  outcome.value = value;
  return outcome;
}

// Reads --key=N options, recording the first problem instead of throwing.
class OptionReader {
 public:
  explicit OptionReader(const CliArguments& arguments) : arguments_(arguments) {}

  std::uint64_t unsigned_value(std::string_view key, std::uint64_t fallback) {
    const std::string* const text = arguments_.value(key);
    if (text == nullptr) return fallback;
    const ParseOutcome parsed = parse_unsigned(*text);
    if (!parsed.ok) {
      if (error_.empty()) {
        error_ = "invalid value for --" + std::string(key) + ": '" + *text + "'";
      }
      return fallback;
    }
    return parsed.value;
  }

  bool ok() const { return error_.empty(); }
  const std::string& error() const { return error_; }

 private:
  const CliArguments& arguments_;
  std::string error_;
};

void print_usage(std::FILE* stream) {
  const char* const usage =
      "usage:\n"
      "  tef-cli demo\n"
      "  tef-cli plan --demands=N --paths-per-demand=N --resources=N --seed=N [--print-judgement]\n"
      "  tef-cli compare --incumbent-runs=N --seed=N\n"
      "  tef-cli inspect --state-dir=DIR\n"
      "  tef-cli selfcheck\n"
      "\n"
      "  Subcommands print deterministic output. Failures print a typed tef error and\n"
      "  exit non-zero; usage errors exit 2. The populations built by demo, plan,\n"
      "  compare and selfcheck are SYNTHETIC and describe no real network.\n";
  const std::size_t written = std::fwrite(usage, 1, std::char_traits<char>::length(usage), stream);
  (void)written;
}

// ---------------------------------------------------------------------------
// demo
// ---------------------------------------------------------------------------

int run_demo() {
  write_out(std::string(kProgramName) + " " + std::string(kVersionString) +
            " - Traffic Engineering Fabric demonstration\n");
  write_out("NOTICE: the fabric population used by this command is SYNTHETIC.\n");
  write_out("NOTICE: it describes no real network, no real traffic and no real tenant.\n");
  write_out("NOTICE: the pipeline runs entirely in-process and touches no durable state.\n\n");

  SyntheticFabricSpec spec;
  spec.demands = 6;
  spec.paths_per_demand = 3;
  spec.resources = 6;
  spec.seed = 20260214;

  const Result<FabricSnapshot> built = build_synthetic_fabric(spec);
  if (!built.has_value()) return report_failure("demo: build population", built.error());
  const FabricSnapshot& snapshot = built.value();

  write_out("-- synthetic fabric --\n" + render_snapshot_summary(snapshot) + "\n");

  Engine engine(make_engine_config());
  const PlanId plan_id = make_id<PlanId>("plan-demo");
  std::string trace;
  const Result<Plan> committed = run_pipeline(engine, snapshot, plan_id, 1, &trace);
  write_out("-- pipeline --\n" + trace);
  if (!committed.has_value()) return report_failure("demo: pipeline", committed.error());

  const Plan& plan = committed.value();
  write_out("\n-- plan summary --\n" + render_plan_summary(plan));

  const std::optional<Explanation> explanation = engine.explain(plan.ref());
  if (!explanation.has_value()) {
    return report_failure("demo: explain",
                          Error(ErrorCode::not_found, "the engine holds no explanation for " +
                                                         plan.id.str()));
  }
  write_out("\n-- explanation --\n" + explanation->to_text());
  write_out("\n-- authority bound by the committed plan --\n" + render_authority(plan.authority));
  write_out("\n-- live authority --\n" + render_authority(engine.live_authority()));
  write_out("\n-- engine stats --\n" + format_engine_stats(engine.stats()));
  return kExitOk;
}

// ---------------------------------------------------------------------------
// plan
// ---------------------------------------------------------------------------

std::string format_solve_line(const SyntheticFabricSpec& spec, const SolveOutcome& outcome) {
  std::string line = "plan";
  line += " demands=" + size_text(spec.demands);
  line += " paths_per_demand=" + size_text(spec.paths_per_demand);
  line += " resources=" + size_text(spec.resources);
  line += " seed=" + u64_text(spec.seed);
  line += " status=" + std::string(to_string(outcome.status));
  line += " score=" + i64_text(outcome.score);
  line += " allocation_digest=" + outcome.allocation_digest.hex();
  line += " iterations=" + u64_text(outcome.iterations);
  line += " total_granted=" + i64_text(outcome.allocation.total_granted);
  return line;
}

void print_judgement(const FabricSnapshot& snapshot, const SolveOutcome& outcome) {
  write_out("\n-- fabric --\n" + render_snapshot_summary(snapshot));
  write_out("\n-- judgement --\n" + render_feasibility(outcome.feasibility));
  write_out("verified=" + bool_text(outcome.verified) + "\n");
  // render_feasibility already prints the summary when it matches the
  // feasibility result; only a divergent outcome summary is reported here.
  if (!outcome.summary.empty() && outcome.summary != outcome.feasibility.summary) {
    write_out("outcome_summary=" + outcome.summary + "\n");
  }
  write_out("\n-- allocation --\n" + render_allocation(outcome.allocation));
  write_out("\n-- objective --\n");
  write_out("score=" + i64_text(outcome.score) + "\n");
  for (const auto& component : outcome.components) {
    write_out("component " + std::string(to_string(component.term)) +
              " weight=" + i64_text(component.weight) + " raw=" + i64_text(component.raw) +
              " weighted=" + i64_text(component.weighted) + " unit=" + component.unit + "\n");
  }
  for (const auto& note : outcome.notes) write_out("note " + note + "\n");
  for (const auto& exclusion : outcome.policy_exclusions) {
    write_out("policy_exclusion " + exclusion + "\n");
  }
  for (const auto& alternative : outcome.alternatives) {
    write_out("alternative subject=" + alternative.subject + " reason=" + alternative.reason +
              " delta=" + i64_text(alternative.delta) +
              " alternative_score=" + i64_text(alternative.alternative_score) + "\n");
  }
}

int run_plan(const CliArguments& arguments) {
  std::string error;
  const std::vector<std::string_view> allowed = {"demands", "paths-per-demand", "resources", "seed",
                                                 "print-judgement"};
  if (!reject_unknown_options(arguments, allowed, error)) return report_usage_error(error);
  const bool print_full_judgement = arguments.has("print-judgement");

  OptionReader reader(arguments);
  SyntheticFabricSpec spec;
  spec.demands = static_cast<std::size_t>(reader.unsigned_value("demands", 16));
  spec.paths_per_demand = static_cast<std::size_t>(reader.unsigned_value("paths-per-demand", 3));
  spec.resources = static_cast<std::size_t>(reader.unsigned_value("resources", 12));
  spec.seed = reader.unsigned_value("seed", 1);
  if (!reader.ok()) return report_usage_error(reader.error());

  spec = clamp_fabric_spec(spec);
  const Result<FabricSnapshot> built = build_synthetic_fabric(spec);
  if (!built.has_value()) return report_failure("plan: build population", built.error());

  SolveOptions options;
  const Result<SolveOutcome> outcome = solve(built.value(), options);
  if (!outcome.has_value()) return report_failure("plan: solve", outcome.error());

  write_out(format_solve_line(spec, outcome.value()) + "\n");
  if (print_full_judgement) print_judgement(built.value(), outcome.value());
  return kExitOk;
}

// ---------------------------------------------------------------------------
// compare
// ---------------------------------------------------------------------------

int run_compare(const CliArguments& arguments) {
  std::string error;
  const std::vector<std::string_view> allowed = {"incumbent-runs", "seed"};
  if (!reject_unknown_options(arguments, allowed, error)) return report_usage_error(error);

  OptionReader reader(arguments);
  const std::uint64_t seed = reader.unsigned_value("seed", 1);
  const std::uint64_t incumbent_runs = reader.unsigned_value("incumbent-runs", 2);
  if (!reader.ok()) return report_usage_error(reader.error());
  if (incumbent_runs == 0 || incumbent_runs > 64) {
    return report_usage_error("--incumbent-runs must be within [1, 64]");
  }

  SyntheticFabricSpec spec;
  spec.demands = 12;
  spec.paths_per_demand = 3;
  spec.resources = 8;
  spec.seed = seed;
  spec = clamp_fabric_spec(spec);

  const Result<FabricSnapshot> built = build_synthetic_fabric(spec);
  if (!built.has_value()) return report_failure("compare: build population", built.error());
  const FabricSnapshot& fabric = built.value();

  write_out(std::string(kProgramName) +
            " - Traffic Engineering Fabric churn comparison\n");
  write_out("NOTICE: the fabric population used by this command is SYNTHETIC.\n\n");

  // The incumbent: solve the fabric once, then re-solve it --incumbent-runs - 1
  // more times with the previous allocation handed back as the incumbent, so the
  // incumbent is the fixed point of the churn-aware loop. The chain is
  // deterministic: the same seed always converges the same way.
  SolveOptions options;
  Result<SolveOutcome> incumbent = solve(fabric, options);
  if (!incumbent.has_value()) return report_failure("compare: incumbent solve", incumbent.error());
  for (std::uint64_t run = 1; run < incumbent_runs; ++run) {
    SolveOptions warm = options;
    warm.has_incumbent = true;
    warm.incumbent = incumbent.value().allocation;
    const Result<SolveOutcome> next = solve(fabric, warm);
    if (!next.has_value()) return report_failure("compare: incumbent solve", next.error());
    incumbent = next;
  }
  if (!carries_allocation(incumbent.value().status)) {
    return report_failure("compare: incumbent solve",
                          Error(ErrorCode::solver_limit,
                                "the incumbent configuration produced " +
                                    std::string(to_string(incumbent.value().status)) +
                                    ", which carries no allocation"));
  }
  const Allocation incumbent_allocation = incumbent.value().allocation;
  // The incumbent is re-scored on the *live* objective, exactly as the engine
  // scores a pending incumbent before calling compare_churn.
  const std::int64_t incumbent_score = score_allocation(fabric, incumbent_allocation, nullptr);

  // The proposal: the current objective plus churn control, solved against the
  // incumbent allocation.
  FabricSnapshot proposal_fabric = fabric;
  proposal_fabric.objective.terms.push_back({ObjectiveTerm::minimize_churn, 4});
  proposal_fabric.objective.generation = make_generation<ObjectiveProfileGeneration>(2);

  SolveOptions proposal_options;
  proposal_options.has_incumbent = true;
  proposal_options.incumbent = incumbent_allocation;
  const Result<SolveOutcome> proposal = solve(proposal_fabric, proposal_options);
  if (!proposal.has_value()) return report_failure("compare: proposal solve", proposal.error());

  const Allocation& proposal_allocation = proposal.value().allocation;
  const ChurnReport churn =
      compare_churn(incumbent_allocation, proposal_allocation, fabric, fabric.policy,
                    incumbent_score, proposal.value().score);

  write_out("fabric demands=" + size_text(spec.demands) +
            " paths_per_demand=" + size_text(spec.paths_per_demand) +
            " resources=" + size_text(spec.resources) + " seed=" + u64_text(spec.seed) + "\n");
  write_out("incumbent runs=" + u64_text(incumbent_runs) +
            " status=" + std::string(to_string(incumbent.value().status)) +
            " score=" + i64_text(incumbent_score) +
            " total_granted=" + i64_text(incumbent_allocation.total_granted) +
            " allocation_digest=" + incumbent_allocation.digest().hex() + "\n");
  write_out(std::string("proposal objective=current+minimize_churn") +
            " status=" + std::string(to_string(proposal.value().status)) +
            " score=" + i64_text(proposal.value().score) +
            " total_granted=" + i64_text(proposal_allocation.total_granted) +
            " allocation_digest=" + proposal_allocation.digest().hex() + "\n");
  const bool identical = incumbent_allocation.digest() == proposal_allocation.digest();
  write_out("identical_allocations=" + bool_text(identical) + "\n");
  write_out("notice: both solves use the same population; the proposal adds churn control "
            "(minimize_churn) to the active objective and is solved against the incumbent "
            "allocation.\n");
  if (identical) {
    write_out("notice: the deterministic solver reached the same allocation from the same "
              "population, so the proposal moves no bandwidth and the incumbent is confirmed "
              "rather than replaced.\n");
  }
  write_out("\n-- churn --\n" + render_churn(churn));
  write_out("incumbent_score=" + i64_text(churn.incumbent_score) + "\n");
  write_out("proposal_score=" + i64_text(churn.proposal_score) + "\n");
  return kExitOk;
}

// ---------------------------------------------------------------------------
// inspect
// ---------------------------------------------------------------------------

struct DurablePlanEntry {
  std::uint64_t sequence = 0;
  Plan plan;
};

std::string file_size_text(const std::filesystem::path& path, bool present) {
  if (!present) return "absent";
  std::error_code code;
  const std::uintmax_t size = std::filesystem::file_size(path, code);
  if (code) return "unavailable";
  return std::to_string(size);
}

int run_inspect(const CliArguments& arguments) {
  std::string error;
  const std::vector<std::string_view> allowed = {"state-dir"};
  if (!reject_unknown_options(arguments, allowed, error)) return report_usage_error(error);

  const std::string* const state_dir = arguments.value("state-dir");
  if (state_dir == nullptr || state_dir->empty()) {
    return report_usage_error("inspect requires --state-dir=DIR");
  }

  const std::filesystem::path root(*state_dir);
  std::error_code code;
  const bool exists = std::filesystem::exists(root, code);
  if (code) {
    return report_failure("inspect: stat " + root.string(),
                          Error(ErrorCode::io_failure, code.message()));
  }
  if (!exists) {
    return report_failure("inspect: open",
                          Error(ErrorCode::not_found, "no such directory: " + root.string()));
  }
  const bool is_directory = std::filesystem::is_directory(root, code);
  if (code || !is_directory) {
    return report_failure("inspect: open",
                          Error(ErrorCode::invalid_argument,
                                "not a directory: " + root.string()));
  }

  write_out(std::string(kProgramName) + " - Traffic Engineering Fabric durable state report\n");
  write_out("state_dir=" + root.string() + "\n");
  write_out("notice: nothing is appended, compacted, fenced or cleared by this command.\n");
  write_out("notice: PlanRepository::open performs crash recovery only; it may complete a missing\n");
  write_out("        journal file and truncate a torn journal tail at the last valid record.\n\n");

  const std::filesystem::path manifest_path = root / kManifestFileName;
  const std::filesystem::path snapshot_path = root / kSnapshotFileName;
  const std::filesystem::path journal_path = root / kJournalFileName;

  const bool manifest_present = std::filesystem::exists(manifest_path, code);
  const bool snapshot_present = std::filesystem::exists(snapshot_path, code);
  const bool journal_present = std::filesystem::exists(journal_path, code);

  write_out("manifest_present=" + bool_text(manifest_present) +
            " bytes=" + file_size_text(manifest_path, manifest_present) + "\n");
  write_out("snapshot_present=" + bool_text(snapshot_present) +
            " bytes=" + file_size_text(snapshot_path, snapshot_present) + "\n");
  write_out("journal_present=" + bool_text(journal_present) +
            " bytes=" + file_size_text(journal_path, journal_present) + "\n");

  if (!manifest_present && !snapshot_present && !journal_present) {
    // Opening an uninitialised directory would create durable files, and inspect
    // must not modify anything.
    write_out("durable_state=none\n");
    write_out("repository=not_opened\n");
    write_out("notice: the directory holds no durable state; it was left exactly as it was "
              "found.\n");
    return kExitOk;
  }

  PlanRepository::Options options;
  options.directory = root.string();
  options.max_records = Limits::max_journal_records;
  options.fsync = false;

  Result<std::unique_ptr<PlanRepository>> opened = PlanRepository::open(options);
  if (!opened.has_value()) return report_failure("inspect: open", opened.error());
  const std::unique_ptr<PlanRepository>& repository = opened.value();

  const std::vector<DurableRecord> records = repository->records();
  const PlanRepository::RecoveryReport recovery = repository->recovery();
  const PlanRepository::Counters counters = repository->counters();
  const std::vector<FenceRecord> fences = repository->fences();

  write_out("repository=opened\n");
  write_out("record_count=" + size_text(records.size()) + "\n");
  write_out("last_sequence=" + u64_text(repository->last_sequence()) + "\n");
  write_out("coordinator_epoch=" + u64_text(repository->coordinator_epoch()) + "\n");

  write_out("recovery manifest_present=" + bool_text(recovery.manifest_present) +
            " snapshot_loaded=" + bool_text(recovery.snapshot_loaded) +
            " snapshot_sequence=" + u64_text(recovery.snapshot_sequence) + "\n");
  write_out("recovery records_recovered=" + u64_text(recovery.records_recovered) +
            " records_discarded=" + u64_text(recovery.records_discarded) +
            " journal_bytes_truncated=" + u64_text(recovery.journal_bytes_truncated) + "\n");
  write_out("recovery coordinator_epoch=" + u64_text(recovery.coordinator_epoch) + "\n");
  std::size_t diagnostic_index = 0;
  for (const std::string& diagnostic : recovery.diagnostics) {
    write_out("recovery diagnostic=" + size_text(diagnostic_index) + " " + diagnostic + "\n");
    ++diagnostic_index;
  }

  write_out("counters appends=" + u64_text(counters.appends) +
            " compactions=" + u64_text(counters.compactions) +
            " recoveries=" + u64_text(counters.recoveries) +
            " truncated_bytes=" + u64_text(counters.truncated_bytes) + "\n");

  const RecordType type_order[] = {RecordType::plan,          RecordType::policy,
                                   RecordType::objective_profile, RecordType::fence,
                                   RecordType::coordinator_epoch, RecordType::audit};
  for (const RecordType type : type_order) {
    std::size_t count = 0;
    for (const auto& record : records) {
      if (record.type == type) ++count;
    }
    write_out("record_type " + std::string(to_string(type)) + " count=" + size_text(count) + "\n");
  }

  // A plan appears once per transition; the durable fact is the latest record
  // per plan identity, exactly as recovery collapses it.
  std::map<PlanId, DurablePlanEntry> latest_plans;
  std::size_t plan_records = 0;
  std::size_t plan_decode_failures = 0;
  for (const auto& record : records) {
    if (record.type != RecordType::plan) continue;
    ++plan_records;
    const Result<Plan> decoded = decode_plan(record.payload);
    if (!decoded.has_value()) {
      ++plan_decode_failures;
      continue;
    }
    DurablePlanEntry entry;
    entry.sequence = record.sequence;
    entry.plan = decoded.value();
    latest_plans[entry.plan.id] = std::move(entry);
  }
  write_out("plan_records=" + size_text(plan_records) +
            " plan_identities=" + size_text(latest_plans.size()) +
            " plan_decode_failures=" + size_text(plan_decode_failures) + "\n");

  std::size_t listed_plans = 0;
  for (const auto& entry : latest_plans) {
    if (listed_plans >= kInspectListingLimit) {
      write_out("plans_truncated=true\n");
      break;
    }
    ++listed_plans;
    const Plan& plan = entry.second.plan;
    std::string line = "plan " + plan.id.str() + " sequence=" + u64_text(entry.second.sequence) +
                       " generation=" + u64_text(plan.generation.value()) +
                       " state=" + std::string(to_string(plan.state)) +
                       " applicability=" + std::string(to_string(plan.applicability)) +
                       " score=" + i64_text(plan.objective_score) +
                       " allocation_digest=" +
                       (plan.allocation.empty() ? std::string("none")
                                                : plan.allocation.digest().hex());
    if (!plan.attempt.str().empty()) line += " attempt=" + plan.attempt.str();
    if (plan.publisher.valid()) line += " publisher=" + plan.publisher.str();
    if (plan.commit.valid()) {
      line += " commit=" + plan.commit.str() + "@" + u64_text(plan.commit_generation.value());
    }
    if (plan.supersedes.id.valid()) line += " supersedes=" + plan.supersedes.id.str();
    if (plan.superseded_by.id.valid()) line += " superseded_by=" + plan.superseded_by.id.str();
    write_out(line + "\n");
  }

  write_out("fences=" + size_text(fences.size()) + "\n");
  std::size_t listed_fences = 0;
  for (const auto& fence : fences) {
    if (listed_fences >= kInspectListingLimit) {
      write_out("fences_truncated=true\n");
      break;
    }
    ++listed_fences;
    write_out("fence publisher=" + fence.publisher.str() + " boot=" + fence.boot.hex() +
              " incarnation=" + u64_text(fence.incarnation.value()) +
              " tick=" + u64_text(fence.tick) + " reason=" + fence.reason + "\n");
  }
  return kExitOk;
}

// ---------------------------------------------------------------------------
// selfcheck
// ---------------------------------------------------------------------------

struct CheckLog {
  std::size_t passed = 0;
  std::size_t failed = 0;

  void expect(bool condition, std::string_view name, const std::string& detail) {
    if (condition) {
      ++passed;
      write_out("PASS " + std::string(name) + "\n");
      return;
    }
    ++failed;
    write_out("FAIL " + std::string(name) + ": " + detail + "\n");
  }
};

// Owns a temporary directory and removes it, whatever happens afterwards.
class TemporaryDirectory {
 public:
  TemporaryDirectory() = default;
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  ~TemporaryDirectory() { remove(); }

  bool create(std::string& error) {
    std::error_code code;
    const std::filesystem::path base = std::filesystem::temp_directory_path(code);
    if (code) {
      error = "cannot resolve the temporary directory: " + code.message();
      return false;
    }
    for (std::size_t index = 0; index < 1000; ++index) {
      const std::filesystem::path candidate =
          base / ("tef-cli-selfcheck-" + std::to_string(index));
      std::error_code create_code;
      if (std::filesystem::create_directory(candidate, create_code)) {
        root_ = candidate;
        return true;
      }
    }
    error = "cannot create a temporary directory below " + base.string();
    return false;
  }

  void remove() {
    if (root_.empty()) return;
    std::error_code code;
    std::filesystem::remove_all(root_, code);
  }

  const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path root_;
};

PlanRepository::Options repository_options(const std::string& directory) {
  PlanRepository::Options options;
  options.directory = directory;
  options.max_records = Limits::max_journal_records;
  options.fsync = false;  // speed only: the checks never depend on durability
  options.compact_after_records = 4096;
  return options;
}

int run_selfcheck() {
  CheckLog log;
  write_out(std::string(kProgramName) + " - Traffic Engineering Fabric self check\n");
  write_out("NOTICE: the fabric population used by this check is SYNTHETIC.\n\n");

  SyntheticFabricSpec requested;
  requested.demands = 8;
  requested.paths_per_demand = 3;
  requested.resources = 6;
  requested.seed = 2026;

  const Result<FabricSnapshot> built = build_synthetic_fabric(requested);
  if (!built.has_value()) {
    log.expect(false, "fabric.build", built.error().format());
    write_out("\nselfcheck passed=" + size_text(log.passed) + " failed=" + size_text(log.failed) +
              "\nselfcheck RESULT=FAIL\n");
    return kExitFailure;
  }
  const FabricSnapshot snapshot = built.value();

  // ---- Structural validation --------------------------------------------
  const Status structural = validate_structure(snapshot);
  log.expect(structural.ok(), "fabric.structural_validation",
             structural.ok() ? std::string() : structural.format());

  FabricSnapshot recanonicalized = snapshot;
  const CanonicalizeReport canonical_report = canonicalize(recanonicalized);
  log.expect(encode_snapshot(recanonicalized) == encode_snapshot(snapshot),
             "fabric.canonicalization_idempotent",
             "re-canonicalizing the population changed its canonical encoding");
  log.expect(canonical_report.duplicate_entries_removed == 0, "fabric.no_duplicate_entries",
             "duplicate entries removed: " +
                 size_text(canonical_report.duplicate_entries_removed));

  const std::vector<std::byte> encoded = encode_snapshot(snapshot);
  const Result<FabricSnapshot> decoded = decode_snapshot(encoded);
  const bool round_trip_ok = decoded.has_value() && encode_snapshot(decoded.value()) == encoded;
  log.expect(round_trip_ok, "fabric.snapshot_round_trip",
             decoded.has_value() ? std::string("re-encoding the decoded snapshot differs")
                                 : decoded.error().format());

  const AuthorityVector authority = authority_of(snapshot);
  log.expect(authority.valid() && !authority.digest().is_zero() &&
                 compare_authority(authority, authority).identical(),
             "fabric.authority_binding", "the authority vector is invalid or not identical to itself");

  // ---- Limits clamping ---------------------------------------------------
  SyntheticFabricSpec oversized;
  oversized.demands = Limits::max_demands * 4;
  oversized.paths_per_demand = Limits::max_paths_per_demand * 4;
  oversized.resources = Limits::max_resources * 4;
  const SyntheticFabricSpec clamped = clamp_fabric_spec(oversized);
  const bool clamped_ok = clamped.demands >= 1 && clamped.demands <= Limits::max_demands &&
                          clamped.paths_per_demand >= 1 &&
                          clamped.paths_per_demand <= Limits::max_paths_per_demand &&
                          clamped.resources >= 1 && clamped.resources <= Limits::max_resources &&
                          clamped.demands * clamped.paths_per_demand <=
                              Limits::max_total_candidate_paths;
  log.expect(clamped_ok, "fabric.limits_clamping",
             "demands=" + size_text(clamped.demands) +
                 " paths_per_demand=" + size_text(clamped.paths_per_demand) +
                 " resources=" + size_text(clamped.resources));

  // ---- Deterministic solve ----------------------------------------------
  SolveOptions options;
  const Result<SolveOutcome> first = solve(snapshot, options);
  const Result<SolveOutcome> second = solve(snapshot, options);
  if (!first.has_value() || !second.has_value()) {
    const std::string detail =
        first.has_value() ? second.error().format() : first.error().format();
    log.expect(false, "solve.deterministic", detail);
    log.expect(false, "solve.status_feasible", detail);
    log.expect(false, "solve.independently_verified", detail);
    log.expect(false, "solve.verifier_accepts_allocation", detail);
  } else {
    const SolveOutcome& a = first.value();
    const SolveOutcome& b = second.value();
    const bool deterministic = a.status == b.status && a.score == b.score &&
                               a.iterations == b.iterations &&
                               a.allocation_digest == b.allocation_digest &&
                               a.allocation.total_granted == b.allocation.total_granted;
    log.expect(deterministic, "solve.deterministic",
               "two solves of one population differ: status " +
                   std::string(to_string(a.status)) + "/" + std::string(to_string(b.status)) +
                   " score " + i64_text(a.score) + "/" + i64_text(b.score) + " digest " +
                   a.allocation_digest.hex() + "/" + b.allocation_digest.hex());
    log.expect(a.status == FeasibilityStatus::feasible, "solve.status_feasible",
               "expected FEASIBLE, got " + std::string(to_string(a.status)) + " (" + a.summary +
                   ")");
    log.expect(a.verified, "solve.independently_verified",
               "SolveOutcome::verified is false");
    const std::vector<BindingConstraint> violations =
        verify_allocation(snapshot, first.value().allocation);
    std::string violation_text = size_text(violations.size()) + " binding constraint(s) violated";
    if (!violations.empty()) {
      violation_text += ": " + std::string(to_string(violations.front().kind)) + " " +
                        violations.front().subject;
    }
    log.expect(violations.empty(), "solve.verifier_accepts_allocation", violation_text);
  }

  // ---- Plan lifecycle ----------------------------------------------------
  Engine engine(make_engine_config());
  const PlanId plan_id = make_id<PlanId>("plan-selfcheck-memory");
  std::string trace;
  const Result<Plan> committed = run_pipeline(engine, snapshot, plan_id, 1, &trace);
  const bool committed_ok = committed.has_value();
  log.expect(committed_ok, "engine.pipeline_commits",
             committed_ok ? std::string() : committed.error().format());

  if (committed_ok) {
    const Plan& plan = committed.value();
    log.expect(plan.state == PlanState::committed, "engine.committed_state",
               "state=" + std::string(to_string(plan.state)));
    const std::optional<Plan> incumbent = engine.incumbent();
    log.expect(incumbent.has_value() && incumbent->id == plan_id, "engine.incumbent_recorded",
               "the engine has no incumbent for " + plan_id.str());

    const Engine::Stats stats = engine.stats();
    const bool stats_ok = stats.plans_created == 1 && stats.plans_committed == 1 &&
                          stats.plans_rejected == 0 && stats.plans_stale == 0 &&
                          stats.supersessions == 0 && stats.commits_revalidated == 1;
    log.expect(stats_ok, "engine.stats_after_commit", format_engine_stats(stats));

    const std::optional<Explanation> explanation = engine.explain(plan.ref());
    log.expect(explanation.has_value() && !explanation->to_text().empty(),
               "engine.explanation_available",
               explanation.has_value() ? "the explanation renders empty"
                                       : "the engine holds no explanation");

    CommitIntent replay_intent;
    replay_intent.commit = make_id<CommitId>("commit-" + plan_id.str());
    replay_intent.commit_generation = CommitGeneration::first();
    const Result<Plan> replay = engine.commit(plan.ref(), replay_intent);
    log.expect(replay.has_value() && replay.value().state == PlanState::committed,
               "engine.commit_replay_idempotent",
               replay.has_value() ? "state=" + std::string(to_string(replay.value().state))
                                  : replay.error().format());

    CommitIntent competing_intent;
    competing_intent.commit = make_id<CommitId>("commit-selfcheck-competing");
    competing_intent.commit_generation = make_generation<CommitGeneration>(2);
    const Result<Plan> competing = engine.commit(plan.ref(), competing_intent);
    log.expect(!competing.has_value() &&
                   competing.error().code() == ErrorCode::already_committed,
               "engine.conflicting_commit_refused",
               competing.has_value() ? "a conflicting commit was accepted"
                                     : std::string(to_string(competing.error().code())));

    const Status revalidate = engine.validate(plan.ref());
    log.expect(!revalidate.ok() && revalidate.code() == ErrorCode::invalid_transition,
               "engine.illegal_transition_refused",
               revalidate.ok() ? "validating a committed plan succeeded"
                               : std::string(to_string(revalidate.code())));

    const Result<Plan> retired = engine.retire(plan.ref());
    log.expect(retired.has_value() && retired.value().state == PlanState::retired,
               "engine.retire_after_commit",
               retired.has_value() ? "state=" + std::string(to_string(retired.value().state))
                                   : retired.error().format());
  }

  // ---- Churn -------------------------------------------------------------
  if (first.has_value()) {
    const Allocation& allocation = first.value().allocation;
    const std::int64_t score = first.value().score;
    const ChurnReport churn =
        compare_churn(allocation, allocation, snapshot, snapshot.policy, score, score);
    log.expect(churn.decision == ChurnDecision::accept && churn.moved_bandwidth == 0 &&
                   churn.demands_changed == 0,
               "churn.identical_allocation_no_movement",
               "decision=" + std::string(to_string(churn.decision)) +
                   " moved_bandwidth=" + i64_text(churn.moved_bandwidth));
  }

  // ---- Persistence: write, reopen, recover -------------------------------
  TemporaryDirectory temporary;
  std::string temporary_error;
  if (!temporary.create(temporary_error)) {
    log.expect(false, "persistence.temporary_directory", temporary_error);
  } else {
    const std::string state_dir = (temporary.root() / "state-a").string();
    const PlanId durable_plan_id = make_id<PlanId>("plan-selfcheck-durable");
    bool first_phase_ok = true;

    {
      Result<std::unique_ptr<PlanRepository>> opened =
          PlanRepository::open(repository_options(state_dir));
      if (!opened.has_value()) {
        first_phase_ok = false;
        log.expect(false, "persistence.open", opened.error().format());
      } else {
        std::shared_ptr<PlanRepository> repository(std::move(opened.value()));
        Engine durable_engine(make_engine_config());
        const Status attached = durable_engine.attach_repository(repository);
        log.expect(attached.ok(), "persistence.attach_repository",
                   attached.ok() ? std::string() : attached.format());

        std::string durable_trace;
        const Result<Plan> durable =
            run_pipeline(durable_engine, snapshot, durable_plan_id, 2, &durable_trace);
        log.expect(durable.has_value() && durable.value().state == PlanState::committed,
                   "persistence.pipeline_commits",
                   durable.has_value()
                       ? "state=" + std::string(to_string(durable.value().state))
                       : durable.error().format());
        const std::size_t records_after_commit = repository->record_count();
        log.expect(records_after_commit >= 1, "persistence.records_written",
                   "record_count=" + size_text(records_after_commit));
        durable_engine.detach_repository();
        const Status closed = repository->close();
        log.expect(closed.ok(), "persistence.close",
                   closed.ok() ? std::string() : closed.format());
      }
    }

    if (first_phase_ok) {
      Result<std::unique_ptr<PlanRepository>> reopened =
          PlanRepository::open(repository_options(state_dir));
      if (!reopened.has_value()) {
        log.expect(false, "persistence.reopen", reopened.error().format());
      } else {
        std::shared_ptr<PlanRepository> repository(std::move(reopened.value()));
        const PlanRepository::RecoveryReport recovery = repository->recovery();
        log.expect(recovery.manifest_present, "persistence.reopen_manifest",
                   "the manifest was not found on reopen");
        log.expect(repository->counters().recoveries >= 1, "persistence.recovery_counter",
                   "recoveries=" + u64_text(repository->counters().recoveries));
        log.expect(repository->record_count() >= 1 && repository->last_sequence() >= 1,
                   "persistence.records_recovered",
                   "record_count=" + size_text(repository->record_count()) +
                       " last_sequence=" + u64_text(repository->last_sequence()));

        Engine recovered_engine(make_engine_config());
        const Status attached = recovered_engine.attach_repository(repository);
        log.expect(attached.ok(), "persistence.attach_recovered_repository",
                   attached.ok() ? std::string() : attached.format());
        const Status recovered = recovered_engine.recover_from_repository();
        log.expect(recovered.ok(), "persistence.recover_from_repository",
                   recovered.ok() ? std::string() : recovered.format());

        const RecoverReport& report = recovered_engine.recovery_report();
        log.expect(report.records_read >= 1 && report.plans_restored == 1 &&
                       report.plans_marked_revalidation_required == 1,
                   "persistence.recovery_report",
                   "records_read=" + u64_text(report.records_read) +
                       " plans_restored=" + u64_text(report.plans_restored) +
                       " revalidation_required=" +
                       u64_text(report.plans_marked_revalidation_required));

        const std::optional<Plan> restored = recovered_engine.get(durable_plan_id);
        log.expect(restored.has_value() && restored->state == PlanState::committed &&
                       restored->applicability == PlanApplicability::revalidation_required,
                   "persistence.durable_plan_restored",
                   restored.has_value()
                       ? "state=" + std::string(to_string(restored->state)) + " applicability=" +
                             std::string(to_string(restored->applicability))
                       : "the durable plan was not restored");
        log.expect(recovered_engine.incumbent().has_value(),
                   "persistence.incumbent_restored_after_recovery",
                   "the recovered engine has no incumbent");
        log.expect(!recovered_engine.snapshot_copy().has_value() &&
                       !recovered_engine.live_authority().valid(),
                   "persistence.live_authority_not_restored",
                   "durable history restored live authority");
        recovered_engine.detach_repository();
        const Status closed = repository->close();
        log.expect(closed.ok(), "persistence.close_reopened",
                   closed.ok() ? std::string() : closed.format());
      }
    }

    // ---- Persistence: fences and coordinator epoch -------------------------
    const std::string fence_dir = (temporary.root() / "state-b").string();
    const PublisherId fenced_publisher = make_id<PublisherId>("publisher-fenced");
    const BootId fenced_boot = derive_boot_id(3, 4);
    {
      Result<std::unique_ptr<PlanRepository>> opened =
          PlanRepository::open(repository_options(fence_dir));
      if (!opened.has_value()) {
        log.expect(false, "persistence.open_fence_store", opened.error().format());
      } else {
        std::shared_ptr<PlanRepository> repository(std::move(opened.value()));
        FenceRecord fence;
        fence.publisher = fenced_publisher;
        fence.boot = fenced_boot;
        fence.incarnation = CoordinatorIncarnation::first();
        fence.reason = "selfcheck fence";
        fence.tick = 42;
        const Status fenced = repository->add_fence(fence);
        log.expect(fenced.ok() && repository->fences().size() == 1, "persistence.add_fence",
                   fenced.ok() ? "fence count=" + size_text(repository->fences().size())
                               : fenced.format());

        CoordinatorEpochRecord epoch_record;
        epoch_record.coordinator = make_id<CoordinatorId>("tef-coordinator-cli");
        epoch_record.incarnation = CoordinatorIncarnation::first();
        epoch_record.boot = derive_boot_id(5, 6);
        epoch_record.epoch = 9;
        const Status epoch = repository->set_coordinator_epoch(9, epoch_record);
        log.expect(epoch.ok() && repository->coordinator_epoch() == 9,
                   "persistence.coordinator_epoch_set",
                   epoch.ok() ? "coordinator_epoch=" + u64_text(repository->coordinator_epoch())
                              : epoch.format());
        const Status closed = repository->close();
        log.expect(closed.ok(), "persistence.close_fence_store",
                   closed.ok() ? std::string() : closed.format());
      }
    }
    {
      Result<std::unique_ptr<PlanRepository>> reopened =
          PlanRepository::open(repository_options(fence_dir));
      if (!reopened.has_value()) {
        log.expect(false, "persistence.reopen_fence_store", reopened.error().format());
      } else {
        std::shared_ptr<PlanRepository> repository(std::move(reopened.value()));
        const std::vector<FenceRecord> fences = repository->fences();
        const bool fence_ok = fences.size() == 1 && fences.front().publisher == fenced_publisher &&
                              fences.front().boot == fenced_boot;
        log.expect(fence_ok, "persistence.fence_survives_reopen",
                   "fence count=" + size_text(fences.size()));
        log.expect(repository->coordinator_epoch() == 9,
                   "persistence.coordinator_epoch_survives_reopen",
                   "coordinator_epoch=" + u64_text(repository->coordinator_epoch()));
        const Status closed = repository->close();
        log.expect(closed.ok(), "persistence.close_fence_store_reopened",
                   closed.ok() ? std::string() : closed.format());
      }
    }

    temporary.remove();
    std::error_code exists_code;
    const bool still_present = std::filesystem::exists(temporary.root(), exists_code);
    log.expect(!still_present, "persistence.temporary_directory_removed",
               "the temporary directory still exists: " + temporary.root().string());
  }

  write_out("\nselfcheck passed=" + size_text(log.passed) + " failed=" + size_text(log.failed) + "\n");
  write_out(log.failed == 0 ? "selfcheck RESULT=PASS\n" : "selfcheck RESULT=FAIL\n");
  return log.failed == 0 ? kExitOk : kExitFailure;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int run(int argc, char** argv) {
  if (argc >= 2) {
    const std::string first = argv[1];
    if (first == "help" || first == "--help" || first == "-h") {
      print_usage(stdout);
      return kExitOk;
    }
  }

  const ParsedCommandLine parsed = parse_command_line(argc, argv);
  if (!parsed.ok) {
    write_error(std::string(kProgramName) + ": " + parsed.error + "\n");
    print_usage(stderr);
    return kExitUsage;
  }

  const std::string& command = parsed.arguments.command;
  if (command == "demo") return run_demo();
  if (command == "plan") return run_plan(parsed.arguments);
  if (command == "compare") return run_compare(parsed.arguments);
  if (command == "inspect") return run_inspect(parsed.arguments);
  if (command == "selfcheck") return run_selfcheck();

  write_error(std::string(kProgramName) + ": unknown subcommand '" + command + "'\n");
  print_usage(stderr);
  return kExitUsage;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    write_error(std::string(kProgramName) + ": " +
                Error(ErrorCode::internal_error, std::string("unhandled exception: ") + error.what())
                    .format() +
                "\n");
  } catch (...) {
    write_error(std::string(kProgramName) + ": " +
                Error(ErrorCode::internal_error, "unhandled non-standard exception").format() +
                "\n");
  }
  return kExitFailure;
}
