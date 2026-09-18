// Traffic Engineering Fabric - distributed protocol proofs over real TCP.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "support/fixtures.hpp"
#include "support/test_framework.hpp"
#include "tef/coordinator.hpp"
#include "tef/net.hpp"
#include "tef/publisher.hpp"
#include "tef/version.hpp"

using namespace tef;
using tef::test::SnapshotSpec;

namespace {

struct Cluster {
  std::unique_ptr<Coordinator> coordinator;
  std::uint16_t port = 0;
};

Cluster start_coordinator(const std::string& data_directory = std::string()) {
  CoordinatorConfig config;
  config.bind_address = "127.0.0.1";
  config.port = 0;
  config.data_directory = data_directory;
  Result<std::unique_ptr<Coordinator>> created = Coordinator::create(config);
  TEF_CHECK_MSG(created.has_value(), created.has_value() ? "" : created.error().format());
  Cluster cluster;
  cluster.coordinator = std::move(created.value());
  const Status started = cluster.coordinator->start();
  TEF_CHECK_MSG(started.ok(), started.format());
  cluster.port = cluster.coordinator->port();
  TEF_CHECK(cluster.port != 0);
  return cluster;
}

PublisherConfig publisher_config(std::uint16_t port, const char* name, std::uint64_t boot_hi,
                                 std::uint64_t boot_lo) {
  PublisherConfig config;
  config.address = "127.0.0.1";
  config.port = port;
  config.publisher = PublisherId::parse(name).value();
  config.boot = derive_boot_id(boot_hi, boot_lo);
  config.description = "distributed test publisher";
  return config;
}

std::unique_ptr<PublisherClient> connect_publisher(const PublisherConfig& config) {
  std::string error;
  Result<std::unique_ptr<PublisherClient>> client = PublisherClient::connect(config, error);
  TEF_CHECK_MSG(client.has_value(), client.has_value() ? "" : error);
  return std::move(client.value());
}

DeclareRequest declare_for(const std::string& plan, const PublisherConfig& config) {
  DeclareRequest request;
  request.plan_id = PlanId::parse(plan).value();
  request.attempt = AttemptId::parse("attempt-" + plan).value();
  request.attempt_generation = AttemptGeneration::first();
  request.publisher = config.publisher;
  request.publisher_boot = config.boot;
  return request;
}

FabricSnapshot demo_snapshot() {
  SnapshotSpec spec;
  spec.resources = {{"res-a", 10000, 0, {"domain-1"}}, {"res-b", 10000, 0, {"domain-2"}}};
  spec.paths = {{"path-a", {"res-a"}, {"domain-1"}, 1, true, 100, EligibilityScope::fabric_wide, "", ""},
                {"path-b", {"res-b"}, {"domain-2"}, 1, true, 100, EligibilityScope::fabric_wide, "", ""}};
  spec.demands = {{"demand-a", "tenant-a", "class-gold", 500, 2000, 4000, 100}};
  return tef::test::build_snapshot(spec);
}

}  // namespace

TEF_TEST(publisher_registers_plans_and_commits_over_real_tcp) {
  Cluster cluster = start_coordinator();
  const PublisherConfig config = publisher_config(cluster.port, "publisher-one", 1, 1);
  std::unique_ptr<PublisherClient> client = connect_publisher(config);

  TEF_CHECK(client->handshake().ok());
  TEF_CHECK(client->register_publisher().ok());
  TEF_CHECK_EQ(client->coordinator_epoch(), cluster.coordinator->epoch());

  const FabricSnapshot snapshot = demo_snapshot();
  const Result<PlanResponse> response =
      client->submit_plan(snapshot, declare_for("plan-net-1", config));
  TEF_CHECK_MSG(response.has_value(), response.has_value() ? "" : response.error().format());
  TEF_CHECK_EQ(response.value().code, ErrorCode::ok);
  TEF_CHECK_EQ(response.value().state, PlanState::proposed);
  TEF_CHECK_EQ(response.value().feasibility, FeasibilityStatus::feasible);
  TEF_CHECK(!response.value().explanation_json.empty());

  const Result<CommitResponse> committed = client->commit(
      response.value().plan, CommitId::parse("commit-net-1").value(), CommitGeneration::first());
  TEF_CHECK_MSG(committed.has_value(), committed.has_value() ? "" : committed.error().format());
  TEF_CHECK_EQ(committed.value().code, ErrorCode::ok);
  TEF_CHECK_EQ(committed.value().state, PlanState::committed);
  TEF_CHECK(!committed.value().commit_digest.is_zero());

  const Result<QueryResponse> query = client->query(response.value().plan, true);
  TEF_CHECK(query.has_value());
  TEF_CHECK_EQ(query.value().plan.state, PlanState::committed);
  TEF_CHECK_EQ(query.value().applicability, PlanApplicability::current);
  TEF_CHECK(!query.value().explanation_json.empty());

  TEF_CHECK(cluster.coordinator->stop().ok());
}

