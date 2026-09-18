// Traffic Engineering Fabric - publisher client.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "tef/publisher.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "tef/version.hpp"

namespace tef {
namespace {

Frame make_frame(MessageType type, std::vector<std::byte> payload) {
  Frame frame;
  frame.version = static_cast<std::uint16_t>(kWireProtocolVersion);
  frame.type = static_cast<std::uint16_t>(type);
  frame.flags = 0;
  frame.payload = std::move(payload);
  return frame;
}

Result<Frame> expect_frame(TcpSocket& socket, MessageType expected, std::string& error) {
  Frame response;
  if (!socket.read_frame(response, error)) {
    return fail_as<Frame>(ErrorCode::transport_failure, error);
  }
  const auto type = static_cast<MessageType>(response.type);
  if (type == MessageType::error_response) {
    const Result<ErrorResponse> decoded =
        decode_error_response(std::span<const std::byte>(response.payload.data(), response.payload.size()));
    if (decoded.has_value()) {
      return fail_as<Frame>(decoded.value().code, decoded.value().detail);
    }
    return fail_as<Frame>(ErrorCode::malformed_input, "the coordinator returned a malformed error frame");
  }
  if (type != expected) {
    return fail_as<Frame>(ErrorCode::malformed_input,
                          "the coordinator returned an unexpected message type");
  }
  return response;
}

}  // namespace

struct PublisherClient::Impl {
  PublisherConfig config;
  TcpSocket socket;
  std::uint64_t coordinator_epoch = 0;
  CoordinatorIncarnation coordinator_incarnation;
  BootId coordinator_boot;
  AttemptGeneration attempt_generation = AttemptGeneration::first();
  std::uint64_t attempt_counter = 0;
  CommitGeneration commit_generation = CommitGeneration::first();
  std::uint64_t commit_counter = 0;
};

PublisherClient::PublisherClient() : impl_(std::make_unique<Impl>()) {}
PublisherClient::~PublisherClient() {
  if (impl_) impl_->socket.close();
}

Result<std::unique_ptr<PublisherClient>> PublisherClient::connect(const PublisherConfig& config,
                                                                  std::string& transport_error) {
  if (!config.publisher.valid() || !config.boot.valid()) {
    return fail_as<std::unique_ptr<PublisherClient>>(ErrorCode::invalid_argument,
                                                     "the publisher identity or boot id is absent");
  }
  auto client = std::unique_ptr<PublisherClient>(new PublisherClient());
  client->impl_->config = config;
  const Status connected = connect_tcp(config.address, config.port, client->impl_->socket, transport_error);
  if (!connected.ok()) {
    return Result<std::unique_ptr<PublisherClient>>(connected.error());
  }
  return client;
}

Status PublisherClient::handshake() {
  Impl& impl = *impl_;
  HelloRequest request;
  request.protocol_version = static_cast<std::uint16_t>(kWireProtocolVersion);
  std::string error;
  if (!impl.socket.write_frame(make_frame(MessageType::hello_request, encode(request)), error)) {
    return Status(Error(ErrorCode::transport_failure, error));
  }
  const Result<Frame> response = expect_frame(impl.socket, MessageType::hello_response, error);
  if (!response.has_value()) return Status(response.error());
  const Result<HelloResponse> decoded = decode_hello_response(
      std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
  if (!decoded.has_value()) return Status(decoded.error());
  if (decoded.value().protocol_version != static_cast<std::uint16_t>(kWireProtocolVersion)) {
    return fail(ErrorCode::unsupported_version, "the coordinator negotiated an unsupported protocol version");
  }
  impl.coordinator_epoch = decoded.value().coordinator_epoch;
  impl.coordinator_incarnation = decoded.value().coordinator_incarnation;
  impl.coordinator_boot = decoded.value().coordinator_boot;
  return Status::success();
}

Status PublisherClient::register_publisher() {
  Impl& impl = *impl_;
  if (impl.coordinator_epoch == 0) {
    const Status greeted = handshake();
    if (!greeted.ok()) return greeted;
  }
  RegisterPublisherRequest request;
  request.publisher = impl.config.publisher;
  request.boot = impl.config.boot;
  request.coordinator_incarnation = impl.coordinator_incarnation;
  request.coordinator_epoch = impl.coordinator_epoch;
  request.description = impl.config.description;
  std::string error;
  if (!impl.socket.write_frame(make_frame(MessageType::register_publisher_request, encode(request)), error)) {
    return Status(Error(ErrorCode::transport_failure, error));
  }
  const Result<Frame> response = expect_frame(impl.socket, MessageType::register_publisher_response, error);
  if (!response.has_value()) return Status(response.error());
  const Result<RegisterPublisherResponse> decoded = decode_register_publisher_response(
      std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
  if (!decoded.has_value()) return Status(decoded.error());
  if (!decoded.value().accepted) {
    return Status(Error(decoded.value().fenced ? ErrorCode::fenced : ErrorCode::unauthorized,
                        decoded.value().detail));
  }
  return Status::success();
}

Result<PlanResponse> PublisherClient::submit_plan(const FabricSnapshot& snapshot,
                                                  const DeclareRequest& request) {
  Impl& impl = *impl_;
  if (impl.coordinator_epoch == 0) {
    const Status greeted = handshake();
    if (!greeted.ok()) return Result<PlanResponse>(greeted.error());
  }
  PlanRequest message;
  message.binding.publisher = impl.config.publisher;
  message.binding.publisher_boot = impl.config.boot;
  message.binding.coordinator_incarnation = impl.coordinator_incarnation;
  message.binding.coordinator_epoch = impl.coordinator_epoch;
  message.binding.attempt = request.attempt;
  ++impl.attempt_counter;
  message.binding.attempt_generation =
      AttemptGeneration::parse(impl.attempt_counter).value_or(AttemptGeneration::first());
  message.plan_id = request.plan_id;
  message.snapshot = encode_snapshot(snapshot);
  message.allow_degraded = request.allow_degraded;
  message.has_incumbent = request.has_incumbent;
  message.incumbent_ref = request.incumbent_ref;
  if (request.has_incumbent) message.incumbent = encode_allocation(request.incumbent);

  std::string error;
  if (!impl.socket.write_frame(make_frame(MessageType::plan_request, encode(message)), error)) {
    return fail_as<PlanResponse>(ErrorCode::transport_failure, error);
  }
  const Result<Frame> response = expect_frame(impl.socket, MessageType::plan_response, error);
  if (!response.has_value()) return Result<PlanResponse>(response.error());
  const Result<PlanResponse> decoded = decode_plan_response(
      std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
  if (!decoded.has_value()) return Result<PlanResponse>(decoded.error());
  return decoded.value();
}

Result<CommitResponse> PublisherClient::commit(const PlanRef& plan, const CommitId& commit_id,
                                               const CommitGeneration& generation) {
  Impl& impl = *impl_;
  CommitRequest message;
  message.binding.publisher = impl.config.publisher;
  message.binding.publisher_boot = impl.config.boot;
  message.binding.coordinator_incarnation = impl.coordinator_incarnation;
  message.binding.coordinator_epoch = impl.coordinator_epoch;
  message.binding.attempt = AttemptId::parse("commit-attempt-" + std::to_string(impl.commit_counter))
                                .value_or(AttemptId{});
  ++impl.commit_counter;
  message.binding.attempt_generation = AttemptGeneration::first();
  message.plan = plan;
  message.commit = commit_id;
  message.commit_generation = generation;

  std::string error;
  if (!impl.socket.write_frame(make_frame(MessageType::commit_request, encode(message)), error)) {
    return fail_as<CommitResponse>(ErrorCode::transport_failure, error);
  }
  const Result<Frame> response = expect_frame(impl.socket, MessageType::commit_response, error);
  if (!response.has_value()) return Result<CommitResponse>(response.error());
  const Result<CommitResponse> decoded = decode_commit_response(
      std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
  if (!decoded.has_value()) return Result<CommitResponse>(decoded.error());
  return decoded.value();
}

Result<SupersedeResponse> PublisherClient::supersede(const PlanRef& incumbent, const PlanRef& replacement) {
  Impl& impl = *impl_;
  SupersedeRequest message;
  message.binding.publisher = impl.config.publisher;
  message.binding.publisher_boot = impl.config.boot;
  message.binding.coordinator_incarnation = impl.coordinator_incarnation;
  message.binding.coordinator_epoch = impl.coordinator_epoch;
  message.binding.attempt = AttemptId::parse("supersede-attempt-" + std::to_string(impl.commit_counter))
                                .value_or(AttemptId{});
  ++impl.commit_counter;
  message.binding.attempt_generation = AttemptGeneration::first();
  message.incumbent = incumbent;
  message.replacement = replacement;

  std::string error;
  if (!impl.socket.write_frame(make_frame(MessageType::supersede_request, encode(message)), error)) {
    return fail_as<SupersedeResponse>(ErrorCode::transport_failure, error);
  }
  const Result<Frame> response = expect_frame(impl.socket, MessageType::supersede_response, error);
  if (!response.has_value()) return Result<SupersedeResponse>(response.error());
  const Result<SupersedeResponse> decoded = decode_supersede_response(
      std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
  if (!decoded.has_value()) return Result<SupersedeResponse>(decoded.error());
  return decoded.value();
}

Result<RetireResponse> PublisherClient::retire(const PlanRef& plan) {
  Impl& impl = *impl_;
  RetireRequest message;
  message.binding.publisher = impl.config.publisher;
  message.binding.publisher_boot = impl.config.boot;
  message.binding.coordinator_incarnation = impl.coordinator_incarnation;
  message.binding.coordinator_epoch = impl.coordinator_epoch;
  message.binding.attempt = AttemptId::parse("retire-attempt-" + std::to_string(impl.commit_counter))
                                .value_or(AttemptId{});
  ++impl.commit_counter;
  message.binding.attempt_generation = AttemptGeneration::first();
  message.plan = plan;

  std::string error;
  if (!impl.socket.write_frame(make_frame(MessageType::retire_request, encode(message)), error)) {
    return fail_as<RetireResponse>(ErrorCode::transport_failure, error);
  }
  const Result<Frame> response = expect_frame(impl.socket, MessageType::retire_response, error);
  if (!response.has_value()) return Result<RetireResponse>(response.error());
  const Result<RetireResponse> decoded = decode_retire_response(
      std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
  if (!decoded.has_value()) return Result<RetireResponse>(decoded.error());
  return decoded.value();
}

Result<QueryResponse> PublisherClient::query(const PlanRef& plan, bool include_explanation) {
  Impl& impl = *impl_;
  QueryRequest message;
  message.plan = plan;
  message.include_explanation = include_explanation;
  std::string error;
  if (!impl.socket.write_frame(make_frame(MessageType::query_request, encode(message)), error)) {
    return fail_as<QueryResponse>(ErrorCode::transport_failure, error);
  }
  const Result<Frame> response = expect_frame(impl.socket, MessageType::query_response, error);
  if (!response.has_value()) return Result<QueryResponse>(response.error());
  const Result<QueryResponse> decoded = decode_query_response(
      std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
  if (!decoded.has_value()) return Result<QueryResponse>(decoded.error());
  return decoded.value();
}

bool PublisherClient::send_raw(const Frame& frame, std::string& error) {
  return impl_->socket.write_frame(frame, error);
}

bool PublisherClient::receive_raw(Frame& frame, std::string& error) {
  return impl_->socket.read_frame(frame, error);
}

std::uint64_t PublisherClient::coordinator_epoch() const noexcept { return impl_->coordinator_epoch; }
CoordinatorIncarnation PublisherClient::coordinator_incarnation() const noexcept {
  return impl_->coordinator_incarnation;
}
const PublisherConfig& PublisherClient::config() const noexcept { return impl_->config; }

}  // namespace tef
