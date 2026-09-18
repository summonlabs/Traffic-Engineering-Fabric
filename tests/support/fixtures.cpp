// Traffic Engineering Fabric - deterministic test fixtures.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "support/fixtures.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tef::test {
namespace {

std::vector<FailureDomainId> domains_of(const std::vector<std::string>& names) {
  std::vector<FailureDomainId> out;
  for (const auto& name : names) out.push_back(FailureDomainId::parse(name).value());
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// splitmix64: a tiny, fully specified PRNG so that a seed reproduces a fabric on
// any platform and with any standard library.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  std::uint64_t range(std::uint64_t low, std::uint64_t high) {
    if (high <= low) return low;
    return low + (next() % (high - low + 1));
  }

 private:
  std::uint64_t state_;
};

}  // namespace

FabricSnapshot build_snapshot(const SnapshotSpec& spec) {
  FabricSnapshot snapshot;
  snapshot.fabric_epoch = FabricEpoch::parse(spec.fabric_epoch).value();
  snapshot.topology_generation = TopologyGeneration::parse(spec.topology_generation).value();
  snapshot.link_state_generation = LinkStateGeneration::parse(spec.link_state_generation).value();
  snapshot.path_authority_generation =
      PathAuthorityGeneration::parse(spec.path_authority_generation).value();
  snapshot.failure_domain_generation =
      FailureDomainGeneration::parse(spec.failure_domain_generation).value();
  snapshot.provenance = provenance("fabric-topology", spec.fabric_epoch);
  snapshot.evaluation_tick = spec.evaluation_tick;

  snapshot.candidate_set.id = CandidateSetId::parse(spec.candidate_set).value();
  snapshot.candidate_set.generation =
      CandidateSetGeneration::parse(spec.candidate_set_generation).value();

  snapshot.capacity.id = CapacitySnapshotId::parse("capacity-a").value();
  snapshot.capacity.generation = CapacitySnapshotGeneration::parse(1).value();
  snapshot.capacity.fabric_epoch = snapshot.fabric_epoch;
  snapshot.capacity.provenance = provenance("link-state-fabric", spec.link_state_generation);
  for (const auto& resource : spec.resources) {
    FabricResource entry;
    entry.id = ResourceId::parse(resource.id).value();
    entry.generation = ResourceGeneration::parse(spec.resource_generation).value();
    entry.usable_capacity = resource.usable;
    entry.committed_load = resource.committed;
    entry.failure_domains = domains_of(resource.domains);
    entry.provenance = provenance("link-state-fabric", spec.link_state_generation);
    snapshot.capacity.resources.push_back(std::move(entry));
  }

  snapshot.reservations.id = ReservationSnapshotId::parse("reservations-a").value();
  snapshot.reservations.generation = ReservationSnapshotGeneration::parse(1).value();
  snapshot.reservations.fabric_epoch = snapshot.fabric_epoch;
  snapshot.reservations.provenance =
      provenance("bandwidth-reservation-fabric", spec.reservation_generation);
  for (const auto& reservation : spec.reservations) {
    Reservation entry;
    entry.id = ReservationId::parse(reservation.id).value();
    entry.generation = ReservationGeneration::parse(spec.reservation_generation).value();
    entry.provenance = provenance("bandwidth-reservation-fabric", spec.reservation_generation);
    for (const auto& resource : reservation.resources) {
      entry.resources.push_back(ResourceId::parse(resource).value());
    }
    for (const auto& path : reservation.paths) entry.paths.push_back(PathId::parse(path).value());
    entry.bandwidth = reservation.bandwidth;
    entry.preemptibility = reservation.preemptibility;
    if (!reservation.owner.empty()) entry.owner = DemandId::parse(reservation.owner).value();
    entry.effective.start_tick = reservation.active ? 0 : 1000;
    entry.effective.open_ended = true;
    snapshot.reservations.reservations.push_back(std::move(entry));
  }

  snapshot.policy = spec.policy;
  snapshot.objective = spec.objective;

  for (const auto& path : spec.paths) {
    CandidatePath entry;
    entry.id = PathId::parse(path.id).value();
    entry.generation = PathGeneration::parse(spec.path_generation).value();
    entry.authority.id = entry.id;
    entry.authority.generation = snapshot.path_authority_generation;
    entry.candidate_set = snapshot.candidate_set;
    entry.provenance = provenance("path-authority", spec.path_authority_generation);
    for (const auto& resource : path.resources) {
      entry.resources.push_back(ResourceId::parse(resource).value());
    }
    entry.failure_domains = domains_of(path.domains);
    entry.cost = path.cost;
    if (path.has_latency) entry.latency_micros = path.latency_micros;
    entry.scope = path.scope;
    if (!path.scope_tenant.empty()) entry.scope_tenant = TenantId::parse(path.scope_tenant).value();
    if (!path.scope_service_class.empty()) {
      entry.scope_service_class = ServiceClassId::parse(path.scope_service_class).value();
    }
    snapshot.paths.push_back(std::move(entry));
  }

  for (const auto& demand : spec.demands) {
    Demand entry;
    entry.id = DemandId::parse(demand.id).value();
    entry.generation = DemandGeneration::parse(spec.demand_generation).value();
    entry.provenance = provenance("network-admission-fabric", spec.demand_generation);
    entry.tenant = TenantId::parse(demand.tenant).value();
    entry.service_class = ServiceClassId::parse(demand.service_class).value();
    entry.minimum_bandwidth = demand.minimum;
    entry.desired_bandwidth = demand.desired;
    entry.maximum_bandwidth = demand.maximum;
    entry.priority = demand.priority;
    if (demand.latency_bound.has_value()) entry.latency_bound_micros = demand.latency_bound;
    if (demand.cost_ceiling.has_value()) entry.path_cost_ceiling = demand.cost_ceiling;
    entry.candidate_set = snapshot.candidate_set;
    for (const auto& path : demand.allowed_paths) entry.allowed_paths.push_back(PathId::parse(path).value());
    for (const auto& path : demand.forbidden_paths) {
      entry.forbidden_paths.push_back(PathId::parse(path).value());
    }
    entry.required_failure_domains = domains_of(demand.required_domains);
    for (const auto& binding : demand.reservation_bindings) {
      entry.reservation_bindings.push_back(ReservationId::parse(binding).value());
    }
    entry.preemptibility = demand.preemptibility;
    snapshot.demands.push_back(std::move(entry));
  }

  canonicalize(snapshot);
  return snapshot;
}

