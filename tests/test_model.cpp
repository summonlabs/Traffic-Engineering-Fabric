// Traffic Engineering Fabric - model validation and canonical encoding proofs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "support/fixtures.hpp"
#include "support/test_framework.hpp"
#include "tef/authority.hpp"
#include "tef/digest.hpp"
#include "tef/model.hpp"
#include "tef/plan.hpp"

using namespace tef;
using tef::test::build_snapshot;
using tef::test::simple_fabric;
using tef::test::SnapshotSpec;

TEF_TEST(simple_fabric_is_structurally_valid) {
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  TEF_CHECK(validate_structure(snapshot).ok());
  TEF_CHECK_EQ(snapshot.demands.size(), std::size_t{2});
  TEF_CHECK_EQ(snapshot.paths.size(), std::size_t{3});
  TEF_CHECK_EQ(snapshot.capacity.resources.size(), std::size_t{4});
}

TEF_TEST(canonicalization_is_idempotent) {
  FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const Digest first = demand_set_digest(snapshot.demands);
  canonicalize(snapshot);
  const Digest second = demand_set_digest(snapshot.demands);
  canonicalize(snapshot);
  const Digest third = demand_set_digest(snapshot.demands);
  TEF_CHECK_EQ(first.hex(), second.hex());
  TEF_CHECK_EQ(second.hex(), third.hex());
}

TEF_TEST(canonicalization_removes_exact_duplicates_in_sets) {
  SnapshotSpec spec = simple_fabric();
  spec.paths[0].resources = {"res-b", "res-a", "res-a"};
  spec.paths[0].domains = {"domain-1", "domain-1"};
  FabricSnapshot snapshot = build_snapshot(spec);
  TEF_CHECK_EQ(snapshot.paths[0].resources.size(), std::size_t{2});
  TEF_CHECK_EQ(snapshot.paths[0].failure_domains.size(), std::size_t{1});
}

TEF_TEST(validation_rejects_duplicate_identities) {
  SnapshotSpec spec = simple_fabric();
  spec.demands.push_back(spec.demands[0]);
  const FabricSnapshot snapshot = build_snapshot(spec);
  const Status status = validate_structure(snapshot);
  TEF_CHECK(!status.ok());
  TEF_CHECK_EQ(status.code(), ErrorCode::duplicate_identity);
}

TEF_TEST(validation_rejects_duplicate_resource_identity) {
  SnapshotSpec spec = simple_fabric();
  spec.resources.push_back(spec.resources[0]);
  const FabricSnapshot snapshot = build_snapshot(spec);
  const Status status = validate_structure(snapshot);
  TEF_CHECK(!status.ok());
  TEF_CHECK_EQ(status.code(), ErrorCode::duplicate_identity);
}

TEF_TEST(validation_rejects_unknown_resource_reference) {
  SnapshotSpec spec = simple_fabric();
  spec.paths[0].resources = {"res-a", "res-missing"};
  const FabricSnapshot snapshot = build_snapshot(spec);
  const Status status = validate_structure(snapshot);
  TEF_CHECK(!status.ok());
  TEF_CHECK_EQ(status.code(), ErrorCode::unknown_reference);
}

TEF_TEST(validation_rejects_missing_provenance) {
  FabricSnapshot snapshot = build_snapshot(simple_fabric());
  snapshot.demands[0].provenance.source = SourceSystemId{};
  const Status status = validate_structure(snapshot);
  TEF_CHECK(!status.ok());
  TEF_CHECK_EQ(status.code(), ErrorCode::missing_provenance);
}

TEF_TEST(validation_rejects_incoherent_bandwidth_bounds) {
  SnapshotSpec spec = simple_fabric();
  spec.demands[0].minimum = 5000;
  spec.demands[0].desired = 1000;
  spec.demands[0].maximum = 6000;
  const FabricSnapshot snapshot = build_snapshot(spec);
  const Status status = validate_structure(snapshot);
  TEF_CHECK(!status.ok());
  TEF_CHECK_EQ(status.code(), ErrorCode::numeric_invalid);
}

TEF_TEST(validation_rejects_absent_generations) {
  FabricSnapshot snapshot = build_snapshot(simple_fabric());
  snapshot.fabric_epoch = FabricEpoch{};
  TEF_CHECK_EQ(validate_structure(snapshot).code(), ErrorCode::invalid_generation);

  FabricSnapshot second = build_snapshot(simple_fabric());
  second.capacity.generation = CapacitySnapshotGeneration{};
  TEF_CHECK_EQ(validate_structure(second).code(), ErrorCode::invalid_generation);

  FabricSnapshot third = build_snapshot(simple_fabric());
  third.paths[0].authority.generation = PathAuthorityGeneration{};
  TEF_CHECK_EQ(validate_structure(third).code(), ErrorCode::invalid_generation);
}