TEF_TEST(registration_of_a_new_boot_fences_the_previous_boot) {
  Cluster cluster = start_coordinator();
  const PublisherConfig first = publisher_config(cluster.port, "publisher-one", 1, 1);
  const PublisherConfig second = publisher_config(cluster.port, "publisher-one", 2, 2);

  std::unique_ptr<PublisherClient> first_client = connect_publisher(first);
  TEF_CHECK(first_client->handshake().ok());
  TEF_CHECK(first_client->register_publisher().ok());

  std::unique_ptr<PublisherClient> second_client = connect_publisher(second);
  TEF_CHECK(second_client->handshake().ok());
  TEF_CHECK(second_client->register_publisher().ok());

  const std::vector<FenceRecord> fences = cluster.coordinator->fences();
  TEF_CHECK_EQ(fences.size(), std::size_t{1});
  TEF_CHECK(fences[0].boot == first.boot);
  TEF_CHECK(fences[0].publisher == first.publisher);

  // The replaced incarnation can no longer register or mutate.
  std::unique_ptr<PublisherClient> revived = connect_publisher(first);
  TEF_CHECK(revived->handshake().ok());
  const Status rejected = revived->register_publisher();
  TEF_CHECK(!rejected.ok());
  TEF_CHECK_EQ(rejected.code(), ErrorCode::fenced);

  TEF_CHECK(cluster.coordinator->stop().ok());
}

TEF_TEST(a_mutation_from_a_stale_epoch_is_rejected) {
  Cluster cluster = start_coordinator();
  const PublisherConfig config = publisher_config(cluster.port, "publisher-one", 3, 3);
  std::unique_ptr<PublisherClient> client = connect_publisher(config);
  TEF_CHECK(client->handshake().ok());
  TEF_CHECK(client->register_publisher().ok());

  Frame forged;
  forged.version = static_cast<std::uint16_t>(kWireProtocolVersion);
  forged.type = static_cast<std::uint16_t>(MessageType::plan_request);
  PlanRequest request;
  request.binding.publisher = config.publisher;
  request.binding.publisher_boot = config.boot;
  request.binding.coordinator_incarnation = client->coordinator_incarnation();
  request.binding.coordinator_epoch = 999;
  request.binding.attempt = AttemptId::parse("attempt-forged").value();
  request.binding.attempt_generation = AttemptGeneration::first();
  request.plan_id = PlanId::parse("plan-forged").value();
  request.snapshot = encode_snapshot(demo_snapshot());
  forged.payload = encode(request);

  std::string error;
  TEF_CHECK(client->send_raw(forged, error));
  Frame reply;
  TEF_CHECK(client->receive_raw(reply, error));
  TEF_CHECK_EQ(static_cast<MessageType>(reply.type), MessageType::plan_response);
  const Result<PlanResponse> decoded =
      decode_plan_response(std::span<const std::byte>(reply.payload.data(), reply.payload.size()));
  TEF_CHECK(decoded.has_value());
  TEF_CHECK_EQ(decoded.value().code, ErrorCode::epoch_mismatch);
  TEF_CHECK(cluster.coordinator->stats().stale_epoch_rejections >= 1);
  TEF_CHECK_EQ(cluster.coordinator->stats().mutations_accepted, std::uint64_t{0});

  TEF_CHECK(cluster.coordinator->stop().ok());
}

TEF_TEST(duplicate_commit_frames_are_idempotent_over_the_wire) {
  Cluster cluster = start_coordinator();
  const PublisherConfig config = publisher_config(cluster.port, "publisher-one", 4, 4);
  std::unique_ptr<PublisherClient> client = connect_publisher(config);
  TEF_CHECK(client->handshake().ok());
  TEF_CHECK(client->register_publisher().ok());

  const Result<PlanResponse> response =
      client->submit_plan(demo_snapshot(), declare_for("plan-net-dup", config));
  TEF_CHECK(response.has_value());
  const CommitId commit = CommitId::parse("commit-net-dup").value();
  const Result<CommitResponse> first = client->commit(response.value().plan, commit, CommitGeneration::first());
  TEF_CHECK(first.has_value());
  TEF_CHECK_EQ(first.value().state, PlanState::committed);

  const Result<CommitResponse> replay = client->commit(response.value().plan, commit, CommitGeneration::first());
  TEF_CHECK(replay.has_value());
  TEF_CHECK_EQ(replay.value().code, ErrorCode::ok);
  TEF_CHECK(replay.value().idempotent_replay);
  TEF_CHECK(cluster.coordinator->stats().duplicate_commit_replays >= 1);

  const Result<CommitResponse> conflicting = client->commit(
      response.value().plan, CommitId::parse("commit-net-other").value(), CommitGeneration::parse(2).value());
  TEF_CHECK(conflicting.has_value());
  TEF_CHECK_EQ(conflicting.value().code, ErrorCode::already_committed);

  TEF_CHECK(cluster.coordinator->stop().ok());
}

