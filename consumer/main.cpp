// Traffic Engineering Fabric - independent installed-package consumer.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// This program only uses the public headers and the exported CMake target. It is
// built against an installed package, never against the source tree.
#include <cstdio>
#include <string>

#include "tef/tef.hpp"

namespace {

tef::Provenance provenance_of(const char* system, std::uint64_t generation) {
  tef::Provenance provenance;
  provenance.source = tef::SourceSystemId::parse(system).value();
  provenance.source_generation = generation;
  provenance.observed_tick = generation;
  provenance.evidence_digest = tef::Sha256::hash(std::string(system) + "#" + std::to_string(generation));
  provenance.detail = std::string(system) + " evidence";
  return provenance;
}

}  // namespace

int main() {
  tef::FabricSnapshot snapshot;
  snapshot.fabric_epoch = tef::FabricEpoch::parse(1).value();
  snapshot.topology_generation = tef::TopologyGeneration::parse(1).value();
  snapshot.link_state_generation = tef::LinkStateGeneration::parse(1).value();
  snapshot.path_authority_generation = tef::PathAuthorityGeneration::parse(1).value();
  snapshot.failure_domain_generation = tef::FailureDomainGeneration::parse(1).value();
  snapshot.provenance = provenance_of("fabric-topology", 1);
  snapshot.candidate_set.id = tef::CandidateSetId::parse("candidates-consumer").value();
  snapshot.candidate_set.generation = tef::CandidateSetGeneration::parse(1).value();

  snapshot.capacity.id = tef::CapacitySnapshotId::parse("capacity-consumer").value();
  snapshot.capacity.generation = tef::CapacitySnapshotGeneration::parse(1).value();
  snapshot.capacity.fabric_epoch = snapshot.fabric_epoch;
  snapshot.capacity.provenance = provenance_of("link-state-fabric", 1);
  for (int i = 0; i < 2; ++i) {
    tef::FabricResource resource;
    resource.id = tef::ResourceId::parse("res-" + std::to_string(i)).value();
    resource.generation = tef::ResourceGeneration::parse(1).value();
    resource.usable_capacity = 5000;
    resource.provenance = provenance_of("link-state-fabric", 1);
    snapshot.capacity.resources.push_back(resource);
  }

  snapshot.reservations.id = tef::ReservationSnapshotId::parse("reservations-consumer").value();
  snapshot.reservations.generation = tef::ReservationSnapshotGeneration::parse(1).value();
  snapshot.reservations.fabric_epoch = snapshot.fabric_epoch;
  snapshot.reservations.provenance = provenance_of("bandwidth-reservation-fabric", 1);

  snapshot.policy.id = tef::PolicyId::parse("policy-consumer").value();
  snapshot.policy.generation = tef::PolicyGeneration::parse(1).value();
  snapshot.policy.provenance = provenance_of("tef-config", 1);

  snapshot.objective.id = tef::ObjectiveProfileId::parse("objective-consumer").value();
  snapshot.objective.generation = tef::ObjectiveProfileGeneration::parse(1).value();
  snapshot.objective.provenance = provenance_of("tef-config", 1);
  snapshot.objective.terms = {{tef::ObjectiveTerm::satisfy_minimums, 1000},
                              {tef::ObjectiveTerm::maximize_desired_bandwidth, 1}};

  tef::CandidatePath path;
  path.id = tef::PathId::parse("path-consumer").value();
  path.generation = tef::PathGeneration::parse(1).value();
  path.authority.id = path.id;
  path.authority.generation = snapshot.path_authority_generation;
  path.candidate_set = snapshot.candidate_set;
  path.provenance = provenance_of("path-authority", 1);
  path.resources = {tef::ResourceId::parse("res-0").value()};
  path.cost = 1;
  snapshot.paths.push_back(path);

  tef::Demand demand;
  demand.id = tef::DemandId::parse("demand-consumer").value();
  demand.generation = tef::DemandGeneration::parse(1).value();
  demand.provenance = provenance_of("network-admission-fabric", 1);
  demand.tenant = tef::TenantId::parse("tenant-consumer").value();
  demand.service_class = tef::ServiceClassId::parse("class-gold").value();
  demand.minimum_bandwidth = 1000;
  demand.desired_bandwidth = 3000;
  demand.maximum_bandwidth = 4000;
  demand.candidate_set = snapshot.candidate_set;
  snapshot.demands.push_back(demand);

  tef::canonicalize(snapshot);
  const tef::Status structural = tef::validate_structure(snapshot);
  if (!structural.ok()) {
    std::printf("FAILED: %s\n", structural.format().c_str());
    return 1;
  }

  tef::SolveOptions options;
  const tef::Result<tef::SolveOutcome> outcome = tef::solve(snapshot, options);
  if (!outcome.has_value()) {
    std::printf("FAILED: %s\n", outcome.error().format().c_str());
    return 1;
  }
  std::printf("library_version=%s\n", std::string(tef::kVersionString).c_str());
  std::printf("status=%s\n", std::string(tef::to_string(outcome.value().status)).c_str());
  std::printf("total_granted=%lld\n", static_cast<long long>(outcome.value().allocation.total_granted));
  std::printf("allocation_digest=%s\n", outcome.value().allocation_digest.hex().c_str());
  std::printf("authority_digest=%s\n", tef::authority_of(snapshot).digest().hex().c_str());
  if (!(outcome.value().status == tef::FeasibilityStatus::feasible) || !outcome.value().verified) {
    std::printf("FAILED: the installed package produced an unexpected outcome\n");
    return 1;
  }
  std::printf("CONSUMER OK\n");
  return 0;
}