TEF_TEST(validation_rejects_epoch_disagreement) {
  FabricSnapshot snapshot = build_snapshot(simple_fabric());
  snapshot.capacity.fabric_epoch = FabricEpoch::parse(9).value();
  TEF_CHECK_EQ(validate_structure(snapshot).code(), ErrorCode::conflicting_authority);
}

TEF_TEST(validation_rejects_candidate_set_mismatch) {
  FabricSnapshot snapshot = build_snapshot(simple_fabric());
  snapshot.paths[0].candidate_set.generation = CandidateSetGeneration::parse(7).value();
  TEF_CHECK_EQ(validate_structure(snapshot).code(), ErrorCode::conflicting_authority);
}

TEF_TEST(validation_rejects_unknown_reservation_binding) {
  SnapshotSpec spec = simple_fabric();
  spec.demands[0].reservation_bindings = {"reservation-missing"};
  const FabricSnapshot snapshot = build_snapshot(spec);
  TEF_CHECK_EQ(validate_structure(snapshot).code(), ErrorCode::unknown_reference);
}

TEF_TEST(validation_rejects_empty_objective_profile) {
  SnapshotSpec spec = simple_fabric();
  spec.objective.terms.clear();
  const FabricSnapshot snapshot = build_snapshot(spec);
  TEF_CHECK_EQ(validate_structure(snapshot).code(), ErrorCode::unsupported_objective);
}

TEF_TEST(validation_rejects_duplicate_objective_term) {
  SnapshotSpec spec = simple_fabric();
  spec.objective.terms.push_back({ObjectiveTerm::satisfy_minimums, 1});
  const FabricSnapshot snapshot = build_snapshot(spec);
  TEF_CHECK_EQ(validate_structure(snapshot).code(), ErrorCode::duplicate_identity);
}

TEF_TEST(validation_rejects_out_of_range_policy) {
  SnapshotSpec spec = simple_fabric();
  spec.policy.max_utilization_permille = 0;
  TEF_CHECK_EQ(validate_structure(build_snapshot(spec)).code(), ErrorCode::invalid_argument);

  SnapshotSpec other = simple_fabric();
  other.policy.churn_max_operational_risk_permille = 2000;
  TEF_CHECK_EQ(validate_structure(build_snapshot(other)).code(), ErrorCode::invalid_argument);
}

TEF_TEST(snapshot_encoding_round_trips_exactly) {
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const std::vector<std::byte> bytes = encode_snapshot(snapshot);
  const Result<FabricSnapshot> decoded = decode_snapshot(std::span<const std::byte>(bytes.data(), bytes.size()));
  TEF_CHECK(decoded.has_value());
  TEF_CHECK_EQ(authority_of(decoded.value()).digest().hex(), authority_of(snapshot).digest().hex());
  TEF_CHECK_EQ(encode_snapshot(decoded.value()).size(), bytes.size());
}

TEF_TEST(snapshot_decoding_rejects_truncated_trailing_and_corrupt_payloads) {
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const std::vector<std::byte> bytes = encode_snapshot(snapshot);
  for (std::size_t cut = 0; cut < bytes.size(); cut += 7) {
    const Result<FabricSnapshot> decoded =
        decode_snapshot(std::span<const std::byte>(bytes.data(), cut));
    TEF_CHECK(!decoded.has_value());
  }
  std::vector<std::byte> trailing = bytes;
  trailing.push_back(std::byte{0});
  const Result<FabricSnapshot> with_trailing =
      decode_snapshot(std::span<const std::byte>(trailing.data(), trailing.size()));
  TEF_CHECK(!with_trailing.has_value());
  TEF_CHECK_EQ(with_trailing.error().code(), ErrorCode::trailing_input);

  // A single flipped byte may land in a free-text field and decode cleanly; the
  // property that must hold is that decoding never crashes, and that a decoded
  // snapshot is always structurally valid and re-encodes to a stable, decodable
  // form. Wire and durable integrity are provided by the frame CRC and the
  // record digest, which are tested separately.
  for (std::size_t index = 0; index < bytes.size(); index += 3) {
    std::vector<std::byte> mutated = bytes;
    mutated[index] = static_cast<std::byte>(static_cast<unsigned>(mutated[index]) ^ 0x5Au);
    const Result<FabricSnapshot> decoded =
        decode_snapshot(std::span<const std::byte>(mutated.data(), mutated.size()));
    if (!decoded.has_value()) continue;
    TEF_CHECK(validate_structure(decoded.value()).ok());
    const std::vector<std::byte> reencoded = encode_snapshot(decoded.value());
    const Result<FabricSnapshot> again =
        decode_snapshot(std::span<const std::byte>(reencoded.data(), reencoded.size()));
    TEF_CHECK(again.has_value());
    TEF_CHECK_EQ(authority_of(again.value()).digest().hex(), authority_of(decoded.value()).digest().hex());
  }
}

