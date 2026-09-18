// Traffic Engineering Fabric - cluster node process.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// This binary is the real operating-system process used by the multiprocess
// proofs. It is deliberately small: one process is either a coordinator or a
// publisher, and every scenario is a single deterministic protocol interaction.
//
//   tef-node coordinator --info-file PATH [--state-dir DIR] [--address ADDR] [--port N]
//   tef-node publisher   --address ADDR --port N --publisher ID --boot HI:LO
//                        --scenario NAME --out PATH [--plan ID] [--commit ID]
//                        [--epoch N] [--second-commit ID] [--max-frames N]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "tef/tef.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace tef;

namespace {

std::string argument(int argc, char** argv, const std::string& name, const std::string& fallback) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (name == argv[i]) return argv[i + 1];
  }
  return fallback;
}

bool has_flag(int argc, char** argv, const std::string& name) {
  for (int i = 1; i < argc; ++i) {
    if (name == argv[i]) return true;
  }
  return false;
}

void publish(const std::vector<std::string>& report, const std::string& out_path) {
  std::string joined;
  for (const auto& line : report) {
    joined += line;
    joined.push_back('\n');
  }
  if (out_path.empty()) {
    std::fputs(joined.c_str(), stdout);
  } else {
    std::ofstream out(out_path, std::ios::trunc);
    out << joined;
  }
  std::fflush(stdout);
}

std::uint64_t parse_u64(const std::string& text, std::uint64_t fallback) {
  try {
    return static_cast<std::uint64_t>(std::stoull(text));
  } catch (...) {
    return fallback;
  }
}

Provenance provenance_of(const char* system, std::uint64_t generation) {
  Provenance provenance;
  provenance.source = SourceSystemId::parse(system).value();
  provenance.source_generation = generation;
  provenance.observed_tick = generation;
  provenance.evidence_digest = Sha256::hash(std::string(system) + "#" + std::to_string(generation));
  provenance.detail = std::string(system) + " evidence";
  return provenance;
}

// A deterministic synthetic fabric. The same seed produces byte-identical
// snapshots in every process, which is what makes cross-process planning
// comparable.
FabricSnapshot build_snapshot(std::uint64_t seed, std::uint64_t epoch) {
  FabricSnapshot snapshot;
  snapshot.fabric_epoch = FabricEpoch::parse(epoch).value();
  snapshot.topology_generation = TopologyGeneration::parse(1).value();
  snapshot.link_state_generation = LinkStateGeneration::parse(1).value();
  snapshot.path_authority_generation = PathAuthorityGeneration::parse(1).value();
  snapshot.failure_domain_generation = FailureDomainGeneration::parse(1).value();
  snapshot.provenance = provenance_of("fabric-topology", 1);
  snapshot.candidate_set.id = CandidateSetId::parse("candidates-cluster").value();
  snapshot.candidate_set.generation = CandidateSetGeneration::parse(1).value();

  snapshot.capacity.id = CapacitySnapshotId::parse("capacity-cluster").value();
  snapshot.capacity.generation = CapacitySnapshotGeneration::parse(1).value();
  snapshot.capacity.fabric_epoch = snapshot.fabric_epoch;
  snapshot.capacity.provenance = provenance_of("link-state-fabric", 1);
  for (int i = 0; i < 4; ++i) {
    FabricResource resource;
    resource.id = ResourceId::parse("res-" + std::to_string(i)).value();
    resource.generation = ResourceGeneration::parse(1).value();
    resource.usable_capacity = 10000 + static_cast<std::int64_t>(seed);
    resource.committed_load = 0;
    resource.failure_domains = {FailureDomainId::parse("domain-1").value()};
    resource.provenance = provenance_of("link-state-fabric", 1);
    snapshot.capacity.resources.push_back(resource);
  }

  snapshot.reservations.id = ReservationSnapshotId::parse("reservations-cluster").value();
  snapshot.reservations.generation = ReservationSnapshotGeneration::parse(1).value();
  snapshot.reservations.fabric_epoch = snapshot.fabric_epoch;
  snapshot.reservations.provenance = provenance_of("bandwidth-reservation-fabric", 1);

  snapshot.policy.id = PolicyId::parse("policy-cluster").value();
  snapshot.policy.generation = PolicyGeneration::parse(1).value();
  snapshot.policy.provenance = provenance_of("tef-config", 1);
  snapshot.policy.max_utilization_permille = 1000;

  snapshot.objective.id = ObjectiveProfileId::parse("objective-cluster").value();
  snapshot.objective.generation = ObjectiveProfileGeneration::parse(1).value();
  snapshot.objective.provenance = provenance_of("tef-config", 1);
  snapshot.objective.terms = {{ObjectiveTerm::satisfy_minimums, 1000},
                              {ObjectiveTerm::maximize_desired_bandwidth, 1}};

  for (int i = 0; i < 3; ++i) {
    CandidatePath path;
    path.id = PathId::parse("path-" + std::to_string(i)).value();
    path.generation = PathGeneration::parse(1).value();
    path.authority.id = path.id;
    path.authority.generation = snapshot.path_authority_generation;
    path.candidate_set = snapshot.candidate_set;
    path.provenance = provenance_of("path-authority", 1);
    path.resources = {ResourceId::parse("res-" + std::to_string(i)).value()};
    path.failure_domains = {FailureDomainId::parse("domain-1").value()};
    path.cost = 1 + i;
    path.latency_micros = 1000 + i;
    snapshot.paths.push_back(path);
  }

  for (int i = 0; i < 2; ++i) {
    Demand demand;
    demand.id = DemandId::parse("demand-" + std::to_string(i)).value();
    demand.generation = DemandGeneration::parse(1).value();
    demand.provenance = provenance_of("network-admission-fabric", 1);
    demand.tenant = TenantId::parse("tenant-a").value();
    demand.service_class = ServiceClassId::parse("class-gold").value();
    demand.minimum_bandwidth = 1000;
    demand.desired_bandwidth = 4000;
    demand.maximum_bandwidth = 6000;
    demand.priority = static_cast<std::uint8_t>(200 - i);
    demand.candidate_set = snapshot.candidate_set;
    snapshot.demands.push_back(demand);
  }

  canonicalize(snapshot);
  return snapshot;
}

