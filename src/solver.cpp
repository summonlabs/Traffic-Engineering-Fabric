// Traffic Engineering Fabric - deterministic allocation solver.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The algorithm implemented here is described in docs/algorithm.md. In brief:
//
//   1. derive()  turns the authoritative snapshot into residuals, reservation
//      accounting and per-demand eligibility (pure accounting, no invention).
//   2. certify() solves a deterministic max-flow relaxation purely to *prove*
//      infeasibility. A feasible relaxation never authorises anything.
//   3. Hard minimums are placed by deterministic bottleneck water-filling in
//      canonical order, with a bounded local repair pass that relocates
//      lower-priority bandwidth out of the way.
//   4. Soft objectives shape the remaining distribution through explicit,
//      deterministic ordering rules and a bounded parametric search on the
//      utilisation ceiling.
//   5. The constructed allocation is re-verified by an independent verifier
//      (verify_allocation) before it is returned.
#include "tef/solver.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "tef/numeric.hpp"

namespace tef {
namespace {

using Bandwidth = std::int64_t;

constexpr Bandwidth kInfinite = std::numeric_limits<Bandwidth>::max() / 8;

// ---------------------------------------------------------------------------
// Deterministic max-flow (Dinic) used only for infeasibility certificates.
// ---------------------------------------------------------------------------

class FlowNetwork {
 public:
  explicit FlowNetwork(std::size_t node_count) : adjacency_(node_count) {}

  std::size_t add_node() {
    adjacency_.emplace_back();
    return adjacency_.size() - 1;
  }

  void add_edge(std::size_t from, std::size_t to, Bandwidth capacity) {
    Edge forward{to, capacity, static_cast<int>(adjacency_[to].size())};
    Edge backward{from, 0, static_cast<int>(adjacency_[from].size())};
    adjacency_[from].push_back(forward);
    adjacency_[to].push_back(backward);
  }

  std::size_t node_count() const noexcept { return adjacency_.size(); }

  // Returns the max-flow value, or -1 when the bounded budget is exhausted.
  Bandwidth max_flow(std::size_t source, std::size_t sink, std::uint64_t max_iterations,
                     std::uint64_t& iterations) {
    Bandwidth total = 0;
    std::vector<int> level(adjacency_.size(), -1);
    std::vector<std::size_t> next(adjacency_.size(), 0);
    while (true) {
      if (!build_levels(source, sink, level, iterations)) break;
      if (iterations > max_iterations) return -1;
      std::fill(next.begin(), next.end(), 0);
      while (true) {
        if (iterations > max_iterations) return -1;
        const Bandwidth pushed = augment(source, sink, kInfinite, level, next, iterations);
        if (pushed <= 0) break;
        total = sat_add(total, pushed);
      }
    }
    return total;
  }

  // Nodes reachable from 'source' in the residual graph.
  std::vector<char> source_side(std::size_t source) const {
    std::vector<char> seen(adjacency_.size(), 0);
    std::deque<std::size_t> queue;
    seen[source] = 1;
    queue.push_back(source);
    while (!queue.empty()) {
      const std::size_t node = queue.front();
      queue.pop_front();
      for (const auto& edge : adjacency_[node]) {
        if (edge.capacity > 0 && seen[edge.to] == 0) {
          seen[edge.to] = 1;
          queue.push_back(edge.to);
        }
      }
    }
    return seen;
  }

 private:
  struct Edge {
    std::size_t to;
    Bandwidth capacity;
    int reverse;
  };

  bool build_levels(std::size_t source, std::size_t sink, std::vector<int>& level,
                    std::uint64_t& iterations) {
    std::fill(level.begin(), level.end(), -1);
    std::deque<std::size_t> queue;
    level[source] = 0;
    queue.push_back(source);
    while (!queue.empty()) {
      ++iterations;
      const std::size_t node = queue.front();
      queue.pop_front();
      for (const auto& edge : adjacency_[node]) {
        if (edge.capacity > 0 && level[edge.to] < 0) {
          level[edge.to] = level[node] + 1;
          queue.push_back(edge.to);
        }
      }
    }
    return level[sink] >= 0;
  }

  Bandwidth augment(std::size_t node, std::size_t sink, Bandwidth limit, std::vector<int>& level,
                    std::vector<std::size_t>& next, std::uint64_t& iterations) {
    if (node == sink) return limit;
    for (; next[node] < adjacency_[node].size(); ++next[node]) {
      ++iterations;
      Edge& edge = adjacency_[node][next[node]];
      if (edge.capacity <= 0 || level[edge.to] != level[node] + 1) continue;
      const Bandwidth pushed =
          augment(edge.to, sink, std::min(limit, edge.capacity), level, next, iterations);
      if (pushed > 0) {
        edge.capacity -= pushed;
        adjacency_[edge.to][static_cast<std::size_t>(edge.reverse)].capacity += pushed;
        return pushed;
      }
    }
    return 0;
  }