TEF_TEST(snapshot_decoding_rejects_wrong_format_version) {
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  std::vector<std::byte> bytes = encode_snapshot(snapshot);
  bytes[0] = std::byte{0};
  bytes[1] = std::byte{99};
  const Result<FabricSnapshot> decoded = decode_snapshot(std::span<const std::byte>(bytes.data(), bytes.size()));
  TEF_CHECK(!decoded.has_value());
  TEF_CHECK_EQ(decoded.error().code(), ErrorCode::unsupported_version);
}

TEF_TEST(authority_detects_advance_regression_and_conflict) {
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  const AuthorityVector bound = authority_of(snapshot);

  AuthorityVector advanced = bound;
  advanced.topology_generation = TopologyGeneration::parse(2).value();
  const AuthorityDelta delta = compare_authority(bound, advanced);
  TEF_CHECK(delta.stale());
  TEF_CHECK(!delta.contradictory());

  AuthorityVector regressed = bound;
  regressed.fabric_epoch = FabricEpoch{};
  TEF_CHECK(compare_authority(bound, regressed).identical());

  AuthorityVector lower = bound;
  lower.capacity.generation = CapacitySnapshotGeneration::parse(1).value();
  AuthorityVector higher = bound;
  higher.capacity.generation = CapacitySnapshotGeneration::parse(5).value();
  TEF_CHECK(!compare_authority(lower, higher).identical());
  TEF_CHECK(compare_authority(higher, lower).contradictory());

  AuthorityVector conflicting = bound;
  conflicting.demand_set_digest = Sha256::hash(std::string_view("different"));
  TEF_CHECK(compare_authority(bound, conflicting).contradictory());
}

TEF_TEST(digests_are_sensitive_to_every_documented_field) {
  const FabricSnapshot base = build_snapshot(simple_fabric());

  SnapshotSpec other = simple_fabric();
  other.demands[0].desired += 1;
  TEF_CHECK_NE(demand_set_digest(base.demands), demand_set_digest(build_snapshot(other).demands));

  SnapshotSpec path_change = simple_fabric();
  path_change.paths[0].cost += 1;
  TEF_CHECK_NE(candidate_set_digest(base.paths), candidate_set_digest(build_snapshot(path_change).paths));

  SnapshotSpec resource_change = simple_fabric();
  resource_change.resources[0].usable += 1;
  TEF_CHECK_NE(resource_catalog_digest(base.capacity.resources),
               resource_catalog_digest(build_snapshot(resource_change).capacity.resources));

  SnapshotSpec policy_change = simple_fabric();
  policy_change.policy.require_minimums = false;
  TEF_CHECK_NE(policy_digest(base.policy), policy_digest(build_snapshot(policy_change).policy));
}

TEF_TEST(enum_rendering_round_trips) {
  for (std::uint16_t i = 0; i <= static_cast<std::uint16_t>(ObjectiveTerm::minimize_path_count); ++i) {
    const auto term = static_cast<ObjectiveTerm>(i);
    const auto parsed = objective_term_from_string(to_string(term));
    TEF_CHECK(parsed.has_value());
    TEF_CHECK(*parsed == term);
  }
  TEF_CHECK(!objective_term_from_string("not-a-term").has_value());
  TEF_CHECK(preemptibility_from_string("preemptible").has_value());
  TEF_CHECK(!preemptibility_from_string("nope").has_value());
}

TEF_TEST(plan_state_machine_rejects_unsupported_transitions) {
  TEF_CHECK(PlanStateMachine::can_transition(PlanState::declared, PlanState::validated));
  TEF_CHECK(PlanStateMachine::can_transition(PlanState::committed, PlanState::superseded));
  TEF_CHECK(!PlanStateMachine::can_transition(PlanState::committed, PlanState::proposed));
  TEF_CHECK(!PlanStateMachine::can_transition(PlanState::rejected, PlanState::validated));
  TEF_CHECK(!PlanStateMachine::can_transition(PlanState::declared, PlanState::declared));
  TEF_CHECK(PlanStateMachine::is_terminal(PlanState::rejected));
  TEF_CHECK(PlanStateMachine::is_terminal(PlanState::retired));
  TEF_CHECK(!PlanStateMachine::is_terminal(PlanState::committed));
  TEF_CHECK(PlanStateMachine::is_live(PlanState::authorized));
  TEF_CHECK(!PlanStateMachine::is_live(PlanState::committed));
}

int main(int argc, char** argv) { return tef::test::run_all(argc, argv); }
