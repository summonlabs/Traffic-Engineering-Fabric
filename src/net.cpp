// Traffic Engineering Fabric - real TCP framed transport.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Concurrency contract: interrupt() and close() may be called from a thread
// other than the one blocked in read_frame()/accept(). Both only ever call
// shutdown() under the socket mutex, which unblocks the pending call without
// invalidating the handle. close() additionally performs the actual
// closesocket(), and the owner must not be inside read_frame() when it runs;
// the poll loop treats a closed or interrupted socket as a terminal read
// failure and never retries.
#include "tef/net.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "tef/digest.hpp"
#include "tef/version.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_handle = SOCKET;
constexpr socket_handle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_handle = int;
constexpr socket_handle kInvalidSocket = -1;
#endif

namespace tef {
namespace {

std::mutex g_net_mutex;
int g_net_users = 0;

#if defined(_WIN32)
std::string g_last_net_error;
void record_error(const char* context, int code) {
  g_last_net_error = std::string(context) + " failed with code " + std::to_string(code);
}
#else
std::string g_last_net_error;
void record_error(const char* context, int code) {
  g_last_net_error = std::string(context) + " failed with errno " + std::to_string(code);
}
#endif

int close_socket(socket_handle handle) {
#if defined(_WIN32)
  return ::closesocket(handle);
#else
  return ::close(handle);
#endif
}

int shutdown_socket(socket_handle handle) {
#if defined(_WIN32)
  return ::shutdown(handle, SD_BOTH);
#else
  return ::shutdown(handle, SHUT_RDWR);
#endif
}

bool would_block() {
#if defined(_WIN32)
  const int code = ::WSAGetLastError();
  return code == WSAEWOULDBLOCK || code == WSAEINTR || code == WSAETIMEDOUT;
#else
  return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
#endif
}

bool socket_valid(socket_handle handle) { return handle != kInvalidSocket; }

// Shared socket identity. interrupt() only shuts the socket down; the handle is
// closed exactly once, by the owning wrapper.
struct SocketState {
  mutable std::mutex mutex;
  socket_handle handle = kInvalidSocket;
  std::atomic<bool> interrupted{false};

  void interrupt() {
    std::lock_guard<std::mutex> guard(mutex);
    interrupted.store(true);
    if (socket_valid(handle)) (void)shutdown_socket(handle);
  }

  void close() {
    std::lock_guard<std::mutex> guard(mutex);
    interrupted.store(true);
    if (socket_valid(handle)) {
      (void)shutdown_socket(handle);
      (void)close_socket(handle);
      handle = kInvalidSocket;
    }
  }

  bool valid() const {
    std::lock_guard<std::mutex> guard(mutex);
    return socket_valid(handle);
  }

  socket_handle get() const {
    std::lock_guard<std::mutex> guard(mutex);
    return handle;
  }
};

// Waits for readability. Returns 1 when readable, 0 on poll timeout, -1 on
// error or interruption.
int wait_readable(socket_handle handle, int timeout_ms, const std::atomic<bool>& interrupted) {
  if (interrupted.load()) return -1;
#if defined(_WIN32)
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(handle, &read_set);
  timeval timeout;
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  const int result = ::select(0, &read_set, nullptr, nullptr, &timeout);
  if (result == SOCKET_ERROR) {
    const int code = ::WSAGetLastError();
    if (code == WSAEINTR) return 0;
    return -1;
  }
  return result;
#else
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(handle, &read_set);
  timeval timeout;
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  const int result = ::select(handle + 1, &read_set, nullptr, nullptr, &timeout);
  if (result < 0) {
    if (errno == EINTR) return 0;
    return -1;
  }
  return result;
#endif
}

int recv_some(socket_handle handle, void* buffer, std::size_t size) {
#if defined(_WIN32)
  return ::recv(handle, static_cast<char*>(buffer), static_cast<int>(size), 0);
#else
  return static_cast<int>(::recv(handle, buffer, size, 0));
#endif
}

int send_some(socket_handle handle, const void* buffer, std::size_t size) {
#if defined(_WIN32)
  return ::send(handle, static_cast<const char*>(buffer), static_cast<int>(size), 0);
#else
  return static_cast<int>(::send(handle, buffer, size, MSG_NOSIGNAL));
#endif
}

}  // namespace

void net_startup() {
  std::lock_guard<std::mutex> guard(g_net_mutex);
  if (g_net_users == 0) {
#if defined(_WIN32)
    WSADATA data;
    const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
      record_error("WSAStartup", result);
    }
#endif
  }
  ++g_net_users;
}

