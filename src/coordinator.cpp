// Traffic Engineering Fabric - distributed coordinator.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Lock ordering (audited, see docs/concurrency.md):
//   Impl::registry_mutex -> SocketState::mutex
//   Engine::Impl::mutex  -> PlanRepository::Impl::mutex
//   Impl::registry_mutex and Engine::Impl::mutex are never held at the same time.
// No lock is held while a worker thread is created or joined: stop() sets the
// stopping flag, interrupts the listener and every live connection, joins the
// acceptor, and only then joins the workers.
#include "tef/coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "tef/numeric.hpp"
#include "tef/version.hpp"

namespace tef {
namespace {

constexpr std::size_t kMaxExplanationBytes = 1024 * 1024;

struct Session {
  TcpSocket socket;
  PublisherId publisher;
  BootId boot;
  bool registered = false;
  bool hello_done = false;
};

}  // namespace

struct Coordinator::Impl {
  CoordinatorConfig config;
  std::unique_ptr<Engine> engine;
  std::shared_ptr<PlanRepository> repository;
  TcpListener listener;

  CoordinatorId coordinator_id;
  BootId boot;
  CoordinatorIncarnation incarnation;
  std::atomic<std::uint64_t> epoch{0};
  std::atomic<bool> stopping{false};
  std::atomic<bool> started{false};

  std::thread acceptor;
  mutable std::mutex registry_mutex;
  std::vector<std::thread> workers;
  std::vector<std::shared_ptr<Session>> sessions;
  std::map<PublisherId, BootId> registered;
  std::vector<FenceRecord> fences;
  std::atomic<std::uint64_t> active_connections{0};
  std::atomic<std::uint64_t> peak_connections{0};

  std::atomic<std::uint64_t> connections_accepted{0};
  std::atomic<std::uint64_t> connections_rejected{0};
  std::atomic<std::uint64_t> frames_read{0};
  std::atomic<std::uint64_t> frames_written{0};
  std::atomic<std::uint64_t> frames_rejected{0};
  std::atomic<std::uint64_t> mutations_accepted{0};
  std::atomic<std::uint64_t> mutations_rejected{0};
  std::atomic<std::uint64_t> stale_epoch_rejections{0};
  std::atomic<std::uint64_t> fenced_rejections{0};
  std::atomic<std::uint64_t> duplicate_commit_replays{0};

  bool is_fenced(const PublisherId& publisher, const BootId& candidate) const {
    std::lock_guard<std::mutex> guard(registry_mutex);
    for (const auto& fence : fences) {
      if (fence.publisher == publisher && fence.boot == candidate) return true;
    }
    return false;
  }

  void add_fence(const FenceRecord& fence) {
    std::lock_guard<std::mutex> guard(registry_mutex);
    fences.push_back(fence);
  }

  Status validate_binding(const MutationBinding& binding, std::string& detail) {
    if (stopping.load()) {
      detail = "the coordinator is stopping";
      return Status(Error(ErrorCode::shutdown, detail));
    }
    if (!binding.valid()) {
      detail = "the mutation binding is incomplete";
      return Status(Error(ErrorCode::invalid_argument, detail));
    }
    if (binding.coordinator_incarnation != incarnation ||
        binding.coordinator_epoch != epoch.load()) {
      ++stale_epoch_rejections;
      detail = "the mutation was issued under a superseded coordinator incarnation or epoch";
      return Status(Error(ErrorCode::epoch_mismatch, detail));
    }
    if (is_fenced(binding.publisher, binding.publisher_boot)) {
      ++fenced_rejections;
      detail = "the publisher boot identity is fenced";
      return Status(Error(ErrorCode::fenced, detail));
    }
    {
      std::lock_guard<std::mutex> guard(registry_mutex);
      const auto it = registered.find(binding.publisher);
      if (it == registered.end() || !(it->second == binding.publisher_boot)) {
        detail = "the publisher boot identity is not the registered incarnation";
        return Status(Error(ErrorCode::unauthorized, detail));
      }
    }
    return Status::success();
  }

