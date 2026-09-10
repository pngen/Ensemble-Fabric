#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "ensemble_fabric/limits.hpp"
#include "ensemble_fabric/outcome.hpp"
#include "ensemble_fabric/persistence.hpp"
#include "ensemble_fabric/protocol.hpp"

namespace ensemble_fabric {

/// Endpoint address for the reference deployment.  Only loopback transports are
/// used; the runtime is vendor-neutral and does not assume a specific fabric.
struct Endpoint {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};

  [[nodiscard]] std::string to_string() const;
};

/// Owning socket handle.  Move-only; closes deterministically in the destructor
/// so that a process death test never leaks descriptors.
class Socket {
 public:
  Socket() noexcept = default;
  explicit Socket(std::uintptr_t handle) noexcept;
  ~Socket();

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uintptr_t handle() const noexcept { return handle_; }

  Status close() noexcept;
  Status shutdown_both() noexcept;

  /// Sends the whole buffer, retrying short writes.  Never called while holding
  /// runtime state locks.
  [[nodiscard]] Status send_all(std::span<const std::byte> data);
  /// Receives at least one byte.  A clean peer close is reported as
  /// transport_error with a stable message.
  [[nodiscard]] Outcome<std::size_t> receive_some(std::span<std::byte> buffer);

 private:
  std::uintptr_t handle_{kInvalid};
  static constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
};

/// Initializes and tears down the platform networking layer exactly once per
/// process.  On Windows this is WSAStartup; on POSIX it is a no-op.
class NetworkRuntime {
 public:
  NetworkRuntime();
  ~NetworkRuntime();
  NetworkRuntime(const NetworkRuntime&) = delete;
  NetworkRuntime& operator=(const NetworkRuntime&) = delete;
  [[nodiscard]] bool ready() const noexcept { return ready_; }

 private:
  bool ready_{false};
};

/// Connects to a loopback endpoint.
[[nodiscard]] Outcome<Socket> connect_to(const Endpoint& endpoint);

/// Listens on a loopback port.  Port 0 requests an ephemeral port; the chosen
/// port is reported through p bound_port.
[[nodiscard]] Outcome<Socket> listen_on(const Endpoint& endpoint, std::uint16_t& bound_port);

/// Accepts one connection.
[[nodiscard]] Outcome<Socket> accept_one(const Socket& listener);

/// A framed connection.  Concurrent sends are serialized by an internal write
/// mutex; the runtime never holds ensemble state locks while sending.
class FrameChannel {
 public:
  explicit FrameChannel(Socket socket, Limits limits = Limits::defaults(), bool owns_socket = true);

  [[nodiscard]] Status send(const Frame& frame);
  /// Blocks until one complete frame is decoded.  Returns a typed failure on
  /// malformed input or closed connection.
  [[nodiscard]] Outcome<Frame> receive();

  [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }
  void close() noexcept;
  [[nodiscard]] std::uint64_t frames_sent() const noexcept { return frames_sent_; }
  [[nodiscard]] std::uint64_t frames_received() const noexcept { return frames_received_; }
  [[nodiscard]] const Limits& limits() const noexcept { return limits_; }

 private:
  Socket socket_;
  Limits limits_;
  std::mutex write_mutex_;
  std::vector<std::byte> inbound_;
  std::uint64_t frames_sent_{0};
  std::uint64_t frames_received_{0};
  bool closed_{false};
};

}  // namespace ensemble_fabric
