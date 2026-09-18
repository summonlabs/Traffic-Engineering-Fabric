// Traffic Engineering Fabric - real TCP framed transport.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "tef/diagnostic.hpp"
#include "tef/limits.hpp"

namespace tef {

// Wire framing:
//   magic        4 bytes 'T','E','F','1'
//   version      u16   (big endian)
//   type         u16
//   flags        u32
//   payload_len  u32   (bounded by Limits::max_frame_payload)
//   payload_crc  u32   CRC-32C over the payload bytes
//   payload      payload_len bytes
inline constexpr std::size_t kFrameHeaderSize = 20;
inline constexpr std::uint32_t kFrameMagic = 0x54454631u;  // 'T','E','F','1'

struct Frame {
  std::uint16_t version = 0;
  std::uint16_t type = 0;
  std::uint32_t flags = 0;
  std::vector<std::byte> payload;

  bool operator==(const Frame& other) const noexcept {
    return version == other.version && type == other.type && flags == other.flags &&
           payload == other.payload;
  }
};

// Encodes a frame to its canonical wire representation.
std::vector<std::byte> encode_frame(const Frame& frame);
// Decodes a header. Returns false for a bad magic, unsupported version, or an
// oversized length; the connection must then be closed.
bool decode_frame_header(std::span<const std::byte> header, Frame& frame, std::string& error);

void net_startup();
void net_shutdown();
std::string last_net_error();

// Blocking socket with exact-length reads and a bounded payload size.
class TcpSocket {
 public:
  TcpSocket();
  ~TcpSocket();
  TcpSocket(TcpSocket&& other) noexcept;
  TcpSocket& operator=(TcpSocket&& other) noexcept;
  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;

  bool valid() const noexcept;
  void close();
  std::uint16_t peer_port() const noexcept;

  // Reads exactly one frame. Returns false on EOF, timeout, framing violation or
  // I/O error. A framing violation is fatal for the connection.
  bool read_frame(Frame& frame, std::string& error);
  bool write_frame(const Frame& frame, std::string& error);

  // Writes raw bytes with no framing. Exposed so that adversarial tests and
  // protocol diagnostics can emit deliberately malformed input on a real
  // socket; the library itself never uses it for authoritative traffic.
  bool write_raw(std::span<const std::byte> bytes, std::string& error);

  // Enables linger-free close and TCP_NODELAY.
  void configure_stream();

  // Idle poll interval used by server loops so that shutdown is prompt without
  // relying on read timeouts for correctness.
  void set_poll_interval_ms(int milliseconds);
  int poll_interval_ms() const noexcept;

  // Interrupts a blocking read from another thread.
  void interrupt();

 private:
  friend class TcpListener;
  friend Status connect_tcp(const std::string& address, std::uint16_t port, TcpSocket& out,
                            std::string& error);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class TcpListener {
 public:
  TcpListener();
  ~TcpListener();
  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  // Binds to the given address. Port 0 selects an ephemeral port; the selected
  // port is reported by local_port().
  Status listen(const std::string& address, std::uint16_t port, int backlog = 16);
  void close();
  bool valid() const noexcept;
  std::uint16_t local_port() const noexcept;
  std::string local_address() const;

  // Waits up to poll_interval_ms for a pending connection.
  bool accept(TcpSocket& out, std::string& error);
  void set_poll_interval_ms(int milliseconds);
  void interrupt();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Connects with an explicit, bounded connect attempt.
Status connect_tcp(const std::string& address, std::uint16_t port, TcpSocket& out,
                   std::string& error);

}  // namespace tef
