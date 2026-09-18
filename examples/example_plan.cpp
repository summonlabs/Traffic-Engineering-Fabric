// Traffic Engineering Fabric - smallest complete planning example.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Demonstrates the whole library boundary in one file: declare authoritative
// inputs, plan against them, inspect the explanation, and commit the plan. The
// numbers are SYNTHETIC and describe no real network.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "tef/engine.hpp"
#include "tef/inspect.hpp"
#include "tef/tef.hpp"

using namespace tef;

namespace {

Provenance provenance_of(const char* system, std::uint64_t generation) {
  Provenance provenance;
  provenance.source = SourceSystemId::parse(system).value();
  provenance.source_generation = generation;
  provenance.observed_tick = generation;
  provenance.evidence_digest = Sha256::hash(std::string(system) + "#" + std::to_string(generation));
  provenance.detail = std::string(system) + " evidence";
  return provenance;
}

}  // namespace

int main() {
  FabricSnapshot snapshot;
  snapshot.fabric_epoch = FabricEpoch::parse(1).value();
  snapshot.topology_generation = TopologyGeneration::parse(1).value();
  snapshot.link_state_generation = LinkStateGeneration::parse(1).value();
  snapshot.path_authority_generation = PathAuthorityGeneration::parse(1).value();
  snapshot.failure_domain_generation = FailureDomainGeneration::parse(1).value();
  snapshot.provenance = provenance_of("fabric-topology", 1);
  snapshot.candidate_set.id = CandidateSetId::parse("candidates-example").value();
  snapshot.candidate_set.generation = CandidateSetGeneration::parse(1).value();

  snapshot.capacity.id = CapacitySnapshotId::parse("capacity-example").value();
  snapshot.capacity.generation = CapacitySnapshotGeneration::parse(1).value();
  snapshot.capacity.fabric_epoch = snapshot.fabric_epoch;
  snapshot.capacity.provenance = provenance_of("link-state-fabric", 1);
  const char* resource_names[] = {"res-a", "res-b", "res-c"};
  for (const char* name : resource_names) {
    FabricResource resource;
    resource.id = ResourceId::parse(name).value();
    resource.generation = ResourceGeneration::parse(1).value();
    resource.usable_capacity = 10000;
    resource.committed_load = 0;
    resource.failure_domains = {FailureDomainId::parse("domain-1").value()};
    resource.provenance = provenance_of("link-state-fabric", 1);
    snapshot.capacity.resources.push_back(resource);
  }

  snapshot.reservations.id = ReservationSnapshotId::parse("reservations-example").value();
  snapshot.reservations.generation = ReservationSnapshotGeneration::parse(1).value();
  snapshot.reservations.fabric_epoch = snapshot.fabric_epoch;
  snapshot.reservations.provenance = provenance_of("bandwidth-reservation-fabric", 1);

  snapshot.policy.id = PolicyId::parse("policy-example").value();
  snapshot.policy.generation = PolicyGeneration::parse(1).value();
  snapshot.policy.provenance = provenance_of("tef-config", 1);
  snapshot.policy.max_utilization_permille = 900;

  snapshot.objective.id = ObjectiveProfileId::parse("objective-example").value();
  snapshot.objective.generation = ObjectiveProfileGeneration::parse(1).value();
  snapshot.objective.provenance = provenance_of("tef-config", 1);
  snapshot.objective.terms = {{ObjectiveTerm::satisfy_minimums, 1000},
                              {ObjectiveTerm::minimize_total_path_cost, 1},
                              {ObjectiveTerm::maximize_desired_bandwidth, 1}};

  const auto make_path = [&](const char* id, std::vector<const char*> members, std::int64_t cost) {
    CandidatePath path;
    path.id = PathId::parse(id).value();
    path.generation = PathGeneration::parse(1).value();
    path.authority.id = path.id;
    path.authority.generation = snapshot.path_authority_generation;
    path.candidate_set = snapshot.candidate_set;
    path.provenance = provenance_of("path-authority", 1);
    for (const char* member : members) path.resources.push_back(ResourceId::parse(member).value());
    path.failure_domains = {FailureDomainId::parse("domain-1").value()};
    path.cost = cost;
    path.latency_micros = 1000;
    return path;
  };
  snapshot.paths.push_back(make_path("path-ab", {"res-a", "res-b"}, 1));
  snapshot.paths.push_back(make_path("path-ac", {"res-a", "res-c"}, 2));

  Demand demand;
  demand.id = DemandId::parse("demand-example").value();
  demand.generation = DemandGeneration::parse(1).value();
  demand.provenance = provenance_of("network-admission-fabric", 1);
  demand.tenant = TenantId::parse("tenant-example").value();
  demand.service_class = ServiceClassId::parse("class-gold").value();
  demand.minimum_bandwidth = 1000;
  demand.desired_bandwidth = 5000;
  demand.maximum_bandwidth = 8000;
  demand.priority = 200;
  demand.candidate_set = snapshot.candidate_set;
  snapshot.demands.push_back(demand);

  canonicalize(snapshot);
  const Status structural = validate_structure(snapshot);
  if (!structural.ok()) {
    std::printf("snapshot rejected: %s\n", structural.format().c_str());
    return 1;
  }

  EngineConfig config;
  config.coordinator_id = CoordinatorId::parse("tef-coordinator").value();
  config.coordinator_incarnation = CoordinatorIncarnation::first();
  Engine engine(config);

  const Status submitted = engine.submit_snapshot(snapshot);
  if (!submitted.ok()) {
    std::printf("snapshot rejected: %s\n", submitted.format().c_str());
    return 1;
  }

  DeclareRequest declare;
  declare.plan_id = PlanId::parse("plan-example").value();
  declare.attempt = AttemptId::parse("attempt-example").value();
  declare.attempt_generation = AttemptGeneration::first();
  declare.publisher = PublisherId::parse("publisher-example").value();
  declare.publisher_boot = derive_boot_id(7, 7);
  const Result<Plan> declared = engine.declare(declare);
  if (!declared.has_value()) {
    std::printf("declare rejected: %s\n", declared.error().format().c_str());
    return 1;
  }
  const Status validated = engine.validate(declared.value().ref());
  if (!validated.ok()) {
    std::printf("validate rejected: %s\n", validated.format().c_str());
    return 1;
  }
  const Result<Plan> solved = engine.solve(declared.value().ref());
  if (!solved.has_value()) {
    std::printf("solve rejected: %s\n", solved.error().format().c_str());
    return 1;
  }
  std::printf("%s", render_plan_summary(solved.value()).c_str());

  const auto explanation = engine.explain(solved.value().ref());
  if (explanation.has_value()) {
    std::printf("\n%s", explanation->to_text().c_str());
  }

  const Result<Plan> authorized = engine.authorize(solved.value().ref(), AuthorizeRequest{});
  if (!authorized.has_value()) {
    std::printf("\nauthorize refused: %s\n", authorized.error().format().c_str());
    return 0;
  }
  CommitIntent intent;
  intent.commit = CommitId::parse("commit-example").value();
  intent.commit_generation = CommitGeneration::first();
  const Result<Plan> committed = engine.commit(solved.value().ref(), intent);
  if (!committed.has_value()) {
    std::printf("\ncommit refused: %s\n", committed.error().format().c_str());
    return 1;
  }
  std::printf("\ncommitted plan %s generation %llu\n", committed.value().id.str().c_str(),
              static_cast<unsigned long long>(committed.value().generation.value()));

  return 0;
}
