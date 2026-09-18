// Traffic Engineering Fabric - distributed coordinator.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tef/authority.hpp"
#include "tef/engine.hpp"
#include "tef/net.hpp"
#include "tef/persist.hpp"
#include "tef/protocol.hpp"

namespace tef {

struct CoordinatorConfig {
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 0;
  std::string data_directory;      // empty == in-memory (no durable state)
  std::uint64_t max_records = Limits::max_audit_records;
  std::size_t max_worker_threads = 8;
  std::size_t max_connections = Limits::max_connections;
  bool fsync = true;
};

// The coordinator is the single authority for committed traffic-engineering
// intent. Publisher identity is authenticated at the protocol level by binding
// every mutation to a publisher boot identity that the coordinator has accepted
// and that has not been fenced.
class Coordinator {
 public:
  static Result<std::unique_ptr<Coordinator>> create(const CoordinatorConfig& config);

  ~Coordinator();
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  // Binds the listener, advances the coordinator epoch, replays durable state and
  // starts the accept loop. Returns only once the socket is accepting.
  Status start();
  // Stops accepting new work, drains in-flight requests, joins workers, and
  // closes the listener. Never called while holding a lock a worker needs.
  Status stop();

  std::uint16_t port() const noexcept;
  std::string address() const;
  std::uint64_t epoch() const noexcept;
  BootId boot_id() const noexcept;
  CoordinatorIncarnation incarnation() const noexcept;

  Engine& engine() noexcept;
  PlanRepository* repository() noexcept;

  struct Stats {
    std::uint64_t connections_accepted = 0;
    std::uint64_t connections_rejected = 0;
    std::uint64_t frames_read = 0;
    std::uint64_t frames_written = 0;
    std::uint64_t frames_rejected = 0;
    std::uint64_t mutations_accepted = 0;
    std::uint64_t mutations_rejected = 0;
    std::uint64_t stale_epoch_rejections = 0;
    std::uint64_t fenced_rejections = 0;
    std::uint64_t duplicate_commit_replays = 0;
    std::uint64_t active_connections = 0;
    std::uint64_t peak_connections = 0;
  };
  Stats stats() const;

  std::vector<FenceRecord> fences() const;

 private:
  Coordinator();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tef