  std::vector<std::vector<Edge>> adjacency_;
};

// ---------------------------------------------------------------------------
// Working problem state
// ---------------------------------------------------------------------------

struct ResourceState {
  ResourceId id;
  ResourceGeneration generation;
  Bandwidth usable = 0;
  Bandwidth committed = 0;
  Bandwidth reserved = 0;
  Bandwidth available = 0;
  Bandwidth ceiling = 0;   // residual after the active utilisation ceiling
  Bandwidth allocated = 0;
};

struct DemandState {
  const DemandDerivation* derivation = nullptr;
  Bandwidth granted = 0;
  Bandwidth reservation_floor = 0;   // portion of the floor owned by reservations
  std::map<PathId, Bandwidth> shares;
  std::vector<RejectedAlternative> alternatives;
  std::string exclusion;             // non-empty when policy removed the demand
};

struct Problem {
  const FabricSnapshot* snapshot = nullptr;
  const Derivation* derivation = nullptr;
  std::map<ResourceId, ResourceState> resources;
  std::map<PathId, const CandidatePath*> paths;
  std::map<DemandId, DemandState> demands;
  Bandwidth ceiling_permille = 1000;
};

void recompute_ceiling(ResourceState& resource, Bandwidth ceiling_permille) {
  const auto scaled = mul_checked(resource.usable, ceiling_permille);
  const Bandwidth cap = scaled ? (*scaled / 1000) : resource.usable;
  const auto claimed = add_checked(resource.committed, resource.reserved);
  Bandwidth headroom = cap;
  if (claimed) headroom = cap > *claimed ? cap - *claimed : 0;
  resource.ceiling = std::max<Bandwidth>(0, std::min(resource.available, headroom));
}

void set_ceiling(Problem& problem, Bandwidth ceiling_permille) {
  problem.ceiling_permille = ceiling_permille;
  for (auto& kv : problem.resources) recompute_ceiling(kv.second, ceiling_permille);
}

Bandwidth path_headroom(const Problem& problem, const CandidatePath& path) {
  Bandwidth best = kInfinite;
  for (const auto& resource_id : path.resources) {
    const auto it = problem.resources.find(resource_id);
    if (it == problem.resources.end()) return 0;
    const Bandwidth headroom = it->second.ceiling - it->second.allocated;
    if (headroom < best) best = headroom;
    if (best <= 0) return 0;
  }
  return best == kInfinite ? 0 : best;
}

void consume(Problem& problem, const CandidatePath& path, Bandwidth amount) {
  for (const auto& resource_id : path.resources) {
    auto it = problem.resources.find(resource_id);
    if (it != problem.resources.end()) it->second.allocated = sat_add(it->second.allocated, amount);
  }
}

void release(Problem& problem, const CandidatePath& path, Bandwidth amount) {
  for (const auto& resource_id : path.resources) {
    auto it = problem.resources.find(resource_id);
    if (it != problem.resources.end()) {
      it->second.allocated = std::max<Bandwidth>(0, it->second.allocated - amount);
    }
  }
}

void grant(Problem& problem, DemandState& demand, const CandidatePath& path, Bandwidth amount) {
  if (amount <= 0) return;
  demand.shares[path.id] = sat_add(demand.shares[path.id], amount);
  demand.granted = sat_add(demand.granted, amount);
  consume(problem, path, amount);
}

void revoke(Problem& problem, DemandState& demand, const CandidatePath& path, Bandwidth amount) {
  if (amount <= 0) return;
  const auto it = demand.shares.find(path.id);
  if (it == demand.shares.end()) return;
  const Bandwidth taken = std::min(amount, it->second);
  it->second -= taken;
  demand.granted = std::max<Bandwidth>(0, demand.granted - taken);
  release(problem, path, taken);
  if (it->second == 0) demand.shares.erase(it);
}

std::vector<DemandId> canonical_processing_order(const Derivation& derivation) {
  std::vector<const DemandDerivation*> active;
  for (const auto& demand : derivation.demands) {
    if (demand.active) active.push_back(&demand);
  }
  std::sort(active.begin(), active.end(), [](const DemandDerivation* a, const DemandDerivation* b) {
    if (a->priority != b->priority) return a->priority > b->priority;
    return a->demand < b->demand;
  });
  std::vector<DemandId> order;
  order.reserve(active.size());
  for (const auto* demand : active) order.push_back(demand->demand);
  return order;
}

std::map<std::string, Bandwidth> failure_domain_load(const Problem& problem) {
  std::map<std::string, Bandwidth> load;
  for (const auto& kv : problem.demands) {
    for (const auto& share : kv.second.shares) {
      if (share.second <= 0) continue;
      const CandidatePath* path = problem.paths.at(share.first);
      for (const auto& domain : path->failure_domains) {
        load[domain.str()] = sat_add(load[domain.str()], share.second);
      }
    }
  }
  return load;
}

bool incumbent_shares(const Allocation* incumbent, const DemandId& demand,
                      std::set<PathId>& out) {
  if (incumbent == nullptr) return false;
  const DemandAllocation* entry = incumbent->find_demand(demand);
  if (entry == nullptr) return false;
  for (const auto& share : entry->shares) {
    if (share.granted > 0) out.insert(share.path);
  }
  return !out.empty();
}

// Deterministic path preference order for a demand.
std::vector<PathId> order_paths(const Problem& problem, const DemandState& demand,
                                const SolveOptions& options) {
  std::vector<PathId> order = demand.derivation->eligible_paths;
  const ObjectiveProfile& profile = problem.snapshot->objective;
  const bool churn_aware = options.has_incumbent && profile.has(ObjectiveTerm::minimize_churn);
  const bool spread_domains = profile.has(ObjectiveTerm::minimize_failure_domain_concentration);
  std::set<PathId> incumbent_paths;
  if (churn_aware) incumbent_shares(&options.incumbent, demand.derivation->demand, incumbent_paths);
  const std::map<std::string, Bandwidth> load =
      spread_domains ? failure_domain_load(problem) : std::map<std::string, Bandwidth>{};

  const auto domain_score = [&load](const CandidatePath& path) {
    Bandwidth total = 0;
    for (const auto& domain : path.failure_domains) {
      const auto it = load.find(domain.str());
      if (it != load.end()) total = sat_add(total, it->second);
    }
    return total;
  };

  std::stable_sort(order.begin(), order.end(), [&](const PathId& a_id, const PathId& b_id) {
    const CandidatePath& a = *problem.paths.at(a_id);
    const CandidatePath& b = *problem.paths.at(b_id);
    if (churn_aware) {
      const bool a_used = incumbent_paths.count(a_id) != 0;
      const bool b_used = incumbent_paths.count(b_id) != 0;
      if (a_used != b_used) return a_used;
    }
    if (spread_domains) {
      const Bandwidth a_load = domain_score(a);
      const Bandwidth b_load = domain_score(b);
      if (a_load != b_load) return a_load < b_load;
    }
    if (a.cost != b.cost) return a.cost < b.cost;
    const Bandwidth a_latency = a.latency_micros.value_or(Limits::max_latency_micros + 1);
    const Bandwidth b_latency = b.latency_micros.value_or(Limits::max_latency_micros + 1);
    if (a_latency != b_latency) return a_latency < b_latency;
    return a.id < b.id;
  });
  return order;
}

Bandwidth place(Problem& problem, DemandState& demand, const std::vector<PathId>& order,
                Bandwidth need) {
  // The authoritative maximum is a hard ceiling: no placement path may exceed
  // it, whatever the caller asks for.
  const Bandwidth allowed = demand.derivation->maximum - demand.granted;
  if (allowed <= 0) return 0;
  if (need > allowed) need = allowed;
  Bandwidth placed = 0;
  for (const auto& path_id : order) {
    if (placed >= need) break;
    const CandidatePath* path = problem.paths.at(path_id);
    const Bandwidth headroom = path_headroom(problem, *path);
    if (headroom <= 0) continue;
    const Bandwidth take = std::min(headroom, need - placed);
    grant(problem, demand, *path, take);
    placed = sat_add(placed, take);
  }
  return placed;
}

bool repair_resource(Problem& problem, const DemandId& blocked, const ResourceId& resource,
                     Bandwidth deficit, std::uint64_t& iterations, std::uint64_t max_iterations,
                     std::string& note) {
  struct Mover {
    DemandId demand;
    PathId path;
    Bandwidth granted;
    std::uint8_t priority;
  };
  std::vector<Mover> movers;
  for (const auto& kv : problem.demands) {
    if (kv.first == blocked) continue;
    for (const auto& share : kv.second.shares) {
      if (share.second <= 0) continue;
      const CandidatePath* path = problem.paths.at(share.first);
      if (std::find(path->resources.begin(), path->resources.end(), resource) ==
          path->resources.end()) {
        continue;
      }
      movers.push_back(Mover{kv.first, share.first, share.second, kv.second.derivation->priority});
    }
  }
  std::sort(movers.begin(), movers.end(), [](const Mover& a, const Mover& b) {
    if (a.priority != b.priority) return a.priority < b.priority;
    if (!(a.demand == b.demand)) return a.demand < b.demand;
    return a.path < b.path;
  });

  bool moved_any = false;
  for (const auto& mover : movers) {
    if (deficit <= 0) break;
    if (iterations > max_iterations) break;
    DemandState& state = problem.demands.at(mover.demand);
    const CandidatePath* from = problem.paths.at(mover.path);
    const Bandwidth wanted = std::min(deficit, mover.granted);
    Bandwidth relocated = 0;
    for (const auto& candidate_id : state.derivation->eligible_paths) {
      if (relocated >= wanted) break;
      if (candidate_id == mover.path) continue;
      const CandidatePath* to = problem.paths.at(candidate_id);
      if (std::find(to->resources.begin(), to->resources.end(), resource) != to->resources.end()) {
        continue;
      }
      ++iterations;
      const Bandwidth headroom = path_headroom(problem, *to);
      const Bandwidth take = std::min(headroom, wanted - relocated);
      if (take <= 0) continue;
      grant(problem, state, *to, take);
      relocated = sat_add(relocated, take);
    }
    if (relocated > 0) {
      revoke(problem, state, *from, relocated);
      deficit -= relocated;
      moved_any = true;
    }
  }
  if (moved_any) note = "relocated lower-priority bandwidth off resource " + resource.str();
  return deficit <= 0;
}

// ---------------------------------------------------------------------------
// Certificate
// ---------------------------------------------------------------------------

struct Certificate {
  bool built = false;
  bool feasible = false;
  Bandwidth achievable = 0;
  Bandwidth required = 0;
  bool only_path_starved = false;
  std::vector<ResourceId> saturated_resources;
  std::vector<DemandId> unreachable_demands;
};

Certificate certify(const Problem& problem, Bandwidth ceiling_permille) {
  Certificate result;
  const Derivation& derivation = *problem.derivation;

  std::vector<const DemandDerivation*> demands;
  std::size_t path_starved = 0;
  for (const auto& demand : derivation.demands) {
    if (!demand.active || demand.floor <= 0) continue;
    result.required = sat_add(result.required, demand.floor);
    if (demand.eligible_paths.empty()) {
      ++path_starved;
      result.unreachable_demands.push_back(demand.demand);
      continue;
    }
    demands.push_back(&demand);
  }
  result.only_path_starved = path_starved > 0;
  result.built = true;

  if (demands.empty()) {
    result.achievable = 0;
    result.feasible = path_starved == 0;
    result.only_path_starved = path_starved > 0;
    return result;
  }

  std::set<PathId> used_paths;
  for (const auto* demand : demands) {
    for (const auto& path : demand->eligible_paths) used_paths.insert(path);
  }

  const std::size_t node_budget = 2 + demands.size() + used_paths.size() + problem.resources.size();
  if (node_budget > Limits::max_certificate_nodes) {
    result.built = false;
    return result;
  }

  std::map<PathId, std::size_t> path_nodes;
  std::map<ResourceId, std::size_t> resource_nodes;
  std::map<DemandId, std::size_t> demand_nodes;

  const std::size_t source = 0;
  const std::size_t sink = 1;
  FlowNetwork network(2);
  for (const auto* demand : demands) demand_nodes[demand->demand] = network.add_node();
  for (const auto& path : used_paths) path_nodes[path] = network.add_node();
  for (const auto& kv : problem.resources) resource_nodes[kv.first] = network.add_node();

  std::size_t edges = 0;
  for (const auto* demand : demands) {
    network.add_edge(source, demand_nodes[demand->demand], demand->floor);
    ++edges;
    for (const auto& path_id : demand->eligible_paths) {
      network.add_edge(demand_nodes[demand->demand], path_nodes[path_id], kInfinite);
      ++edges;
      if (edges > Limits::max_certificate_edges) {
        result.built = false;
        return result;
      }
    }
  }
  for (const auto& path_id : used_paths) {
    const CandidatePath* path = problem.paths.at(path_id);
    for (const auto& resource_id : path->resources) {
      network.add_edge(path_nodes[path_id], resource_nodes[resource_id], kInfinite);
      ++edges;
      if (edges > Limits::max_certificate_edges) {
        result.built = false;
        return result;
      }
    }
  }
  for (const auto& kv : problem.resources) {
    ResourceState state = kv.second;
    recompute_ceiling(state, ceiling_permille);
    network.add_edge(resource_nodes[kv.first], sink, state.ceiling);
  }

  std::uint64_t iterations = 0;
  const Bandwidth flow = network.max_flow(source, sink, Limits::max_solver_iterations, iterations);
  if (flow < 0) {
    result.built = false;
    return result;
  }
  result.achievable = flow;
  result.feasible = flow >= result.required;

  if (!result.feasible) {
    const std::vector<char> reachable = network.source_side(source);
    for (const auto& kv : resource_nodes) {
      if (reachable[kv.second] == 0) result.saturated_resources.push_back(kv.first);
    }
    std::sort(result.saturated_resources.begin(), result.saturated_resources.end());
    for (const auto& kv : demand_nodes) {
      if (reachable[kv.second] == 0) result.unreachable_demands.push_back(kv.first);
    }
    std::sort(result.unreachable_demands.begin(), result.unreachable_demands.end());
  }
  return result;
}

// ---------------------------------------------------------------------------
// Objective scoring
// ---------------------------------------------------------------------------

std::string unit_of(ObjectiveTerm term) {
  switch (term) {
    case ObjectiveTerm::satisfy_minimums: return "fbu";
    case ObjectiveTerm::minimize_max_utilization: return "permille";
    case ObjectiveTerm::minimize_congestion_exposure: return "fbu";
    case ObjectiveTerm::minimize_total_path_cost: return "cost-fbu";
    case ObjectiveTerm::minimize_churn: return "fbu";
    case ObjectiveTerm::preserve_reservations: return "reservations";
    case ObjectiveTerm::preserve_priority: return "priority-fbu";
    case ObjectiveTerm::maximize_desired_bandwidth: return "fbu";
    case ObjectiveTerm::fairness_across_groups: return "permille";
    case ObjectiveTerm::minimize_failure_domain_concentration: return "fbu";
    case ObjectiveTerm::minimize_path_count: return "shares";
  }
  return "unit";
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// Objective metrics and scoring
// ---------------------------------------------------------------------------

struct Metrics {
  Bandwidth shortfall_below_floor = 0;
  Bandwidth shortfall_below_desired = 0;
  Bandwidth max_utilization_permille = 0;
  Bandwidth congestion_exposure = 0;
  Bandwidth weighted_path_cost = 0;
  Bandwidth moved_bandwidth = 0;
  Bandwidth displaced_reservations = 0;
  Bandwidth priority_weighted_shortfall = 0;
  Bandwidth fairness_spread_permille = 0;
  Bandwidth max_domain_concentration = 0;
  Bandwidth used_shares = 0;
};

Metrics compute_metrics(const FabricSnapshot& snapshot, const Derivation& derivation,
                        const Allocation& allocation, const SolveOptions& options) {
  Metrics metrics;

  std::map<DemandId, const DemandDerivation*> expected;
  for (const auto& demand : derivation.demands) expected.emplace(demand.demand, &demand);

  std::map<std::string, std::pair<Bandwidth, Bandwidth>> tenant_totals;

  for (const auto& entry : allocation.demands) {
    const auto it = expected.find(entry.demand);
    if (it == expected.end()) continue;
    const DemandDerivation& demand = *it->second;
    if (!demand.active) continue;

    if (entry.granted < demand.floor) {
      metrics.shortfall_below_floor =
          sat_add(metrics.shortfall_below_floor, demand.floor - entry.granted);
    }
    if (entry.granted < demand.desired) {
      const Bandwidth shortfall = demand.desired - entry.granted;
      metrics.shortfall_below_desired = sat_add(metrics.shortfall_below_desired, shortfall);
      const Bandwidth weight = static_cast<Bandwidth>(256 - demand.priority);
      metrics.priority_weighted_shortfall =
          sat_add(metrics.priority_weighted_shortfall, sat_mul(shortfall, weight));
    }
    auto& totals = tenant_totals[demand.tenant.str()];
    totals.first = sat_add(totals.first, entry.granted);
    totals.second = sat_add(totals.second, demand.desired);

    for (const auto& share : entry.shares) {
      if (share.granted <= 0) continue;
      ++metrics.used_shares;
      for (const auto& path : snapshot.paths) {
        if (!(path.id == share.path)) continue;
        metrics.weighted_path_cost =
            sat_add(metrics.weighted_path_cost, sat_mul(share.granted, path.cost));
        break;
      }
    }
  }

  if (options.has_incumbent) {
    std::map<std::pair<std::string, std::string>, Bandwidth> before;
    std::map<std::pair<std::string, std::string>, Bandwidth> after;
    for (const auto& demand : options.incumbent.demands) {
      for (const auto& share : demand.shares) {
        before[{demand.demand.str(), share.path.str()}] = share.granted;
      }
    }
    for (const auto& demand : allocation.demands) {
      for (const auto& share : demand.shares) {
        after[{demand.demand.str(), share.path.str()}] = share.granted;
      }
    }
    std::set<std::pair<std::string, std::string>> keys;
    for (const auto& entry : before) keys.insert(entry.first);
    for (const auto& entry : after) keys.insert(entry.first);
    for (const auto& key : keys) {
      const auto a = before.find(key);
      const auto b = after.find(key);
      const Bandwidth lhs = a == before.end() ? 0 : a->second;
      const Bandwidth rhs = b == after.end() ? 0 : b->second;
      metrics.moved_bandwidth =
          sat_add(metrics.moved_bandwidth, lhs > rhs ? lhs - rhs : rhs - lhs);
    }
  }

  for (const auto& accounting : derivation.reservations) {
    if (accounting.displaceable && accounting.active) ++metrics.displaced_reservations;
  }

  for (const auto& resource : allocation.resources) {
    metrics.max_utilization_permille =
        std::max(metrics.max_utilization_permille, resource.utilization_permille);
    if (resource.available > 0) {
      const Bandwidth share = permille(resource.allocated, resource.available);
      metrics.congestion_exposure =
          sat_add(metrics.congestion_exposure, sat_mul(resource.allocated, share) / 1000);
    }
  }

  std::map<std::string, Bandwidth> domain_load;
  for (const auto& demand : allocation.demands) {
    for (const auto& share : demand.shares) {
      if (share.granted <= 0) continue;
      for (const auto& path : snapshot.paths) {
        if (!(path.id == share.path)) continue;
        for (const auto& domain : path.failure_domains) {
          domain_load[domain.str()] = sat_add(domain_load[domain.str()], share.granted);
        }
        break;
      }
    }
  }
  for (const auto& entry : domain_load) {
    metrics.max_domain_concentration = std::max(metrics.max_domain_concentration, entry.second);
  }

  if (!tenant_totals.empty()) {
    Bandwidth lowest = std::numeric_limits<Bandwidth>::max();
    Bandwidth highest = 0;
    for (const auto& entry : tenant_totals) {
      const Bandwidth ratio =
          entry.second.second <= 0
              ? 1000
              : std::min<Bandwidth>(1000, permille(entry.second.first, entry.second.second));
      lowest = std::min(lowest, ratio);
      highest = std::max(highest, ratio);
    }
    if (lowest == std::numeric_limits<Bandwidth>::max()) lowest = 0;
    metrics.fairness_spread_permille = highest - lowest;
  }

  return metrics;
}

Bandwidth raw_value(ObjectiveTerm term, const Metrics& metrics) {
  switch (term) {
    case ObjectiveTerm::satisfy_minimums: return metrics.shortfall_below_floor;
    case ObjectiveTerm::minimize_max_utilization: return metrics.max_utilization_permille;
    case ObjectiveTerm::minimize_congestion_exposure: return metrics.congestion_exposure;
    case ObjectiveTerm::minimize_total_path_cost: return metrics.weighted_path_cost;
    case ObjectiveTerm::minimize_churn: return metrics.moved_bandwidth;
    case ObjectiveTerm::preserve_reservations: return metrics.displaced_reservations;
    case ObjectiveTerm::preserve_priority: return metrics.priority_weighted_shortfall;
    case ObjectiveTerm::maximize_desired_bandwidth: return metrics.shortfall_below_desired;
    case ObjectiveTerm::fairness_across_groups: return metrics.fairness_spread_permille;
    case ObjectiveTerm::minimize_failure_domain_concentration:
      return metrics.max_domain_concentration;
    case ObjectiveTerm::minimize_path_count: return metrics.used_shares;
  }
  return 0;
}

Bandwidth score_from_metrics(const ObjectiveProfile& profile, const Metrics& metrics,
                             std::vector<ObjectiveComponent>* components) {
  Bandwidth total = 0;
  if (components != nullptr) components->clear();
  for (const auto& term : profile.terms) {
    ObjectiveComponent component;
    component.term = term.term;
    component.weight = term.weight;
    component.raw = raw_value(term.term, metrics);
    component.weighted = sat_mul(term.weight, component.raw);
    component.unit = unit_of(term.term);
    total = sat_add(total, component.weighted);
    if (components != nullptr) components->push_back(std::move(component));
  }
  return total;
}

std::map<std::string, Bandwidth> reservation_floors(const Derivation& derivation) {
  std::map<std::string, Bandwidth> floors;
  for (const auto& accounting : derivation.reservations) {
    if (!accounting.active || !accounting.charged_to.valid()) continue;
    floors[accounting.charged_to.str()] =
        sat_add(floors[accounting.charged_to.str()], accounting.bandwidth);
  }
  return floors;
}

void mark_reserved_shares(DemandAllocation& entry, Bandwidth floor) {
  Bandwidth remaining = std::min(floor, entry.granted);
  for (auto& share : entry.shares) {
    if (remaining <= 0) break;
    const Bandwidth take = std::min(remaining, share.granted);
    share.reserved = take;
    remaining -= take;
  }
}

// Bounded path-count consolidation: move bandwidth from later-preferred paths
// onto earlier-preferred ones wherever headroom allows.
void consolidate_paths(Problem& problem, DemandState& state, const std::vector<PathId>& preferred) {
  if (state.shares.size() < 2) return;
  for (auto it = preferred.rbegin(); it != preferred.rend(); ++it) {
    const auto share = state.shares.find(*it);
    if (share == state.shares.end() || share->second <= 0) continue;
    const CandidatePath* from = problem.paths.at(*it);
    const Bandwidth moveable = share->second;
    Bandwidth moved = 0;
    for (const auto& target : preferred) {
      if (moved >= moveable) break;
      if (target == *it) break;
      const CandidatePath* to = problem.paths.at(target);
      const Bandwidth headroom = path_headroom(problem, *to);
      const Bandwidth take = std::min(headroom, moveable - moved);
      if (take <= 0) continue;
      grant(problem, state, *to, take);
      moved = sat_add(moved, take);
    }
    if (moved > 0) revoke(problem, state, *from, moved);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

std::int64_t score_allocation(const FabricSnapshot& snapshot, const Allocation& allocation,
                              std::vector<ObjectiveComponent>* components) {
  const Result<Derivation> derivation_result = derive(snapshot);
  if (!derivation_result.has_value()) {
    if (components != nullptr) components->clear();
    return 0;
  }
  const SolveOptions options;
  const Metrics metrics = compute_metrics(snapshot, derivation_result.value(), allocation, options);
  return score_from_metrics(snapshot.objective, metrics, components);
}

Result<SolveOutcome> solve(const FabricSnapshot& snapshot, const SolveOptions& options) {
  SolveOutcome outcome;
  if (options.max_iterations == 0) {
    return fail_as<SolveOutcome>(ErrorCode::invalid_argument,
                                 "the solver iteration budget must be positive");
  }

  const Result<Derivation> derivation_result = derive(snapshot);
  if (!derivation_result.has_value()) return Result<SolveOutcome>(derivation_result.error());
  const Derivation& derivation = derivation_result.value();
  const ObjectiveProfile& profile = snapshot.objective;

  for (const auto& term : profile.terms) {
    if (term.weight < 0) {
      outcome.status = FeasibilityStatus::unsupported_objective;
      outcome.summary = std::string("objective term ") + std::string(to_string(term.term)) +
                        " declares a negative weight, which this solver does not support";
      outcome.feasibility.status = outcome.status;
      outcome.feasibility.summary = outcome.summary;
      return outcome;
    }
  }

  const std::map<std::string, Bandwidth> floors = reservation_floors(derivation);
  const auto make_problem = [&]() {
    Problem fresh;
    fresh.snapshot = &snapshot;
    fresh.derivation = &derivation;
    for (const auto& path : snapshot.paths) fresh.paths.emplace(path.id, &path);
    for (const auto& resource : derivation.residuals.resources) {
      ResourceState state;
      state.id = resource.resource;
      state.generation = resource.generation;
      state.usable = resource.usable_capacity;
      state.committed = resource.committed_load;
      state.reserved = resource.reserved;
      state.available = resource.available;
      state.ceiling = resource.available;
      fresh.resources.emplace(resource.resource, state);
    }
    for (const auto& demand : derivation.demands) {
      DemandState state;
      state.derivation = &demand;
      const auto it = floors.find(demand.demand.str());
      if (it != floors.end()) state.reservation_floor = it->second;
      for (const auto& note : demand.notes) {
        if (note.kind == ConstraintKind::tenant_policy ||
            note.kind == ConstraintKind::service_class_policy) {
          state.exclusion = note.detail;
        }
      }
      fresh.demands.emplace(demand.demand, std::move(state));
    }
    return fresh;
  };

  Problem problem = make_problem();

  outcome.feasibility.binding = derivation.constraints;
  outcome.policy_exclusions = derivation.policy_exclusions;

  bool reservation_conflict = false;
  for (const auto& constraint : derivation.constraints) {
    if (constraint.kind == ConstraintKind::reservation_obligation) reservation_conflict = true;
  }

  const Bandwidth policy_ceiling = snapshot.policy.max_utilization_permille;
  Bandwidth ceiling = policy_ceiling;
  const bool min_max_requested = profile.has(ObjectiveTerm::minimize_max_utilization);

  if (!reservation_conflict && min_max_requested) {
    Bandwidth low = 1;
    Bandwidth high = policy_ceiling;
    Bandwidth best = 0;
    std::uint64_t probes = 0;
    while (low <= high && probes < Limits::max_max_utilization_probes) {
      ++probes;
      const Bandwidth mid = low + (high - low) / 2;
      const Certificate probe = certify(problem, mid);
      if (!probe.built) break;
      if (probe.feasible) {
        best = mid;
        if (mid == 1) break;
        high = mid - 1;
      } else {
        low = mid + 1;
      }
    }
    if (best == 0) {
      const Certificate at_cap = certify(problem, policy_ceiling);
      const Certificate without_cap = certify(problem, 1000);
      const bool policy_is_the_cause = policy_ceiling < 1000 && without_cap.built &&
                                       without_cap.feasible && at_cap.built && !at_cap.feasible;
      outcome.status = policy_is_the_cause ? FeasibilityStatus::infeasible_policy
                                           : FeasibilityStatus::infeasible_capacity;
      if (policy_is_the_cause) {
        outcome.summary =
            "the policy utilisation cap of " + std::to_string(policy_ceiling) +
            " permille makes the hard minimums unsatisfiable while they are satisfiable without the cap";
        BindingConstraint constraint;
        constraint.kind = ConstraintKind::policy_utilization_cap;
        constraint.subject = "policy/" + snapshot.policy.id.str();
        constraint.required = at_cap.required;
        constraint.available = at_cap.achievable;
        constraint.slack = at_cap.achievable - at_cap.required;
        constraint.detail = "a minimum cut proves the capped fabric cannot carry the declared minimums";
        outcome.feasibility.binding.push_back(std::move(constraint));
      } else {
        outcome.summary = "a minimum cut proves the declared hard minimums cannot be carried";
      }
      for (const auto& resource : at_cap.saturated_resources) {
        BindingConstraint constraint;
        constraint.kind = ConstraintKind::min_cut_certificate;
        constraint.subject = "resource/" + resource.str();
        constraint.resource = resource;
        constraint.detail = "this resource is on the sink side of a minimum cut";
        outcome.feasibility.binding.push_back(std::move(constraint));
      }
      outcome.feasibility.status = outcome.status;
      outcome.feasibility.summary = outcome.summary;
      canonicalize_bindings(outcome.feasibility.binding, outcome.feasibility.binding_truncated);
      return outcome;
    }
    ceiling = best;
  }
  set_ceiling(problem, ceiling);

  const std::vector<DemandId> order = canonical_processing_order(derivation);
  std::uint64_t iterations = 0;
  std::vector<std::string> repair_notes;

  // ---- Phase 1: hard minimums -------------------------------------------
  // Returns the number of active demands whose hard floor was not placed. The
  // routine is deterministic and idempotent for a freshly built Problem, which
  // is what the utilisation back-off below relies on.
  const auto run_minimums = [&](Problem& target) -> std::size_t {
    for (const auto& id : order) {
      DemandState& state = target.demands.at(id);
      if (!state.exclusion.empty()) continue;
      const std::vector<PathId> preferred = order_paths(target, state, options);
      place(target, state, preferred, state.derivation->floor);

      int rounds = 0;
      while (state.granted < state.derivation->floor && rounds < 4 &&
             iterations <= options.max_iterations) {
        ++rounds;
        const Bandwidth need = state.derivation->floor - state.granted;
        bool progressed = false;
        for (const auto& path_id : preferred) {
          const CandidatePath* path = target.paths.at(path_id);
          for (const auto& resource_id : path->resources) {
            const auto resource = target.resources.find(resource_id);
            if (resource == target.resources.end()) continue;
            const Bandwidth headroom = resource->second.ceiling - resource->second.allocated;
            if (headroom >= need) {
              progressed = true;
              break;
            }
            std::string note;
            if (repair_resource(target, id, resource_id, need - headroom, iterations,
                                options.max_iterations, note)) {
              progressed = true;
              if (!note.empty()) repair_notes.push_back(note);
            }
          }
          if (progressed) break;
        }
        if (!progressed) break;
        place(target, state, preferred, state.derivation->floor - state.granted);
      }
    }
    std::size_t unmet = 0;
    for (const auto& demand : derivation.demands) {
      if (!demand.active) continue;
      const DemandState& state = target.demands.at(demand.demand);
      if (!state.exclusion.empty()) continue;
      if (state.granted < demand.floor) ++unmet;
    }
    return unmet;
  };

  std::size_t unmet = run_minimums(problem);

  // The relaxation search above finds the tightest utilisation ceiling that
  // admits the hard minimums *in the relaxation*. A tighter ceiling can still
  // defeat the constructive placement, so the ceiling is widened by a bounded
  // deterministic search until the construction succeeds. Widening never
  // exceeds the policy cap, and never silently ignores an unmet floor.
  if (unmet > 0 && min_max_requested && ceiling < policy_ceiling) {
    Bandwidth low = ceiling + 1;
    Bandwidth high = policy_ceiling;
    Bandwidth best = 0;
    std::uint64_t probes = 0;
    while (low <= high && probes < Limits::max_max_utilization_probes) {
      ++probes;
      const Bandwidth mid = low + (high - low) / 2;
      Problem trial = make_problem();
      set_ceiling(trial, mid);
      if (run_minimums(trial) == 0) {
        best = mid;
        if (mid == low) break;
        high = mid - 1;
      } else {
        low = mid + 1;
      }
    }
    if (best == 0) best = policy_ceiling;
    problem = make_problem();
    set_ceiling(problem, best);
    unmet = run_minimums(problem);
    if (unmet == 0 && best != ceiling) {
      repair_notes.push_back(
          "minimise_max_utilization widened the utilisation ceiling from " +
          std::to_string(ceiling) + " to " + std::to_string(best) +
          " permille so that the hard minimums could be constructed");
    }
    ceiling = best;
  }

  // ---- Phase 2: soft targets --------------------------------------------
  const bool fairness = profile.has(ObjectiveTerm::fairness_across_groups);
  const auto fill_one = [&](const DemandId& id) {
    DemandState& state = problem.demands.at(id);
    if (!state.exclusion.empty()) return;
    const Bandwidth target = std::min(state.derivation->desired, state.derivation->maximum);
    if (state.granted >= target) return;
    const std::vector<PathId> preferred = order_paths(problem, state, options);
    place(problem, state, preferred, target - state.granted);
  };

  if (!fairness) {
    for (const auto& id : order) fill_one(id);
  } else {
    std::map<std::string, std::vector<DemandId>> by_tenant;
    for (const auto& id : order) {
      by_tenant[problem.demands.at(id).derivation->tenant.str()].push_back(id);
    }
    std::size_t rounds = 0;
    for (const auto& entry : by_tenant) rounds = std::max(rounds, entry.second.size());
    for (std::size_t round = 0; round < rounds; ++round) {
      for (auto& entry : by_tenant) {
        if (round < entry.second.size()) fill_one(entry.second[round]);
      }
    }
  }

  // ---- Phase 3: path-count consolidation --------------------------------
  if (profile.has(ObjectiveTerm::minimize_path_count)) {
    for (const auto& id : order) {
      DemandState& state = problem.demands.at(id);
      if (!state.exclusion.empty()) continue;
      consolidate_paths(problem, state, order_paths(problem, state, options));
    }
  }

  // ---- Build the allocation ---------------------------------------------
  Allocation allocation;
  for (const auto& demand : derivation.demands) {
    DemandAllocation entry;
    entry.demand = demand.demand;
    entry.generation = demand.generation;
    entry.minimum = demand.minimum;
    entry.desired = demand.desired;
    entry.maximum = demand.maximum;
    const DemandState& state = problem.demands.at(demand.demand);
    for (const auto& share : state.shares) {
      if (share.second <= 0) continue;
      const CandidatePath* path = problem.paths.at(share.first);
      PathShare path_share;
      path_share.path = share.first;
      path_share.generation = path->generation;
      path_share.granted = share.second;
      if (options.has_incumbent) {
        const DemandAllocation* incumbent = options.incumbent.find_demand(demand.demand);
        Bandwidth before = 0;
        if (incumbent != nullptr) {
          for (const auto& previous : incumbent->shares) {
            if (previous.path == share.first) {
              before = previous.granted;
              break;
            }
          }
        }
        path_share.delta_from_incumbent = share.second - before;
        path_share.newly_used = before == 0;
      }
      entry.shares.push_back(std::move(path_share));
    }
    entry.granted = state.granted;
    entry.reserved = std::min(state.reservation_floor, state.granted);
    entry.effective = state.granted;
    entry.shortfall_against_minimum =
        demand.active && state.granted < demand.floor ? demand.floor - state.granted : 0;
    entry.shortfall_against_desired =
        demand.active && state.granted < demand.desired ? demand.desired - state.granted : 0;
    mark_reserved_shares(entry, entry.reserved);
    allocation.total_granted = sat_add(allocation.total_granted, entry.granted);
    allocation.total_reserved = sat_add(allocation.total_reserved, entry.reserved);
    allocation.demands.push_back(std::move(entry));
  }
  std::sort(allocation.demands.begin(), allocation.demands.end(),
            [](const DemandAllocation& a, const DemandAllocation& b) { return a.demand < b.demand; });

  for (const auto& resource : derivation.residuals.resources) {
    ResourceUtilization utilization;
    utilization.resource = resource.resource;
    utilization.generation = resource.generation;
    utilization.usable_capacity = resource.usable_capacity;
    utilization.committed_load = resource.committed_load;
    utilization.reserved = resource.reserved;
    utilization.available = resource.available;
    utilization.allocated = problem.resources.at(resource.resource).allocated;
    utilization.headroom = utilization.available - utilization.allocated;
    const auto claimed = add_checked(utilization.committed_load, utilization.reserved);
    const Bandwidth in_use =
        sat_add(claimed ? *claimed : utilization.usable_capacity, utilization.allocated);
    utilization.utilization_permille = permille(in_use, utilization.usable_capacity);
    utilization.saturated = utilization.available <= 0 || utilization.headroom <= 0;
    allocation.resources.push_back(std::move(utilization));
  }

  // ---- Alternatives and notes -------------------------------------------
  std::size_t alternatives_recorded = 0;
  for (const auto& id : order) {
    DemandState& state = problem.demands.at(id);
    if (!state.exclusion.empty()) continue;
    if (alternatives_recorded >= Limits::max_alternatives) break;
    const std::vector<PathId> preferred = order_paths(problem, state, options);
    for (const auto& path_id : preferred) {
      if (alternatives_recorded >= Limits::max_alternatives) break;
      const CandidatePath* path = problem.paths.at(path_id);
      const auto share = state.shares.find(path_id);
      if (share != state.shares.end() && share->second > 0) continue;
      RejectedAlternative alternative;
      alternative.subject = "demand/" + id.str() + "/path/" + path_id.str();
      const Bandwidth headroom = path_headroom(problem, *path);
      if (headroom <= 0) {
        alternative.reason = "no residual capacity under the active utilisation ceiling";
      } else if (state.granted >= std::min(state.derivation->desired, state.derivation->maximum)) {
        alternative.reason = "demand already satisfied to its target on preferred paths";
      } else {
        alternative.reason = "not required: preferred paths absorbed the whole grant";
      }
      alternative.delta = headroom;
      alternative.alternative_score = path->cost;
      state.alternatives.push_back(std::move(alternative));
      ++alternatives_recorded;
    }
  }

  // ---- Status ------------------------------------------------------------
  bool floors_met = true;
  bool policy_blocked = false;
  for (const auto& demand : derivation.demands) {
    if (!demand.active) continue;
    const DemandState& state = problem.demands.at(demand.demand);
    if (!state.exclusion.empty()) {
      if (demand.floor > 0) {
        policy_blocked = true;
        floors_met = false;
      }
      continue;
    }
    if (state.granted < demand.floor) floors_met = false;
  }

  if (reservation_conflict) {
    outcome.status = FeasibilityStatus::infeasible_reservation_conflict;
    outcome.summary =
        "non-displaceable reservation obligations exceed the authoritative usable capacity of at "
        "least one resource";
  } else if (floors_met) {
    outcome.status = FeasibilityStatus::feasible;
    outcome.summary = "every active demand was granted at least its hard floor";
  } else if (policy_blocked) {
    outcome.status = FeasibilityStatus::infeasible_policy;
    outcome.summary = "the active policy excludes at least one demand that declares a hard floor";
    allocation.degraded = true;
  } else {
    const Certificate certificate = certify(problem, policy_ceiling);
    if (certificate.built && !certificate.feasible) {
      BindingConstraint constraint;
      constraint.kind = ConstraintKind::min_cut_certificate;
      constraint.subject = "fabric";
      constraint.required = certificate.required;
      constraint.available = certificate.achievable;
      constraint.slack = certificate.achievable - certificate.required;
      constraint.detail =
          "a minimum cut over the relaxed eligibility network proves the declared hard minimums "
          "cannot all be carried";
      outcome.feasibility.binding.push_back(std::move(constraint));
      for (const auto& resource : certificate.saturated_resources) {
        BindingConstraint cut;
        cut.kind = ConstraintKind::min_cut_certificate;
        cut.subject = "resource/" + resource.str();
        cut.resource = resource;
        cut.detail = "resource on the sink side of a minimum cut";
        outcome.feasibility.binding.push_back(std::move(cut));
      }
      // The policy utilisation cap is itself a hard constraint. When the
      // minimums fail only because of the cap, the outcome is a policy
      // infeasibility, not a capacity one; when they would fail anyway, the
      // capacity classification is the honest one.
      const Certificate uncapped = certify(problem, 1000);
      const bool policy_is_the_cause =
          policy_ceiling < 1000 && uncapped.built && uncapped.feasible;
      if (certificate.only_path_starved) {
        outcome.status = FeasibilityStatus::infeasible_path_set;
        outcome.summary =
            "at least one demand has no eligible candidate path under the current policy";
      } else if (policy_is_the_cause) {
        outcome.status = FeasibilityStatus::infeasible_policy;
        outcome.summary =
            "the policy utilisation cap of " + std::to_string(policy_ceiling) +
            " permille makes the hard minimums unsatisfiable while they are satisfiable without the cap";
        BindingConstraint cap_constraint;
        cap_constraint.kind = ConstraintKind::policy_utilization_cap;
        cap_constraint.subject = "policy/" + snapshot.policy.id.str();
        cap_constraint.required = certificate.required;
        cap_constraint.available = certificate.achievable;
        cap_constraint.slack = certificate.achievable - certificate.required;
        cap_constraint.detail =
            "a minimum cut proves the capped fabric cannot carry the declared minimums";
        outcome.feasibility.binding.push_back(std::move(cap_constraint));
      } else {
        outcome.status = FeasibilityStatus::infeasible_capacity;
        outcome.summary =
            "the hard minimums cannot all be carried; a minimum cut certifies the shortfall";
      }
      for (const auto& demand : derivation.demands) {
        if (!demand.active) continue;
        const DemandState& state = problem.demands.at(demand.demand);
        if (state.granted >= demand.floor) continue;
        BindingConstraint unmet_constraint;
        unmet_constraint.kind = ConstraintKind::demand_minimum;
        unmet_constraint.subject = "demand/" + demand.demand.str();
        unmet_constraint.demand = demand.demand;
        unmet_constraint.required = demand.floor;
        unmet_constraint.available = state.granted;
        unmet_constraint.slack = state.granted - demand.floor;
        unmet_constraint.detail = "the demand hard floor could not be placed";
        outcome.feasibility.binding.push_back(std::move(unmet_constraint));
      }
      allocation.degraded = true;
    } else if (options.allow_degraded && !snapshot.policy.require_minimums) {
      outcome.status = FeasibilityStatus::feasible_degraded;
      outcome.summary =
          "the request is degraded: at least one hard floor could not be placed and policy permits "
          "a degraded result";
      allocation.degraded = true;
      for (const auto& demand : derivation.demands) {
        if (!demand.active) continue;
        const DemandState& state = problem.demands.at(demand.demand);
        if (state.granted >= demand.floor) continue;
        BindingConstraint unmet_constraint;
        unmet_constraint.kind = ConstraintKind::demand_minimum;
        unmet_constraint.subject = "demand/" + demand.demand.str();
        unmet_constraint.demand = demand.demand;
        unmet_constraint.required = demand.floor;
        unmet_constraint.available = state.granted;
        unmet_constraint.slack = state.granted - demand.floor;
        unmet_constraint.detail = "the demand hard floor could not be placed within the bounded search";
        outcome.feasibility.binding.push_back(std::move(unmet_constraint));
      }
    } else {
      outcome.status = FeasibilityStatus::solver_limit_reached;
      outcome.summary =
          "the bounded deterministic search could not place every hard floor and no minimum cut "
          "certificate proves the request infeasible; the request is neither accepted nor proven "
          "infeasible";
      allocation.degraded = true;
      BindingConstraint constraint;
      constraint.kind = ConstraintKind::structural;
      constraint.subject = "solver";
      constraint.detail = outcome.summary;
      outcome.feasibility.binding.push_back(std::move(constraint));
      // Even when no certificate proves the request infeasible, the explanation
      // must still say exactly which demands are short and by how much.
      for (const auto& demand : derivation.demands) {
        if (!demand.active) continue;
        const DemandState& state = problem.demands.at(demand.demand);
        if (state.granted >= demand.floor) continue;
        BindingConstraint unmet_constraint;
        unmet_constraint.kind = ConstraintKind::demand_minimum;
        unmet_constraint.subject = "demand/" + demand.demand.str();
        unmet_constraint.demand = demand.demand;
        unmet_constraint.required = demand.floor;
        unmet_constraint.available = state.granted;
        unmet_constraint.slack = state.granted - demand.floor;
        unmet_constraint.detail =
            "the demand hard floor could not be placed within the bounded search";
        outcome.feasibility.binding.push_back(std::move(unmet_constraint));
      }
    }
  }

  outcome.allocation = std::move(allocation);
  outcome.iterations = iterations;
  for (const auto& note : repair_notes) outcome.notes.push_back(note);
  std::sort(outcome.notes.begin(), outcome.notes.end());
  outcome.notes.erase(std::unique(outcome.notes.begin(), outcome.notes.end()), outcome.notes.end());
  if (min_max_requested && ceiling != policy_ceiling) {
    outcome.notes.push_back("minimise_max_utilization tightened the utilisation ceiling from " +
                            std::to_string(policy_ceiling) + " to " + std::to_string(ceiling) +
                            " permille");
  }

  const Metrics metrics = compute_metrics(snapshot, derivation, outcome.allocation, options);
  std::vector<ObjectiveComponent> components;
  outcome.score = score_from_metrics(profile, metrics, &components);
  outcome.components = std::move(components);

  for (const auto& id : order) {
    DemandState& state = problem.demands.at(id);
    for (auto& alternative : state.alternatives) outcome.alternatives.push_back(std::move(alternative));
  }
  std::sort(outcome.alternatives.begin(), outcome.alternatives.end());
  if (outcome.alternatives.size() > Limits::max_alternatives) {
    outcome.alternatives.resize(Limits::max_alternatives);
  }

  outcome.allocation_digest = outcome.allocation.digest();

  const std::vector<BindingConstraint> violations = verify_allocation(snapshot, outcome.allocation);
  if (!violations.empty()) {
    outcome.verified = false;
    outcome.status = FeasibilityStatus::conflicting_input;
    outcome.summary =
        "the constructed allocation failed independent verification and was not offered as a plan";
    for (const auto& violation : violations) outcome.feasibility.binding.push_back(violation);
  } else {
    outcome.verified = true;
  }

  outcome.feasibility.status = outcome.status;
  outcome.feasibility.summary = outcome.summary;
  canonicalize_bindings(outcome.feasibility.binding, outcome.feasibility.binding_truncated);
  return outcome;
}

}  // namespace tef

