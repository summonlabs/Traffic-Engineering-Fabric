# Validation: what is REAL, what is SYNTHETIC, what is UNSUPPORTED

This page is the honest inventory of what the shipped test and benchmark suites actually exercise.
Nothing in this repository claims physical-network, multi-switch, multi-rack, multi-site, RDMA,
optical, NIC, DPU or hardware-forwarding validation, because none of it was performed.

## REAL

These are genuine operating-system, filesystem and network behaviours.

| Claim | How it is proven |
| --- | --- |
| real TCP transport | `tef_distributed_tests` and `tef_multiprocess_tests` connect over `127.0.0.1` TCP sockets bound to an ephemeral port; `TcpListener`/`TcpSocket` are the same classes the shipped coordinator uses |
| real independent OS processes | `tef_multiprocess_tests` launches the shipped `tef-node` binary with @CreateProcess` (POSIX: `posix_spawn`), one process per coordinator or publisher, and drives the protocol across process boundaries |
| real process kill | the supervisor calls @TerminateProcess` / `SIGKILL` on a live coordinator that is accepting connections, and on a publisher that is blocked mid-protocol |
| real coordinator restart with epoch advancement | the killed coordinator is restarted against the same durable directory; the new epoch is strictly greater and the boot identity differs |
| stale epoch / fenced boot replay rejection | a forged frame carrying the superseded epoch, and a fresh process using a fenced boot identity, are both refused by the real coordinator and asserted on the returned typed codes |
| real close/reopen of durable state | `tef_persistence_tests` closes a `PlanRepository`, reopens it, and compares every record, sequence number and digest |
| torn and corrupted durable state | the tests write partial trailing bytes, flip payload bytes, corrupt a snapshot payload and replace a journal with garbage, then assert exactly what recovery does |
| real filesystem semantics | atomic replace through @MoveFileEx` / `rename`, temp-file discard on open, and file-size assertions after truncation |
| real concurrency | `tef_concurrency_tests` uses real threads with @std::latch`/`std::barrier` release, real sockets, and real joins - no simulated scheduler |
| installable package | `cmake --install` produces a package, and an independent @find_package` consumer is configured, built and run against it |

## SYNTHETIC

These use clearly-labelled synthetic populations. They validate *algorithmic* behaviour and
*determinism*; they say nothing about any physical network.

| Claim | How it is exercised |
| --- | --- |
| multi-demand global allocation | synthetic fabrics with interacting demands sharing resources |
| hard constraints versus soft objectives | synthetic infeasible and degraded populations |
| deterministic tie-breaking | repeated solves and permuted inputs over deterministic seeds |
| capacity accounting closes | exact integer accounting assertions over seeded random fabrics |
| reservation obligations respected | synthetic reservations at every documented disposition |
| authorised paths only | assertions that every used path is in the authoritative candidate set |
| generation-bound stale rejection | synthetic generation advancement at every bound field |
| incumbent/churn comparison | synthetic incumbent allocations with measured moved bandwidth |
| property/invariant coverage | 64 deterministic seeds per invariant, four population tiers |
| planning throughput | `benchmarks/tef_bench.cpp` measures *completed planning work* on synthetic populations only |

Population generator: a documented splitmix64 PRNG, so any seed reproduces a fabric exactly on any
platform. The random fabrics deliberately include pathological inputs (zero-capacity resources,
already-over-committed resources, disconnected candidate sets, enormous declared bandwidths).

## UNSUPPORTED

These are explicitly not implemented and are never claimed:

* discovering topology, link state or path legality;
* computing candidate paths;
* installing routes, mutating weights, or programming any device;
* ECMP membership decisions;
* reservation lifecycle, admission or flow scheduling;
* reading or applying actual forwarding state (the `observed_applied` fields exist as input slots
  for an external observer and are never populated by this runtime);
* GPU, accelerator, RDMA, NVLink, optical, switch ASIC or NIC behaviour of any kind;
* external/third-party optimisation solvers;
* any form of hardware-in-the-loop validation.

## Sanitizers and static analysis

* AddressSanitizer is used where the toolchain provides the runtime; when it is unavailable the fact
  is stated in the release report rather than claimed.
* MSVC `/analyze` is run over the first-party sources; first-party findings are fixed.
* Release and Debug are both built with `/W4 /WX` (MSVC) or
  `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror` (GCC/Clang).

## Test philosophy

Tests here are proof obligations, not a count target. There are no timeouts, no watchdogs, no retries
and no sleeps used as synchronisation. A hanging test is a defect to diagnose; CTest is configured
with `TIMEOUT 0` so that nothing terminates a test on its behalf.