  void register_session(const std::shared_ptr<Session>& session) {
    std::lock_guard<std::mutex> guard(registry_mutex);
    sessions.push_back(session);
  }

  void unregister_session(const std::shared_ptr<Session>& session) {
    std::lock_guard<std::mutex> guard(registry_mutex);
    sessions.erase(std::remove(sessions.begin(), sessions.end(), session), sessions.end());
  }

  void handle_connection(const std::shared_ptr<Session>& session);
  bool dispatch(const std::shared_ptr<Session>& session, const Frame& request, Frame& response);
  // The request handlers are separate functions so that no single function
  // carries the stack frame of every message shape at once.
  void handle_plan_request(const Frame& request, Frame& response);
  void handle_commit_request(const Frame& request, Frame& response);
  void handle_query_request(const Frame& request, Frame& response);
  void accept_loop();
};

namespace {

Frame make_frame(MessageType type, std::vector<std::byte> payload) {
  Frame frame;
  frame.version = static_cast<std::uint16_t>(kWireProtocolVersion);
  frame.type = static_cast<std::uint16_t>(type);
  frame.flags = 0;
  frame.payload = std::move(payload);
  return frame;
}

// Explanations are bounded before they reach the wire. When the bound is
// exceeded the payload is replaced by a small, valid, explicitly truncated
// document rather than a partial one.
std::string bounded(std::string text) {
  if (text.size() <= kMaxExplanationBytes) return text;
  return R"({"truncated":true,"reason":"the explanation exceeded the transport bound"})";
}

}  // namespace

Coordinator::Coordinator() : impl_(std::make_unique<Impl>()) {}
Coordinator::~Coordinator() {
  if (impl_ && impl_->started.load()) {
    (void)stop();
  }
}

Result<std::unique_ptr<Coordinator>> Coordinator::create(const CoordinatorConfig& config) {
  auto coordinator = std::unique_ptr<Coordinator>(new Coordinator());
  Coordinator::Impl& impl = *coordinator->impl_;
  impl.config = config;
  coordinator->impl_->engine = std::make_unique<Engine>();
  return coordinator;
}

Status Coordinator::start() {
  Impl& impl = *impl_;
  if (impl.started.load()) {
    return fail(ErrorCode::invalid_argument, "the coordinator is already started");
  }
  impl.coordinator_id = CoordinatorId::parse("tef-coordinator")
                            .value_or(CoordinatorId{});
  impl.incarnation = CoordinatorIncarnation::first();
  impl.boot = random_boot_id();

  if (!impl.config.data_directory.empty()) {
    PlanRepository::Options options;
    options.directory = impl.config.data_directory;
    options.max_records = impl.config.max_records;
    options.fsync = impl.config.fsync;
    Result<std::unique_ptr<PlanRepository>> opened = PlanRepository::open(options);
    if (!opened.has_value()) {
      return Status(opened.error());
    }
    auto repository = std::shared_ptr<PlanRepository>(std::move(opened.value()));
    const std::uint64_t previous = repository->coordinator_epoch();
    if (previous == UINT64_MAX) {
      return fail(ErrorCode::invalid_generation, "the persisted coordinator epoch is exhausted");
    }
    CoordinatorEpochRecord record;
    record.coordinator = impl.coordinator_id;
    record.incarnation = impl.incarnation;
    record.boot = impl.boot;
    record.epoch = previous + 1;
    const Status persisted = repository->set_coordinator_epoch(record.epoch, record);
    if (!persisted.ok()) return persisted;
    impl.epoch.store(record.epoch);
    impl.fences = repository->fences();
    impl.engine->attach_repository(repository);
    const Status recovered = impl.engine->recover_from_repository();
    if (!recovered.ok()) return recovered;
    impl.repository = std::move(repository);
  } else {
    impl.epoch.store(1);
  }

  const Status bound = impl.listener.listen(impl.config.bind_address, impl.config.port);
  if (!bound.ok()) return bound;
  impl.listener.set_poll_interval_ms(25);
  impl.started.store(true);
  impl.stopping.store(false);
  impl.acceptor = std::thread([&impl]() { impl.accept_loop(); });
  return Status::success();
}

void Coordinator::Impl::accept_loop() {
  while (!stopping.load()) {
    auto session = std::make_shared<Session>();
    std::string error;
    if (!listener.accept(session->socket, error)) {
      if (stopping.load()) break;
      if (error.empty()) continue;
      ++frames_rejected;
      ++connections_rejected;
      continue;
    }
    if (stopping.load()) {
      session->socket.close();
      break;
    }
    if (active_connections.load() >= config.max_worker_threads ||
        active_connections.load() >= config.max_connections) {
      ++connections_rejected;
      session->socket.close();
      continue;
    }
    ++connections_accepted;
    const std::uint64_t current = active_connections.fetch_add(1) + 1;
    std::uint64_t peak = peak_connections.load();
    while (current > peak && !peak_connections.compare_exchange_weak(peak, current)) {
    }
    register_session(session);
    workers.emplace_back([this, session]() {
      handle_connection(session);
      unregister_session(session);
      active_connections.fetch_sub(1);
    });
  }
}

bool Coordinator::Impl::dispatch(const std::shared_ptr<Session>& session, const Frame& request,
                                 Frame& response) {
  const auto type = static_cast<MessageType>(request.type);
  switch (type) {
    case MessageType::hello_request: {
      const Result<HelloRequest> decoded = decode_hello_request(
          std::span<const std::byte>(request.payload.data(), request.payload.size()));
      if (!decoded.has_value()) {
        response = make_frame(MessageType::error_response,
                              encode(ErrorResponse{decoded.error().code(), decoded.error().detail()}));
        return true;
      }
      if (decoded.value().protocol_version != static_cast<std::uint16_t>(kWireProtocolVersion)) {
        response = make_frame(MessageType::error_response,
                              encode(ErrorResponse{ErrorCode::unsupported_version,
                                                   "the requested protocol version is not supported"}));
        return true;
      }
      HelloResponse hello;
      hello.protocol_version = static_cast<std::uint16_t>(kWireProtocolVersion);
      hello.coordinator_epoch = epoch.load();
      hello.coordinator_incarnation = incarnation;
      hello.coordinator_boot = boot;
      hello.accepting = !stopping.load();
      session->hello_done = true;
      response = make_frame(MessageType::hello_response, encode(hello));
      return true;
    }
    case MessageType::register_publisher_request: {
      const Result<RegisterPublisherRequest> decoded = decode_register_publisher_request(
          std::span<const std::byte>(request.payload.data(), request.payload.size()));
      if (!decoded.has_value()) {
        response = make_frame(MessageType::error_response,
                              encode(ErrorResponse{decoded.error().code(), decoded.error().detail()}));
        return true;
      }
      const RegisterPublisherRequest& request_message = decoded.value();
      RegisterPublisherResponse reply;
      reply.coordinator_epoch = epoch.load();
      reply.coordinator_incarnation = incarnation;
      if (request_message.coordinator_epoch != epoch.load() ||
          request_message.coordinator_incarnation != incarnation) {
        ++stale_epoch_rejections;
        reply.accepted = false;
        reply.fenced = false;
        reply.detail = "the registration was issued under a superseded coordinator epoch";
        response = make_frame(MessageType::register_publisher_response, encode(reply));
        return true;
      }
      if (is_fenced(request_message.publisher, request_message.boot)) {
        ++fenced_rejections;
        reply.accepted = false;
        reply.fenced = true;
        reply.detail = "the publisher boot identity is fenced";
        response = make_frame(MessageType::register_publisher_response, encode(reply));
        return true;
      }
      // A newer boot identity for the same publisher permanently fences the
      // replaced incarnation. Fencing is decided under the registry lock but is
      // persisted without holding it, so no I/O ever runs beneath a lock that a
      // connection handler may need.
      bool replaced = false;
      BootId replaced_boot;
      {
        std::lock_guard<std::mutex> guard(registry_mutex);
        const auto it = registered.find(request_message.publisher);
        if (it != registered.end() && !(it->second == request_message.boot)) {
          replaced = true;
          replaced_boot = it->second;
        }
      }
      if (replaced) {
        FenceRecord fence;
        fence.publisher = request_message.publisher;
        fence.boot = replaced_boot;
        fence.incarnation = incarnation;
        fence.reason = "replaced by a newer publisher boot incarnation";
        fence.tick = epoch.load();
        if (repository) {
          const Status persisted = repository->add_fence(fence);
          if (!persisted.ok()) {
            reply.accepted = false;
            reply.detail = persisted.error().format();
            response = make_frame(MessageType::register_publisher_response, encode(reply));
            return true;
          }
        }
        add_fence(fence);
      }
      {
        std::lock_guard<std::mutex> guard(registry_mutex);
        registered[request_message.publisher] = request_message.boot;
      }
      session->publisher = request_message.publisher;
      session->boot = request_message.boot;
      session->registered = true;
      reply.accepted = true;
      reply.detail = "registered";
      response = make_frame(MessageType::register_publisher_response, encode(reply));
      return true;
    }
    case MessageType::plan_request: {
      handle_plan_request(request, response);
      return true;
    }
    case MessageType::commit_request: {
      handle_commit_request(request, response);
      return true;
    }
    case MessageType::supersede_request: {
      const Result<SupersedeRequest> decoded = decode_supersede_request(
          std::span<const std::byte>(request.payload.data(), request.payload.size()));
      SupersedeResponse reply;
      if (!decoded.has_value()) {
        ++mutations_rejected;
        reply.code = decoded.error().code();
        reply.detail = decoded.error().detail();
        response = make_frame(MessageType::supersede_response, encode(reply));
        return true;
      }
      std::string detail;
      const Status binding = validate_binding(decoded.value().binding, detail);
      if (!binding.ok()) {
        ++mutations_rejected;
        reply.code = binding.code();
        reply.detail = detail;
        response = make_frame(MessageType::supersede_response, encode(reply));
        return true;
      }
      const Result<Plan> superseded =
          engine->supersede(decoded.value().incumbent, decoded.value().replacement);
      if (!superseded.has_value()) {
        ++mutations_rejected;
        reply.code = superseded.error().code();
        reply.detail = superseded.error().detail();
      } else {
        ++mutations_accepted;
        reply.code = ErrorCode::ok;
        reply.detail = "superseded";
        reply.incumbent = decoded.value().incumbent;
        reply.replacement = decoded.value().replacement;
      }
      response = make_frame(MessageType::supersede_response, encode(reply));
      return true;
    }
    case MessageType::retire_request: {
      const Result<RetireRequest> decoded = decode_retire_request(
          std::span<const std::byte>(request.payload.data(), request.payload.size()));
      RetireResponse reply;
      if (!decoded.has_value()) {
        ++mutations_rejected;
        reply.code = decoded.error().code();
        reply.detail = decoded.error().detail();
        response = make_frame(MessageType::retire_response, encode(reply));
        return true;
      }
      std::string detail;
      const Status binding = validate_binding(decoded.value().binding, detail);
      if (!binding.ok()) {
        ++mutations_rejected;
        reply.code = binding.code();
        reply.detail = detail;
        response = make_frame(MessageType::retire_response, encode(reply));
        return true;
      }
      const Result<Plan> retired = engine->retire(decoded.value().plan);
      if (!retired.has_value()) {
        ++mutations_rejected;
        reply.code = retired.error().code();
        reply.detail = retired.error().detail();
      } else {
        ++mutations_accepted;
        reply.code = ErrorCode::ok;
        reply.detail = "retired";
        reply.plan = retired.value().ref();
        reply.state = retired.value().state;
      }
      response = make_frame(MessageType::retire_response, encode(reply));
      return true;
    }
    case MessageType::query_request: {
      handle_query_request(request, response);
      return true;
    }
    case MessageType::ping:
      response = make_frame(MessageType::pong, std::vector<std::byte>{});
      return true;
    default:
      ++frames_rejected;
      response = make_frame(MessageType::error_response,
                            encode(ErrorResponse{ErrorCode::unsupported_feature,
                                                 "the message type is not supported"}));
      return true;
  }
}


void Coordinator::Impl::handle_plan_request(const Frame& request, Frame& response) {
  const Result<PlanRequest> decoded = decode_plan_request(
      std::span<const std::byte>(request.payload.data(), request.payload.size()));
  if (!decoded.has_value()) {
    ++mutations_rejected;
    response = make_frame(MessageType::error_response,
                          encode(ErrorResponse{decoded.error().code(), decoded.error().detail()}));
    return;
  }
  const PlanRequest& request_message = decoded.value();
  PlanResponse reply;
  std::string detail;
  const Status binding = validate_binding(request_message.binding, detail);
  if (!binding.ok()) {
    ++mutations_rejected;
    reply.code = binding.code();
    reply.detail = detail;
    response = make_frame(MessageType::plan_response, encode(reply));
    return;
  }
  const Result<FabricSnapshot> snapshot = decode_snapshot(
      std::span<const std::byte>(request_message.snapshot.data(), request_message.snapshot.size()));
  if (!snapshot.has_value()) {
    ++mutations_rejected;
    reply.code = snapshot.error().code();
    reply.detail = snapshot.error().detail();
    response = make_frame(MessageType::plan_response, encode(reply));
    return;
  }
  const Status submitted = engine->submit_snapshot(snapshot.value());
  if (!submitted.ok()) {
    ++mutations_rejected;
    reply.code = submitted.code();
    reply.detail = submitted.error().detail();
    response = make_frame(MessageType::plan_response, encode(reply));
    return;
  }
  if (!request_message.plan_id.valid()) {
    ++mutations_rejected;
    reply.code = ErrorCode::invalid_argument;
    reply.detail = "the plan request does not name a plan identity";
    response = make_frame(MessageType::plan_response, encode(reply));
    return;
  }
  DeclareRequest declare;
  declare.plan_id = request_message.plan_id;
  declare.attempt = request_message.binding.attempt;
  declare.attempt_generation = request_message.binding.attempt_generation;
  declare.publisher = request_message.binding.publisher;
  declare.publisher_boot = request_message.binding.publisher_boot;
  declare.allow_degraded = request_message.allow_degraded;
  declare.has_incumbent = request_message.has_incumbent;
  declare.incumbent_ref = request_message.incumbent_ref;
  if (request_message.has_incumbent) {
    const Result<Allocation> incumbent = decode_allocation(
        std::span<const std::byte>(request_message.incumbent.data(), request_message.incumbent.size()));
    if (!incumbent.has_value()) {
      ++mutations_rejected;
      reply.code = incumbent.error().code();
      reply.detail = incumbent.error().detail();
      response = make_frame(MessageType::plan_response, encode(reply));
      return;
    }
    declare.incumbent = incumbent.value();
  }
  declare.tick = epoch.load();
  const Result<Plan> declared = engine->declare(declare);
  if (!declared.has_value()) {
    ++mutations_rejected;
    reply.code = declared.error().code();
    reply.detail = declared.error().detail();
    response = make_frame(MessageType::plan_response, encode(reply));
    return;
  }
  const Status validated = engine->validate(declared.value().ref());
  if (!validated.ok()) {
    ++mutations_rejected;
    reply.code = validated.code();
    reply.detail = validated.error().detail();
    const auto current = engine->get(declared.value().ref());
    if (current.has_value()) {
      reply.plan = current->ref();
      reply.state = current->state;
      reply.feasibility = current->feasibility.status;
    }
    response = make_frame(MessageType::plan_response, encode(reply));
    return;
  }
  const Result<Plan> solved = engine->solve(declared.value().ref());
  if (!solved.has_value()) {
    ++mutations_rejected;
    reply.code = solved.error().code();
    reply.detail = solved.error().detail();
    const auto current = engine->get(declared.value().ref());
    if (current.has_value()) {
      reply.plan = current->ref();
      reply.state = current->state;
      reply.feasibility = current->feasibility.status;
    }
    response = make_frame(MessageType::plan_response, encode(reply));
    return;
  }
  ++mutations_accepted;
  reply.code = ErrorCode::ok;
  reply.detail = solved.value().feasibility.summary;
  reply.plan = solved.value().ref();
  reply.state = solved.value().state;
  reply.applicability = solved.value().applicability;
  reply.feasibility = solved.value().feasibility.status;
  reply.objective_score = solved.value().objective_score;
  reply.plan_digest = solved.value().content_digest();
  reply.explanation_digest = solved.value().explanation_digest;
  const auto explanation = engine->explain(solved.value().ref());
  if (explanation.has_value()) reply.explanation_json = bounded(explanation->to_json());
  const auto incumbent = engine->incumbent();
  if (incumbent.has_value()) reply.incumbent = incumbent->ref();
  response = make_frame(MessageType::plan_response, encode(reply));
  return;
}

void Coordinator::Impl::handle_commit_request(const Frame& request, Frame& response) {
  const Result<CommitRequest> decoded = decode_commit_request(
      std::span<const std::byte>(request.payload.data(), request.payload.size()));
  if (!decoded.has_value()) {
    ++mutations_rejected;
    response = make_frame(MessageType::error_response,
                          encode(ErrorResponse{decoded.error().code(), decoded.error().detail()}));
    return;
  }
  const CommitRequest& request_message = decoded.value();
  CommitResponse reply;
  std::string detail;
  const Status binding = validate_binding(request_message.binding, detail);
  if (!binding.ok()) {
    ++mutations_rejected;
    reply.code = binding.code();
    reply.detail = detail;
    response = make_frame(MessageType::commit_response, encode(reply));
    return;
  }
  const auto existing = engine->get(request_message.plan);
  if (existing.has_value() && existing->state == PlanState::committed &&
      existing->commit == request_message.commit &&
      existing->commit_generation == request_message.commit_generation) {
    ++duplicate_commit_replays;
    reply.code = ErrorCode::ok;
    reply.detail = "idempotent replay of an already committed plan";
    reply.plan = existing->ref();
    reply.state = existing->state;
    reply.commit_generation = existing->commit_generation;
    reply.commit_digest = existing->content_digest();
    reply.idempotent_replay = true;
    reply.churn = existing->churn;
    response = make_frame(MessageType::commit_response, encode(reply));
    return;
  }
  const auto plan = engine->get(request_message.plan);
  if (!plan.has_value()) {
    ++mutations_rejected;
    reply.code = ErrorCode::not_found;
    reply.detail = "unknown plan";
    response = make_frame(MessageType::commit_response, encode(reply));
    return;
  }
  if (plan->state == PlanState::proposed || plan->state == PlanState::degraded) {
    const Result<Plan> authorized = engine->authorize(plan->ref(), AuthorizeRequest{});
    if (!authorized.has_value()) {
      ++mutations_rejected;
      reply.code = authorized.error().code();
      reply.detail = authorized.error().detail();
      reply.plan = plan->ref();
      const auto refreshed = engine->get(plan->ref());
      if (refreshed.has_value()) reply.state = refreshed->state;
      response = make_frame(MessageType::commit_response, encode(reply));
      return;
    }
  }
  const Result<Plan> committed =
      engine->commit(request_message.plan, CommitIntent{request_message.commit,
                                                        request_message.commit_generation,
                                                        epoch.load()});
  if (!committed.has_value()) {
    ++mutations_rejected;
    reply.code = committed.error().code();
    reply.detail = committed.error().detail();
    const auto refreshed = engine->get(request_message.plan);
    if (refreshed.has_value()) {
      reply.plan = refreshed->ref();
      reply.state = refreshed->state;
    }
    response = make_frame(MessageType::commit_response, encode(reply));
    return;
  }
  ++mutations_accepted;
  reply.code = ErrorCode::ok;
  reply.detail = "committed";
  reply.plan = committed.value().ref();
  reply.state = committed.value().state;
  reply.commit_generation = committed.value().commit_generation;
  reply.commit_digest = committed.value().content_digest();
  reply.churn = committed.value().churn;
  response = make_frame(MessageType::commit_response, encode(reply));
}

void Coordinator::Impl::handle_query_request(const Frame& request, Frame& response) {
  const Result<QueryRequest> decoded = decode_query_request(
      std::span<const std::byte>(request.payload.data(), request.payload.size()));
  QueryResponse reply;
  if (!decoded.has_value()) {
    reply.code = decoded.error().code();
    reply.detail = decoded.error().detail();
    response = make_frame(MessageType::query_response, encode(reply));
    return;
  }
  const auto plan = engine->get(decoded.value().plan);
  if (!plan.has_value()) {
    reply.code = ErrorCode::not_found;
    reply.detail = "unknown plan";
    response = make_frame(MessageType::query_response, encode(reply));
    return;
  }
  reply.code = ErrorCode::ok;
  reply.detail = "ok";
  reply.plan = plan.value();
  reply.applicability = plan->applicability;
  if (decoded.value().include_explanation) {
    const auto explanation = engine->explain(plan->ref());
    if (explanation.has_value()) reply.explanation_json = bounded(explanation->to_json());
  }
  response = make_frame(MessageType::query_response, encode(reply));
}
void Coordinator::Impl::handle_connection(const std::shared_ptr<Session>& session) {
  while (!stopping.load()) {
    Frame request;
    std::string error;
    if (!session->socket.read_frame(request, error)) {
      if (!error.empty()) ++frames_rejected;
      break;
    }
    ++frames_read;
    Frame response;
    if (!dispatch(session, request, response)) break;
    if (!session->socket.write_frame(response, error)) {
      ++frames_rejected;
      break;
    }
    ++frames_written;
  }
  session->socket.close();
}

Status Coordinator::stop() {
  Impl& impl = *impl_;
  if (!impl.started.load()) {
    return Status::success();
  }
  impl.stopping.store(true);
  impl.listener.interrupt();
  if (impl.acceptor.joinable()) impl.acceptor.join();
  impl.listener.close();

  std::vector<std::shared_ptr<Session>> sessions;
  {
    std::lock_guard<std::mutex> guard(impl.registry_mutex);
    sessions = impl.sessions;
  }
  for (const auto& session : sessions) session->socket.interrupt();

  for (auto& worker : impl.workers) {
    if (worker.joinable()) worker.join();
  }
  impl.workers.clear();
  {
    std::lock_guard<std::mutex> guard(impl.registry_mutex);
    impl.sessions.clear();
    impl.registered.clear();
  }
  impl.started.store(false);
  if (impl.repository) {
    const Status closed = impl.repository->close();
    if (!closed.ok()) return closed;
  }
  return Status::success();
}

std::uint16_t Coordinator::port() const noexcept { return impl_->listener.local_port(); }
std::string Coordinator::address() const { return impl_->listener.local_address(); }
std::uint64_t Coordinator::epoch() const noexcept { return impl_->epoch.load(); }
BootId Coordinator::boot_id() const noexcept { return impl_->boot; }
CoordinatorIncarnation Coordinator::incarnation() const noexcept { return impl_->incarnation; }
Engine& Coordinator::engine() noexcept { return *impl_->engine; }
PlanRepository* Coordinator::repository() noexcept { return impl_->repository.get(); }

Coordinator::Stats Coordinator::stats() const {
  Stats out;
  out.connections_accepted = impl_->connections_accepted.load();
  out.connections_rejected = impl_->connections_rejected.load();
  out.frames_read = impl_->frames_read.load();
  out.frames_written = impl_->frames_written.load();
  out.frames_rejected = impl_->frames_rejected.load();
  out.mutations_accepted = impl_->mutations_accepted.load();
  out.mutations_rejected = impl_->mutations_rejected.load();
  out.stale_epoch_rejections = impl_->stale_epoch_rejections.load();
  out.fenced_rejections = impl_->fenced_rejections.load();
  out.duplicate_commit_replays = impl_->duplicate_commit_replays.load();
  out.active_connections = impl_->active_connections.load();
  out.peak_connections = impl_->peak_connections.load();
  return out;
}

std::vector<FenceRecord> Coordinator::fences() const {
  std::lock_guard<std::mutex> guard(impl_->registry_mutex);
  return impl_->fences;
}

}  // namespace tef