TEF_TEST(a_frame_with_bad_magic_is_rejected_and_the_connection_is_closed) {
  Cluster cluster = start_coordinator();
  std::string error;
  TcpSocket socket;
  const Status connected = connect_tcp("127.0.0.1", cluster.port, socket, error);
  TEF_CHECK_MSG(connected.ok(), error);

  std::vector<std::byte> garbage(kFrameHeaderSize, std::byte{0x00});
  garbage[0] = std::byte{0x58};
  TEF_CHECK(socket.write_raw(std::span<const std::byte>(garbage.data(), garbage.size()), error));
  Frame frame;
  const bool read_ok = socket.read_frame(frame, error);
  TEF_CHECK(!read_ok);
  socket.close();
  TEF_CHECK(cluster.coordinator->stop().ok());
}

TEF_TEST(an_oversized_frame_length_is_rejected) {
  Cluster cluster = start_coordinator();
  std::string error;
  TcpSocket socket;
  TEF_CHECK(connect_tcp("127.0.0.1", cluster.port, socket, error).ok());

  std::vector<std::byte> header(kFrameHeaderSize, std::byte{0});
  header[0] = std::byte{0x54};
  header[1] = std::byte{0x45};
  header[2] = std::byte{0x46};
  header[3] = std::byte{0x31};
  header[4] = std::byte{0x00};
  header[5] = std::byte{0x01};
  const std::uint32_t oversized = 0xFFFFFFFFu;
  for (int i = 0; i < 4; ++i) {
    header[12 + static_cast<std::size_t>(i)] =
        static_cast<std::byte>(oversized >> (8 * (3 - i)));
  }
  TEF_CHECK(socket.write_raw(std::span<const std::byte>(header.data(), header.size()), error));
  Frame frame;
  TEF_CHECK(!socket.read_frame(frame, error));
  TEF_CHECK(!error.empty());
  socket.close();
  TEF_CHECK(cluster.coordinator->stop().ok());
}

TEF_TEST(a_publisher_that_never_registered_cannot_mutate_state) {
  Cluster cluster = start_coordinator();
  const PublisherConfig config = publisher_config(cluster.port, "publisher-unknown", 5, 5);
  std::unique_ptr<PublisherClient> client = connect_publisher(config);
  TEF_CHECK(client->handshake().ok());

  const Result<PlanResponse> response =
      client->submit_plan(demo_snapshot(), declare_for("plan-unregistered", config));
  TEF_CHECK(response.has_value());
  TEF_CHECK_EQ(response.value().code, ErrorCode::unauthorized);
  TEF_CHECK_EQ(cluster.coordinator->stats().mutations_accepted, std::uint64_t{0});
  TEF_CHECK_EQ(cluster.coordinator->engine().stats().plans_created, std::uint64_t{0});

  TEF_CHECK(cluster.coordinator->stop().ok());
}

TEF_TEST(a_truncated_payload_leaves_the_coordinator_healthy) {
  Cluster cluster = start_coordinator();
  std::string error;
  TcpSocket socket;
  TEF_CHECK(connect_tcp("127.0.0.1", cluster.port, socket, error).ok());

  // A header that promises 64 payload bytes, followed by only 8.
  std::vector<std::byte> header(kFrameHeaderSize, std::byte{0});
  header[0] = std::byte{0x54};
  header[1] = std::byte{0x45};
  header[2] = std::byte{0x46};
  header[3] = std::byte{0x31};
  header[5] = std::byte{0x01};
  header[7] = std::byte{0x01};
  const std::uint32_t length = 64;
  for (int i = 0; i < 4; ++i) {
    header[12 + static_cast<std::size_t>(i)] = static_cast<std::byte>(length >> (8 * (3 - i)));
  }
  std::vector<std::byte> partial(header);
  partial.insert(partial.end(), 8, std::byte{0x7F});
  TEF_CHECK(socket.write_raw(std::span<const std::byte>(partial.data(), partial.size()), error));
  socket.close();

  // The coordinator must still serve a well-behaved publisher afterwards.
  const PublisherConfig config = publisher_config(cluster.port, "publisher-healthy", 6, 6);
  std::unique_ptr<PublisherClient> client = connect_publisher(config);
  TEF_CHECK(client->handshake().ok());
  TEF_CHECK(client->register_publisher().ok());
  const Result<PlanResponse> response =
      client->submit_plan(demo_snapshot(), declare_for("plan-healthy", config));
  TEF_CHECK(response.has_value());
  TEF_CHECK_EQ(response.value().code, ErrorCode::ok);
  TEF_CHECK(cluster.coordinator->stats().frames_rejected >= 1);

  TEF_CHECK(cluster.coordinator->stop().ok());
}