SnapshotSpec simple_fabric() {
  SnapshotSpec spec;
  spec.resources = {
      {"res-a", 10000, 0, {"domain-1"}},
      {"res-b", 10000, 0, {"domain-1"}},
      {"res-c", 10000, 0, {"domain-2"}},
      {"res-d", 10000, 0, {"domain-2"}},
  };
  spec.paths = {
      {"path-ab", {"res-a", "res-b"}, {"domain-1"}, 1, true, 1000},
      {"path-cd", {"res-c", "res-d"}, {"domain-2"}, 2, true, 2000},
      {"path-ac", {"res-a", "res-c"}, {"domain-1", "domain-2"}, 3, true, 3000},
  };
  spec.demands = {
      {"demand-one", "tenant-a", "class-gold", 1000, 4000, 6000, 200},
      {"demand-two", "tenant-b", "class-silver", 500, 2000, 4000, 100},
  };
  return spec;
}

SnapshotSpec random_fabric(std::uint64_t seed, std::size_t demand_count, std::size_t path_count,
                           std::size_t resource_count) {
  Rng rng(seed);
  SnapshotSpec spec;
  const std::size_t resources = std::max<std::size_t>(resource_count, 1);
  const std::size_t paths = std::max<std::size_t>(path_count, 1);
  const std::size_t demands = std::max<std::size_t>(demand_count, 1);

  for (std::size_t i = 0; i < resources; ++i) {
    ResourceSpec resource;
    resource.id = "res-" + std::to_string(i);
    resource.usable = static_cast<std::int64_t>(rng.range(0, 20000));
    resource.committed = static_cast<std::int64_t>(rng.range(0, 2000));
    resource.domains = {"domain-" + std::to_string(rng.range(0, 3))};
    spec.resources.push_back(std::move(resource));
  }

  std::vector<std::string> tenants = {"tenant-a", "tenant-b", "tenant-c"};
  std::vector<std::string> classes = {"class-gold", "class-silver", "class-bronze"};

  for (std::size_t i = 0; i < paths; ++i) {
    PathSpec path;
    path.id = "path-" + std::to_string(i);
    path.cost = static_cast<std::int64_t>(rng.range(1, 50));
    path.latency_micros = static_cast<std::int64_t>(rng.range(100, 9000));
    std::vector<std::string> used;
    const std::size_t member_count = static_cast<std::size_t>(rng.range(1, 3));
    for (std::size_t m = 0; m < member_count; ++m) {
      const std::string candidate = "res-" + std::to_string(rng.range(0, resources - 1));
      if (std::find(used.begin(), used.end(), candidate) == used.end()) used.push_back(candidate);
    }
    if (used.empty()) used.push_back("res-0");
    path.resources = used;
    path.domains = {"domain-" + std::to_string(rng.range(0, 3))};
    spec.paths.push_back(std::move(path));
  }

  for (std::size_t i = 0; i < demands; ++i) {
    DemandSpec demand;
    demand.id = "demand-" + std::to_string(i);
    demand.tenant = tenants[rng.range(0, tenants.size() - 1)];
    demand.service_class = classes[rng.range(0, classes.size() - 1)];
    const std::int64_t desired = static_cast<std::int64_t>(rng.range(0, 5000));
    demand.desired = desired;
    demand.minimum = static_cast<std::int64_t>(rng.range(0, static_cast<std::uint64_t>(desired) + 1));
    demand.maximum = desired + static_cast<std::int64_t>(rng.range(0, 2000));
    demand.priority = static_cast<std::uint8_t>(rng.range(1, 255));
    spec.demands.push_back(std::move(demand));
  }

  return spec;
}

std::string temporary_directory(std::string_view tag) {
  const std::filesystem::path base = std::filesystem::temp_directory_path();
  static std::uint64_t counter = 0;
  ++counter;
  const std::filesystem::path path =
      base / ("tef-test-" + std::string(tag) + "-" + std::to_string(counter));
  std::error_code code;
  std::filesystem::remove_all(path, code);
  std::filesystem::create_directories(path, code);
  return path.string();
}

}  // namespace tef::test
