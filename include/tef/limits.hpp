// Traffic Engineering Fabric - bounded resource limits.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace tef {

// Every externally reachable structure is bounded. These limits are part of the
// public contract: inputs exceeding them are rejected with a typed error rather
// than consuming unbounded memory or time.
struct Limits {
  // Model cardinality.
  static constexpr std::size_t max_demands = 4096;
  static constexpr std::size_t max_paths_per_demand = 64;
  static constexpr std::size_t max_total_candidate_paths = 65536;
  static constexpr std::size_t max_resources = 65536;
  static constexpr std::size_t max_resources_per_path = 256;
  static constexpr std::size_t max_reservations = 65536;
  static constexpr std::size_t max_failure_domains = 4096;
  static constexpr std::size_t max_domains_per_path = 16;
  static constexpr std::size_t max_constraint_refs = 64;
  static constexpr std::size_t max_explanation_entries = 128;
  static constexpr std::size_t max_alternatives = 32;
  static constexpr std::size_t max_audit_records = 1000000;
  static constexpr std::size_t max_lineage_depth = 1024;

  // Encoded sizes.
  static constexpr std::size_t max_name_length = 96;
  static constexpr std::size_t max_text_length = 512;
  static constexpr std::size_t max_frame_payload = 16u * 1024u * 1024u;
  static constexpr std::size_t max_frame_header = 32;
  static constexpr std::size_t max_connections = 64;
  static constexpr std::size_t max_publishers = 256;
  static constexpr std::size_t max_fences = 4096;
  static constexpr std::size_t max_journal_records = 1000000;

  // Solver bounds (deterministic; exceeding them yields an explicit outcome,
  // never a silent partial answer).
  static constexpr std::uint64_t max_solver_iterations = 4'000'000;
  static constexpr std::uint64_t max_solver_augmentations = 1'000'000;
  static constexpr std::uint64_t max_max_utilization_probes = 64;
  static constexpr std::size_t max_certificate_edges = 2000000;
  static constexpr std::size_t max_certificate_nodes = 200000;

  // Numeric bounds for authoritative arithmetic (bandwidth in fabric bandwidth
  // units, one unit == one kilobit per second).
  static constexpr std::int64_t max_bandwidth = 1'000'000'000'000'000LL;  // 1e15 kbit/s
  static constexpr std::int64_t max_cost = 1'000'000'000LL;
  static constexpr std::int64_t max_latency_micros = 1'000'000'000'000LL;
};

}  // namespace tef