void net_shutdown() {
  std::lock_guard<std::mutex> guard(g_net_mutex);
  if (g_net_users == 0) return;
  --g_net_users;
  if (g_net_users == 0) {
#if defined(_WIN32)
    (void)::WSACleanup();
#endif
  }
}

std::string last_net_error() {
  std::lock_guard<std::mutex> guard(g_net_mutex);
  return g_last_net_error;
}

std::vector<std::byte> encode_frame(const Frame& frame) {
  std::vector<std::byte> out(kFrameHeaderSize + frame.payload.size());
  std::uint8_t* header = reinterpret_cast<std::uint8_t*>(out.data());
  header[0] = 0x54;  // 'T'
  header[1] = 0x45;  // 'E'
  header[2] = 0x46;  // 'F'
  header[3] = 0x31;  // '1'
  header[4] = static_cast<std::uint8_t>(frame.version >> 8);
  header[5] = static_cast<std::uint8_t>(frame.version);
  header[6] = static_cast<std::uint8_t>(frame.type >> 8);
  header[7] = static_cast<std::uint8_t>(frame.type);
  for (int i = 0; i < 4; ++i) {
    header[8 + i] = static_cast<std::uint8_t>(frame.flags >> (8 * (3 - i)));
  }
  const auto length = static_cast<std::uint32_t>(frame.payload.size());
  for (int i = 0; i < 4; ++i) {
    header[12 + i] = static_cast<std::uint8_t>(length >> (8 * (3 - i)));
  }
  const std::uint32_t checksum =
      crc32c(std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
  for (int i = 0; i < 4; ++i) {
    header[16 + i] = static_cast<std::uint8_t>(checksum >> (8 * (3 - i)));
  }
  if (!frame.payload.empty()) {
    std::memcpy(out.data() + kFrameHeaderSize, frame.payload.data(), frame.payload.size());
  }
  return out;
}

bool decode_frame_header(std::span<const std::byte> header, Frame& frame, std::string& error) {
  if (header.size() < kFrameHeaderSize) {
    error = "the frame header is truncated";
    return false;
  }
  const std::uint8_t* in = reinterpret_cast<const std::uint8_t*>(header.data());
  const std::uint32_t magic = (static_cast<std::uint32_t>(in[0]) << 24) |
                              (static_cast<std::uint32_t>(in[1]) << 16) |
                              (static_cast<std::uint32_t>(in[2]) << 8) |
                              static_cast<std::uint32_t>(in[3]);
  if (magic != kFrameMagic) {
    error = "the frame magic does not match";
    return false;
  }
  const std::uint16_t version =
      static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[4]) << 8) | in[5]);
  if (version == 0 || version > static_cast<std::uint16_t>(kWireProtocolVersion)) {
    error = "the frame protocol version is not supported";
    return false;
  }
  frame.version = version;
  frame.type = static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[6]) << 8) | in[7]);
  std::uint32_t flags = 0;
  for (int i = 0; i < 4; ++i) flags = (flags << 8) | in[8 + i];
  frame.flags = flags;
  std::uint32_t length = 0;
  for (int i = 0; i < 4; ++i) length = (length << 8) | in[12 + i];
  if (length > Limits::max_frame_payload) {
    error = "the frame payload exceeds the supported bound";
    return false;
  }
  frame.payload.clear();
  frame.payload.reserve(length);
  return true;
}

