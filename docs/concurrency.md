# Concurrency, locking and shutdown

This document is the written record of the mandatory lock-reentrancy and deadlock audit. It states
the ownership rules, the global lock order, and how each hazard on the audit list is avoided.

## Lock inventory

| Lock | Owner | Protects |
| --- | --- | --- |
| `Engine::Impl::mutex` (std::shared_mutex) | Engine | the live snapshot, the live authority, the plan map, the explanation map, recovery state, statistics |
| `PlanRepository::Impl::mutex` (std::mutex) | PlanRepository | the journal file handle, the record log, the fence set, the coordinator epoch, counters |
| `Coordinator::Impl::registry_mutex` (std::mutex) | Coordinator | the registered-boot map, the fence set, the live session list |
| `SocketState::mutex` (std::mutex) | TcpSocket / TcpListener | the socket handle and its interrupted flag |
| `g_net_mutex` (std::mutex) | process | the Winsock/BSD-socket reference count and the last transport error |

There are no other locks. Nothing in the library allocates a lock dynamically or uses a lock-free
structure for authoritative state.

## Global lock order

```
Engine::Impl::mutex                 ->  PlanRepository::Impl::mutex
Coordinator::Impl::registry_mutex   ->  SocketState::mutex
```

The two chains are disjoint: no path acquires `Engine::Impl::mutex` while holding
`registry_mutex`, and no path acquires `registry_mutex` while holding `Engine::Impl::mutex`.
`PlanRepository` never calls back into a caller or into any other component, so its lock is always
a leaf. `SocketState::mutex` is held only for the duration of a handle read or a `shutdown` call.

A single writer path (`PlanRepository::append`) holds `PlanRepository::Impl::mutex` across a small
durable write. That is deliberate: acknowledging a durable mutation before the record is on stable
storage would break the crash-safety contract.

## Reader/writer discipline

* Read-only `Engine` methods (`get`, `list`, `incumbent`, `explain`, `live_authority`,
  `snapshot_copy`, `stats`, `recovery_report`) take `std::shared_lock` and never acquire the
  write lock. There is therefore no read-then-write upgrade path on `Engine::Impl::mutex`.
* Mutating `Engine` methods take `std::unique_lock` once, at the top, and call only `*_locked`
  helpers that assume the lock is already held. No public method is ever called from a `*_locked`
  helper, so the mutex is never re-entered.
* `PlanRepository` accessors take `std::unique_lock` because they read mutable members (the record
  log, counters, the fence set) that are guarded by the same mutex.

## Audit against the required hazard list

| Hazard | How it is avoided |
| --- | --- |
| read lock followed by write acquisition on the same lock before dropping the guard | read paths never escalate; every mutating path starts with a fresh `unique_lock` |
| write lock held while calling code that reads or writes the same lock | all helpers are `*_locked` and assume the caller holds it; none re-acquires |
| mutex re-entry through callbacks | the library has no callback registry; no caller-supplied function is ever invoked |
| event emission while internal locks are held | no events, no observers, no listeners |
| worker shutdown while holding locks workers need | `Coordinator::stop` sets the stopping flag and interrupts sockets while holding no lock, joins the acceptor, then joins workers |
| joining a thread while holding state required by that thread | threads are joined with no lock held: the session list is copied under `registry_mutex`, the lock is released, then sockets are interrupted and threads joined |
| cancellation paths with reversed lock ordering | the only cancellation is socket interruption, which takes `SocketState::mutex` and nothing else |
| progress callbacks that re-enter mutable state | there is no progress callback surface |
| shutdown paths that wait on work while preventing that work from completing | the coordinator accept loop polls with a short interval and observes the stopping flag; handlers blocked in a read are woken by `shutdown()` on their socket |
| callbacks invoked beneath internal state locks | not applicable; there are no callbacks |
| nested resource acquisition with inconsistent global ordering | the two lock chains are disjoint and each is strictly ordered |

One case deserves a note because it is a genuine improvement over the naive design: the coordinator
decides whether a publisher registration must fence a replaced boot identity **under**
`registry_mutex`, but persists the fence record **without** holding it. Fencing therefore never
performs I/O beneath a lock that a connection handler may need, while still keeping the decision
atomic with respect to the registry.

## Socket interruption

`TcpSocket::interrupt()` and `TcpSocket::close()` may be called from a thread other than the one
blocked in `read_frame()`. Both take `SocketState::mutex`; `interrupt()` marks the state
interrupted and calls `shutdown()`, which unblocks a pending `recv` immediately without
invalidating the handle; `close()` additionally performs the actual `closesocket` / `close` and
sets the handle to invalid. The read loop waits with `select()` at `poll_interval_ms` granularity,
re-checks the interrupted flag on every iteration, and treats an interrupted or closed socket as a
terminal read failure. A live but slow peer is waited for indefinitely: there is no total-read
timeout anywhere in the transport.

## Shutdown sequence

`Coordinator::stop()`:

1. set the stopping flag (no lock held);
2. `TcpListener::interrupt()` to wake the acceptor;
3. join the acceptor thread;
4. close the listener;
5. copy the live session list under `registry_mutex`, release the lock, then `interrupt()` every
   session socket;
6. join every worker thread;
7. clear the registry;
8. close the durable repository.

`stop()` is idempotent; a second call is a no-op. Work that had not crossed its authoritative
completion boundary is never committed, and no commit is acknowledged before its durable record is
written.

## Cancellation

There is no thread pool and no work queue, so there is no queued-work cancellation to get wrong. The
cancellation surfaces are:

* **Socket interruption**, described above: interrupted reads report failure and the connection is
  closed; nothing is published afterwards.
* **Solver budget**, `SolveOptions::max_iterations`: exceeding it produces an explicit
  `SOLVER_LIMIT_REACHED` outcome. A partially constructed allocation is never published as a plan.
* **Plan state**, the lifecycle state machine: a plan that moved to `STALE`, `REJECTED` or
  `RETIRED` cannot be authorised or committed, so cancelled work cannot later mutate authoritative
  state.

## Thread ownership

The library creates threads in exactly one place: the coordinator accept loop and its per-session
handlers. A session handler thread is created by the accept loop and joined by `stop()`. No thread
is detached. The engine and repository own no threads at all, which is why they can be used from any
thread a caller chooses.
