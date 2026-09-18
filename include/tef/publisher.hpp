// Traffic Engineering Fabric - publisher client.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "tef/authority.hpp"
#include "tef/engine.hpp"
#include "tef/net.hpp"
#include "tef/protocol.hpp"

namespace tef {

struct PublisherConfig {
  std::string address = "127.0.0.1";
  std::uint16_t port = 0;
  PublisherId publisher;
  BootId boot;
  std::string description;
};

// A publisher proposes traffic-engineering intent. It can never mutate
// authoritative state directly: every proposal is revalidated by the coordinator
// against the live authority before it can be committed.
class PublisherClient {
 public:
  static Result<std::unique_ptr<PublisherClient>> connect(const PublisherConfig& config,
                                                          std::string& transport_error);

  ~PublisherClient();
  PublisherClient(const PublisherClient&) = delete;
  PublisherClient& operator=(const PublisherClient&) = delete;

  Status handshake();
  Status register_publisher();

  Result<PlanResponse> submit_plan(const FabricSnapshot& snapshot, const DeclareRequest& request);
  Result<CommitResponse> commit(const PlanRef& plan, const CommitId& commit,
                                const CommitGeneration& generation);
  Result<SupersedeResponse> supersede(const PlanRef& incumbent, const PlanRef& replacement);
  Result<RetireResponse> retire(const PlanRef& plan);
  Result<QueryResponse> query(const PlanRef& plan, bool include_explanation);

  // Low-level frame access used by adversarial tests to emit malformed,
  // replayed, or out-of-epoch frames on a real socket.
  bool send_raw(const Frame& frame, std::string& error);
  bool receive_raw(Frame& frame, std::string& error);

  std::uint64_t coordinator_epoch() const noexcept;
  CoordinatorIncarnation coordinator_incarnation() const noexcept;
  const PublisherConfig& config() const noexcept;

 private:
  PublisherClient();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tef