int run_coordinator(int argc, char** argv) {
  const std::string info_file = argument(argc, argv, "--info-file", "");
  const std::string state_dir = argument(argc, argv, "--state-dir", "");
  const std::string address = argument(argc, argv, "--address", "127.0.0.1");
  const std::uint16_t port =
      static_cast<std::uint16_t>(parse_u64(argument(argc, argv, "--port", "0"), 0));
  if (info_file.empty()) {
    std::fprintf(stderr, "coordinator requires --info-file\n");
    return 2;
  }

  CoordinatorConfig config;
  config.bind_address = address;
  config.port = port;
  config.data_directory = state_dir;
  Result<std::unique_ptr<Coordinator>> created = Coordinator::create(config);
  if (!created.has_value()) {
    std::fprintf(stderr, "create failed: %s\n", created.error().format().c_str());
    return 1;
  }
  std::unique_ptr<Coordinator> coordinator = std::move(created.value());
  const Status started = coordinator->start();
  if (!started.ok()) {
    std::fprintf(stderr, "start failed: %s\n", started.format().c_str());
    return 1;
  }

  std::ofstream info(info_file, std::ios::trunc);
  if (!info) {
    std::fprintf(stderr, "cannot write %s\n", info_file.c_str());
    return 1;
  }
  info << "address=" << coordinator->address() << "\n";
  info << "port=" << coordinator->port() << "\n";
  info << "epoch=" << coordinator->epoch() << "\n";
  info << "boot=" << coordinator->boot_id().hex() << "\n";
  info << "incarnation=" << coordinator->incarnation().value() << "\n";
  info << "pid=" <<
#if defined(_WIN32)
      static_cast<unsigned long long>(::GetCurrentProcessId())
#else
      static_cast<unsigned long long>(::getpid())
#endif
       << "\n";
  info.close();
  std::printf("READY port=%u epoch=%llu\n", static_cast<unsigned>(coordinator->port()),
              static_cast<unsigned long long>(coordinator->epoch()));
  std::fflush(stdout);

  // Runs until the process is terminated by its supervisor or by a shutdown
  // signal. There is no internal watchdog.
#if defined(_WIN32)
  // Blocks until this process is terminated; the process handle becomes
  // signalled exactly when the process exits, so no polling and no watchdog is
  // involved.
  (void)::WaitForSingleObject(::GetCurrentProcess(), INFINITE);
#else
  for (;;) {
    ::pause();
  }
#endif
  return 0;
}