// ---------------------------------------------------------------------------
// TcpSocket
// ---------------------------------------------------------------------------

struct TcpSocket::Impl {
  std::shared_ptr<SocketState> state = std::make_shared<SocketState>();
  int poll_interval_ms = 100;
  std::uint16_t peer_port = 0;
};

TcpSocket::TcpSocket() : impl_(std::make_unique<Impl>()) {}
TcpSocket::~TcpSocket() {
  if (impl_) impl_->state->close();
}
TcpSocket::TcpSocket(TcpSocket&& other) noexcept : impl_(std::move(other.impl_)) {}
TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
  if (this != &other) {
    if (impl_) impl_->state->close();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

bool TcpSocket::valid() const noexcept { return impl_ && impl_->state->valid(); }

void TcpSocket::close() {
  if (impl_) impl_->state->close();
}

std::uint16_t TcpSocket::peer_port() const noexcept { return impl_ ? impl_->peer_port : 0; }

void TcpSocket::set_poll_interval_ms(int milliseconds) {
  if (!impl_) return;
  impl_->poll_interval_ms = milliseconds <= 0 ? 1 : milliseconds;
}

int TcpSocket::poll_interval_ms() const noexcept { return impl_ ? impl_->poll_interval_ms : 0; }

void TcpSocket::interrupt() {
  if (impl_) impl_->state->interrupt();
}

void TcpSocket::configure_stream() {
  if (!impl_) return;
  const socket_handle handle = impl_->state->get();
  if (!socket_valid(handle)) return;
  const char enabled = 1;
  (void)::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
}

bool TcpSocket::read_frame(Frame& frame, std::string& error) {
  if (!impl_) {
    error = "the socket is not initialised";
    return false;
  }
  const socket_handle handle = impl_->state->get();
  if (!socket_valid(handle)) {
    error = "the socket is closed";
    return false;
  }

  std::array<std::byte, kFrameHeaderSize> header{};
  std::size_t received = 0;
  while (received < header.size()) {
    const int ready = wait_readable(handle, impl_->poll_interval_ms, impl_->state->interrupted);
    if (ready < 0) {
      error = "the socket was interrupted or failed while waiting for a frame header";
      return false;
    }
    if (ready == 0) continue;
    const int count = recv_some(handle, header.data() + received, header.size() - received);
    if (count == 0) {
      error = received == 0 ? "the peer closed the connection" : "the connection closed mid-frame";
      return false;
    }
    if (count < 0) {
      if (would_block()) continue;
      error = "the socket read failed";
      return false;
    }
    received += static_cast<std::size_t>(count);
  }

  if (!decode_frame_header(std::span<const std::byte>(header.data(), header.size()), frame, error)) {
    return false;
  }
  const std::uint8_t* header_bytes = reinterpret_cast<const std::uint8_t*>(header.data());
  const std::uint32_t expected_crc = [header_bytes]() {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value = (value << 8) | header_bytes[16 + i];
    return value;
  }();
  const std::uint32_t expected_length = [header_bytes]() {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value = (value << 8) | header_bytes[12 + i];
    return value;
  }();

  frame.payload.assign(expected_length, std::byte{0});
  received = 0;
  while (received < frame.payload.size()) {
    const int ready = wait_readable(handle, impl_->poll_interval_ms, impl_->state->interrupted);
    if (ready < 0) {
      error = "the socket was interrupted or failed while reading a frame payload";
      return false;
    }
    if (ready == 0) continue;
    const int count = recv_some(handle, frame.payload.data() + received, frame.payload.size() - received);
    if (count == 0) {
      error = "the connection closed mid-frame";
      return false;
    }
    if (count < 0) {
      if (would_block()) continue;
      error = "the socket read failed";
      return false;
    }
    received += static_cast<std::size_t>(count);
  }

  if (crc32c(std::span<const std::byte>(frame.payload.data(), frame.payload.size())) != expected_crc) {
    error = "the frame payload checksum does not match";
    return false;
  }
  return true;
}

bool TcpSocket::write_raw(std::span<const std::byte> bytes, std::string& error) {
  if (!impl_) {
    error = "the socket is not initialised";
    return false;
  }
  const socket_handle handle = impl_->state->get();
  if (!socket_valid(handle)) {
    error = "the socket is closed";
    return false;
  }
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const int count = send_some(handle, bytes.data() + sent, bytes.size() - sent);
    if (count <= 0) {
      if (count < 0 && would_block()) continue;
      error = "the socket write failed";
      return false;
    }
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

bool TcpSocket::write_frame(const Frame& frame, std::string& error) {
  if (!impl_) {
    error = "the socket is not initialised";
    return false;
  }
  const socket_handle handle = impl_->state->get();
  if (!socket_valid(handle)) {
    error = "the socket is closed";
    return false;
  }
  if (frame.payload.size() > Limits::max_frame_payload) {
    error = "the frame payload exceeds the supported bound";
    return false;
  }
  Frame outbound = frame;
  if (outbound.version == 0) outbound.version = static_cast<std::uint16_t>(kWireProtocolVersion);
  const std::vector<std::byte> bytes = encode_frame(outbound);
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const int count = send_some(handle, bytes.data() + sent, bytes.size() - sent);
    if (count <= 0) {
      if (count < 0 && would_block()) continue;
      error = "the socket write failed";
      return false;
    }
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

// ---------------------------------------------------------------------------
// TcpListener
// ---------------------------------------------------------------------------

struct TcpListener::Impl {
  std::shared_ptr<SocketState> state = std::make_shared<SocketState>();
  int poll_interval_ms = 100;
  std::uint16_t local_port = 0;
  std::string local_address;
};

TcpListener::TcpListener() : impl_(std::make_unique<Impl>()) {}
TcpListener::~TcpListener() {
  if (impl_) impl_->state->close();
}
TcpListener::TcpListener(TcpListener&& other) noexcept : impl_(std::move(other.impl_)) {}
TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this != &other) {
    if (impl_) impl_->state->close();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

Status TcpListener::listen(const std::string& address, std::uint16_t port, int backlog) {
  net_startup();
  if (!impl_) impl_ = std::make_unique<Impl>();

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* results = nullptr;
  const std::string port_text = std::to_string(port);
  const int resolved = ::getaddrinfo(address.empty() ? nullptr : address.c_str(), port_text.c_str(),
                                     &hints, &results);
  if (resolved != 0 || results == nullptr) {
    if (results != nullptr) ::freeaddrinfo(results);
    return fail(ErrorCode::transport_failure, "cannot resolve the bind address " + address);
  }

  socket_handle handle = kInvalidSocket;
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    handle = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (!socket_valid(handle)) continue;
    const char enabled = 1;
    (void)::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (::bind(handle, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0 &&
        ::listen(handle, backlog) == 0) {
      break;
    }
    (void)close_socket(handle);
    handle = kInvalidSocket;
  }
  ::freeaddrinfo(results);

  if (!socket_valid(handle)) {
    return fail(ErrorCode::transport_failure, "cannot bind the listener to " + address + ":" + port_text);
  }

  sockaddr_storage bound{};
  int bound_length = static_cast<int>(sizeof(bound));
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
    if (bound.ss_family == AF_INET) {
      const auto* v4 = reinterpret_cast<const sockaddr_in*>(&bound);
      char text[INET_ADDRSTRLEN] = {};
      if (::inet_ntop(AF_INET, &v4->sin_addr, text, sizeof(text)) != nullptr) {
        impl_->local_address = text;
      }
      impl_->local_port = ntohs(v4->sin_port);
    } else if (bound.ss_family == AF_INET6) {
      const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&bound);
      char text[INET6_ADDRSTRLEN] = {};
      if (::inet_ntop(AF_INET6, &v6->sin6_addr, text, sizeof(text)) != nullptr) {
        impl_->local_address = text;
      }
      impl_->local_port = ntohs(v6->sin6_port);
    }
  }

  {
    std::lock_guard<std::mutex> guard(impl_->state->mutex);
    impl_->state->handle = handle;
    impl_->state->interrupted.store(false);
  }
  return Status::success();
}

void TcpListener::close() {
  if (impl_) impl_->state->close();
}

bool TcpListener::valid() const noexcept { return impl_ && impl_->state->valid(); }

std::uint16_t TcpListener::local_port() const noexcept { return impl_ ? impl_->local_port : 0; }

std::string TcpListener::local_address() const { return impl_ ? impl_->local_address : std::string(); }

void TcpListener::set_poll_interval_ms(int milliseconds) {
  if (impl_) impl_->poll_interval_ms = milliseconds <= 0 ? 1 : milliseconds;
}

void TcpListener::interrupt() {
  if (impl_) impl_->state->interrupt();
}

bool TcpListener::accept(TcpSocket& out, std::string& error) {
  error.clear();
  if (!impl_) {
    error = "the listener is not initialised";
    return false;
  }
  const socket_handle handle = impl_->state->get();
  if (!socket_valid(handle)) {
    error = "the listener is closed";
    return false;
  }

  const int ready = wait_readable(handle, impl_->poll_interval_ms, impl_->state->interrupted);
  if (ready < 0) {
    error = "the listener was interrupted";
    return false;
  }
  if (ready == 0) return false;

  sockaddr_storage peer{};
  int peer_length = static_cast<int>(sizeof(peer));
  const socket_handle accepted = ::accept(handle, reinterpret_cast<sockaddr*>(&peer), &peer_length);
  if (!socket_valid(accepted)) {
    if (impl_->state->interrupted.load()) {
      error = "the listener was interrupted";
      return false;
    }
    error = "the listener accept call failed";
    return false;
  }

  out.close();
  if (!out.impl_) out.impl_ = std::make_unique<TcpSocket::Impl>();
  {
    std::lock_guard<std::mutex> guard(out.impl_->state->mutex);
    out.impl_->state->handle = accepted;
    out.impl_->state->interrupted.store(false);
  }
  out.impl_->peer_port = 0;
  if (peer.ss_family == AF_INET) {
    out.impl_->peer_port = ntohs(reinterpret_cast<const sockaddr_in*>(&peer)->sin_port);
  } else if (peer.ss_family == AF_INET6) {
    out.impl_->peer_port = ntohs(reinterpret_cast<const sockaddr_in6*>(&peer)->sin6_port);
  }
  out.set_poll_interval_ms(impl_->poll_interval_ms);
  out.configure_stream();
  return true;
}

// ---------------------------------------------------------------------------
// Client connect
// ---------------------------------------------------------------------------

Status connect_tcp(const std::string& address, std::uint16_t port, TcpSocket& out,
                   std::string& error) {
  net_startup();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string port_text = std::to_string(port);
  const int resolved = ::getaddrinfo(address.c_str(), port_text.c_str(), &hints, &results);
  if (resolved != 0 || results == nullptr) {
    if (results != nullptr) ::freeaddrinfo(results);
    error = "cannot resolve " + address + ":" + port_text;
    return Status(Error(ErrorCode::transport_failure, error));
  }

  socket_handle handle = kInvalidSocket;
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    handle = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (!socket_valid(handle)) continue;
    if (::connect(handle, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) break;
    (void)close_socket(handle);
    handle = kInvalidSocket;
  }
  ::freeaddrinfo(results);

  if (!socket_valid(handle)) {
    error = "cannot connect to " + address + ":" + port_text;
    return Status(Error(ErrorCode::transport_failure, error));
  }

  TcpSocket socket;
  if (!socket.impl_) socket.impl_ = std::make_unique<TcpSocket::Impl>();
  {
    std::lock_guard<std::mutex> guard(socket.impl_->state->mutex);
    socket.impl_->state->handle = handle;
    socket.impl_->state->interrupted.store(false);
  }
  socket.configure_stream();
  out = std::move(socket);
  return Status::success();
}

}  // namespace tef