TEF_TEST(the_coordinator_epoch_is_advanced_and_persisted_across_restarts) {
  const std::string directory = tef::test::temporary_directory("coordinator-epoch");
  std::uint64_t first_epoch = 0;
  BootId first_boot;
  {
    Cluster cluster = start_coordinator(directory);
    first_epoch = cluster.coordinator->epoch();
    first_boot = cluster.coordinator->boot_id();
    TEF_CHECK(first_boot.valid());
    TEF_CHECK(cluster.coordinator->stop().ok());
  }
  {
    Cluster cluster = start_coordinator(directory);
    TEF_CHECK(cluster.coordinator->epoch() > first_epoch);
    TEF_CHECK(!(cluster.coordinator->boot_id() == first_boot));
    TEF_CHECK(cluster.coordinator->stop().ok());
  }
}

TEF_TEST(committed_plans_survive_a_full_coordinator_restart_as_history) {
  const std::string directory = tef::test::temporary_directory("coordinator-history");
  const PublisherConfig config_template = publisher_config(0, "publisher-one", 7, 7);
  {
    Cluster cluster = start_coordinator(directory);
    const PublisherConfig config = publisher_config(cluster.port, "publisher-one", 7, 7);
    std::unique_ptr<PublisherClient> client = connect_publisher(config);
    TEF_CHECK(client->handshake().ok());
    TEF_CHECK(client->register_publisher().ok());
    const Result<PlanResponse> response =
        client->submit_plan(demo_snapshot(), declare_for("plan-history", config));
    TEF_CHECK(response.has_value());
    const Result<CommitResponse> committed = client->commit(
        response.value().plan, CommitId::parse("commit-history").value(), CommitGeneration::first());
    TEF_CHECK(committed.has_value());
    TEF_CHECK_EQ(committed.value().state, PlanState::committed);
    TEF_CHECK(cluster.coordinator->stop().ok());
  }
  {
    Cluster cluster = start_coordinator(directory);
    const RecoverReport report = cluster.coordinator->engine().recovery_report();
    TEF_CHECK_MSG(report.records_read > 0, "records_read=" + std::to_string(report.records_read));
    TEF_CHECK_MSG(report.plans_restored >= 1,
                  "plans_restored=" + std::to_string(report.plans_restored) +
                      " skipped=" + std::to_string(report.plans_skipped) +
                      " read=" + std::to_string(report.records_read));
    const auto restored = cluster.coordinator->engine().get(PlanId::parse("plan-history").value());
    TEF_CHECK(restored.has_value());
    TEF_CHECK_EQ(restored->state, PlanState::committed);
    TEF_CHECK_EQ(restored->applicability, PlanApplicability::revalidation_required);
    TEF_CHECK_EQ(restored->commit.str(), std::string("commit-history"));
    TEF_CHECK(cluster.coordinator->engine().recovery_report().plans_restored >= 1);
    TEF_CHECK(cluster.coordinator->stop().ok());
  }
  (void)config_template;
}

TEF_TEST(stop_is_idempotent_and_releases_connections) {
  Cluster cluster = start_coordinator();
  const PublisherConfig config = publisher_config(cluster.port, "publisher-one", 8, 8);
  std::unique_ptr<PublisherClient> client = connect_publisher(config);
  TEF_CHECK(client->handshake().ok());
  TEF_CHECK(client->register_publisher().ok());

  TEF_CHECK(cluster.coordinator->stop().ok());
  TEF_CHECK(cluster.coordinator->stop().ok());
  TEF_CHECK_EQ(cluster.coordinator->stats().active_connections, std::uint64_t{0});
}

int main(int argc, char** argv) { return tef::test::run_all(argc, argv); }