int run_publisher(int argc, char** argv) {
  const std::string address = argument(argc, argv, "--address", "127.0.0.1");
  const std::uint16_t port =
      static_cast<std::uint16_t>(parse_u64(argument(argc, argv, "--port", "0"), 0));
  const std::string publisher_text = argument(argc, argv, "--publisher", "publisher-one");
  const std::string boot_text = argument(argc, argv, "--boot", "1:1");
  const std::string scenario = argument(argc, argv, "--scenario", "register");
  const std::string out_path = argument(argc, argv, "--out", "");
  const std::string plan_id = argument(argc, argv, "--plan", "plan-a");
  const std::string commit_id = argument(argc, argv, "--commit", "commit-a");
  const std::string second_commit = argument(argc, argv, "--second-commit", "commit-b");
  const std::uint64_t seed = parse_u64(argument(argc, argv, "--seed", "1"), 1);
  const std::uint64_t epoch_override = parse_u64(argument(argc, argv, "--epoch", "0"), 0);

  std::uint64_t boot_hi = 1;
  std::uint64_t boot_lo = 1;
  {
    const std::size_t colon = boot_text.find(':');
    if (colon != std::string::npos) {
      boot_hi = parse_u64(boot_text.substr(0, colon), 1);
      boot_lo = parse_u64(boot_text.substr(colon + 1), 1);
    }
  }

  std::vector<std::string> report;
  const auto emit = [&report](const std::string& key, const std::string& value) {
    report.push_back(key + "=" + value);
  };

  PublisherConfig config;
  config.address = address;
  config.port = port;
  config.publisher = PublisherId::parse(publisher_text).value();
  config.boot = derive_boot_id(boot_hi, boot_lo);
  config.description = "tef-node publisher";

  std::string transport_error;
  Result<std::unique_ptr<PublisherClient>> created = PublisherClient::connect(config, transport_error);
  if (!created.has_value()) {
    emit("connect", "FAILED");
    emit("detail", transport_error);
    emit("scenario", scenario);
    emit("result", "TRANSPORT_FAILURE");
  } else {
    std::unique_ptr<PublisherClient> client = std::move(created.value());
    emit("connect", "OK");
    emit("scenario", scenario);

    const Status handshake = client->handshake();
    emit("handshake", handshake.ok() ? "OK" : handshake.format());

    const FabricSnapshot snapshot = build_snapshot(seed, 1);
    DeclareRequest declare;
    declare.plan_id = PlanId::parse(plan_id).value();
    declare.attempt = AttemptId::parse("attempt-" + plan_id).value();
    declare.attempt_generation = AttemptGeneration::first();
    declare.publisher = config.publisher;
    declare.publisher_boot = config.boot;

    if (scenario == "register" || scenario == "hold") {
      const Status registered = client->register_publisher();
      emit("register", registered.ok() ? "OK" : registered.format());
      if (scenario == "hold") {
        // Publish the registration result before blocking, so that a supervisor
        // can observe that this process is registered and then kill it
        // mid-protocol.
        publish(report, out_path);
        if (registered.ok()) {
          Frame frame;
          std::string error;
          (void)client->receive_raw(frame, error);
          emit("hold_released", error.empty() ? "FRAME" : error);
        }
      }
    } else if (scenario == "plan-only") {
      const Status registered = client->register_publisher();
      emit("register", registered.ok() ? "OK" : registered.format());
      const Result<PlanResponse> response = client->submit_plan(snapshot, declare);
      if (response.has_value()) {
        emit("plan_code", std::string(to_string(response.value().code)));
        emit("plan_state", std::string(to_string(response.value().state)));
        emit("plan_feasibility", std::string(to_string(response.value().feasibility)));
        emit("plan_digest", response.value().plan_digest.hex());
      } else {
        emit("plan_code", std::string(to_string(response.error().code())));
        emit("plan_detail", response.error().detail());
      }
    } else if (scenario == "plan-commit" || scenario == "plan-commit-twice" ||
               scenario == "plan-commit-query") {
      const Status registered = client->register_publisher();
      emit("register", registered.ok() ? "OK" : registered.format());
      const Result<PlanResponse> response = client->submit_plan(snapshot, declare);
      if (!response.has_value()) {
        emit("plan_code", std::string(to_string(response.error().code())));
        emit("plan_detail", response.error().detail());
      } else {
        emit("plan_code", std::string(to_string(response.value().code)));
        emit("plan_state", std::string(to_string(response.value().state)));
        const Result<CommitResponse> committed = client->commit(
            response.value().plan, CommitId::parse(commit_id).value(), CommitGeneration::first());
        if (!committed.has_value()) {
          emit("commit_code", std::string(to_string(committed.error().code())));
          emit("commit_detail", committed.error().detail());
        } else {
          emit("commit_code", std::string(to_string(committed.value().code)));
          emit("commit_state", std::string(to_string(committed.value().state)));
          emit("commit_digest", committed.value().commit_digest.hex());
          emit("superseded", committed.value().churn.decision == ChurnDecision::no_incumbent ? "no"
                                                                                              : "compared");
        }
        if (scenario == "plan-commit-twice") {
          const Result<CommitResponse> replay = client->commit(
              response.value().plan, CommitId::parse(commit_id).value(), CommitGeneration::first());
          if (replay.has_value()) {
            emit("replay_code", std::string(to_string(replay.value().code)));
            emit("replay_idempotent", replay.value().idempotent_replay ? "true" : "false");
          } else {
            emit("replay_code", std::string(to_string(replay.error().code())));
          }
          const Result<CommitResponse> conflicting = client->commit(
              response.value().plan, CommitId::parse(second_commit).value(),
              CommitGeneration::parse(2).value());
          if (conflicting.has_value()) {
            emit("conflict_code", std::string(to_string(conflicting.value().code)));
          } else {
            emit("conflict_code", std::string(to_string(conflicting.error().code())));
          }
        }
        if (scenario == "plan-commit-query") {
          const Result<QueryResponse> query = client->query(response.value().plan, true);
          if (query.has_value()) {
            emit("query_state", std::string(to_string(query.value().plan.state)));
            emit("query_applicability", std::string(to_string(query.value().applicability)));
            emit("query_explanation_bytes", std::to_string(query.value().explanation_json.size()));
          } else {
            emit("query_code", std::string(to_string(query.error().code())));
          }
        }
      }
    } else if (scenario == "stale-epoch") {
      const Status registered = client->register_publisher();
      emit("register", registered.ok() ? "OK" : registered.format());
      // Forge a frame that carries a superseded coordinator epoch.
      Frame frame;
      frame.version = static_cast<std::uint16_t>(kWireProtocolVersion);
      frame.type = static_cast<std::uint16_t>(MessageType::plan_request);
      PlanRequest forged;
      forged.binding.publisher = config.publisher;
      forged.binding.publisher_boot = config.boot;
      forged.binding.coordinator_incarnation = client->coordinator_incarnation();
      forged.binding.coordinator_epoch = epoch_override == 0 ? 1 : epoch_override;
      forged.binding.attempt = AttemptId::parse("attempt-forged").value();
      forged.binding.attempt_generation = AttemptGeneration::first();
      forged.plan_id = declare.plan_id;
      forged.snapshot = encode_snapshot(snapshot);
      frame.payload = encode(forged);
      std::string error;
      if (!client->send_raw(frame, error)) {
        emit("send", "FAILED");
        emit("detail", error);
      } else {
        Frame reply;
        if (client->receive_raw(reply, error)) {
          if (static_cast<MessageType>(reply.type) == MessageType::plan_response) {
            const Result<PlanResponse> decoded = decode_plan_response(
                std::span<const std::byte>(reply.payload.data(), reply.payload.size()));
            if (decoded.has_value()) {
              emit("plan_code", std::string(to_string(decoded.value().code)));
              emit("plan_detail", decoded.value().detail);
            } else {
              emit("plan_code", "DECODE_FAILED");
            }
          } else {
            emit("frame_type", std::to_string(reply.type));
          }
        } else {
          emit("receive", error);
        }
      }
    } else if (scenario == "fenced") {
      // Registration with a boot identity that the coordinator is expected to
      // have fenced permanently.
      const Status registered = client->register_publisher();
      emit("register", registered.ok() ? "OK" : registered.format());
    } else if (scenario == "raw-bad-magic") {
      Frame frame;
      frame.version = static_cast<std::uint16_t>(kWireProtocolVersion);
      frame.type = static_cast<std::uint16_t>(MessageType::hello_request);
      frame.payload = encode(HelloRequest{static_cast<std::uint16_t>(kWireProtocolVersion)});
      std::string error;
      if (client->send_raw(frame, error)) emit("send", "OK");
      Frame reply;
      if (client->receive_raw(reply, error)) {
        emit("reply_type", std::to_string(reply.type));
      } else {
        emit("connection", error);
      }
    } else {
      emit("result", "UNKNOWN_SCENARIO");
    }
    emit("coordinator_epoch", std::to_string(client->coordinator_epoch()));
  }

  emit("publisher", publisher_text);
  emit("boot", config.boot.hex());

  publish(report, out_path);
  return 0;
}

void print_usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  tef-node coordinator --info-file PATH [--state-dir DIR] [--address ADDR] [--port N]\n"
               "  tef-node publisher --address ADDR --port N --publisher ID --boot HI:LO\n"
               "                     --scenario NAME --out PATH [--plan ID] [--commit ID]\n"
               "                     [--second-commit ID] [--epoch N] [--seed N]\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 2;
  }
  const std::string role = argv[1];
  if (role == "coordinator") return run_coordinator(argc, argv);
  if (role == "publisher") return run_publisher(argc, argv);
  if (has_flag(argc, argv, "--help")) {
    print_usage();
    return 0;
  }
  print_usage();
  return 2;
}
