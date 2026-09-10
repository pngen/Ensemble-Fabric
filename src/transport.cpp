#include "ensemble_fabric/transport.hpp"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace ensemble_fabric {
namespace {

#if defined(_WIN32)
using native_socket = SOCKET;
inline constexpr native_socket kInvalidSocket = INVALID_SOCKET;

[[nodiscard]] Status last_socket_error(std::string_view what) {
  const int code = WSAGetLastError();
  return fail(EnsembleError::transport_error,
              std::string(what) + " failed with winsock error " + std::to_string(code));
}

void close_native(native_socket handle) noexcept {
  if (handle != kInvalidSocket) {
    ::shutdown(handle, SD_BOTH);
    ::closesocket(handle);
  }
}

void shutdown_native(native_socket handle) noexcept {
  if (handle != kInvalidSocket) {
    ::shutdown(handle, SD_BOTH);
  }
}
#else
using native_socket = int;
inline constexpr native_socket kInvalidSocket = -1;

[[nodiscard]] Status last_socket_error(std::string_view what) {
  return fail(EnsembleError::transport_error, std::string(what) + " failed");
}

void close_native(native_socket handle) noexcept {
  if (handle != kInvalidSocket) {
    ::shutdown(handle, SHUT_RDWR);
    ::close(handle);
  }
}

void shutdown_native(native_socket handle) noexcept {
  if (handle != kInvalidSocket) {
    ::shutdown(handle, SHUT_RDWR);
  }
}
#endif

/// Disables Nagle batching: the reference deployment is latency-sensitive and
/// message-oriented, so a small frame must not wait for more data.
void configure_socket(native_socket handle) noexcept {
  const int enabled = 1;
  ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&enabled), static_cast<socklen_t>(sizeof(enabled)));
#if defined(SO_REUSEADDR)
  const int reuse = 1;
  ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               static_cast<socklen_t>(sizeof(reuse)));
#endif
}

}  // namespace

std::string Endpoint::to_string() const {
  return host + ":" + std::to_string(port);
}

Socket::Socket(std::uintptr_t handle) noexcept : handle_(handle) {}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) {
  other.handle_ = kInvalid;
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalid;
  }
  return *this;
}

bool Socket::valid() const noexcept {
#if defined(_WIN32)
  return handle_ != kInvalid && handle_ != static_cast<std::uintptr_t>(INVALID_SOCKET);
#else
  return handle_ != kInvalid;
#endif
}

Status Socket::close() noexcept {
  if (valid()) {
    close_native(static_cast<native_socket>(handle_));
    handle_ = kInvalid;
  }
  return ok_status();
}

Status Socket::shutdown_both() noexcept {
  if (valid()) {
    shutdown_native(static_cast<native_socket>(handle_));
  }
  return ok_status();
}

Status Socket::send_all(std::span<const std::byte> data) {
  if (!valid()) {
    return fail(EnsembleError::transport_error, "cannot send on a closed socket");
  }
  std::size_t sent = 0;
  while (sent < data.size()) {
    const std::size_t remaining = data.size() - sent;
    const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 1u << 20));
    const int written = ::send(static_cast<native_socket>(handle_),
                               reinterpret_cast<const char*>(data.data() + sent), chunk, 0);
    if (written <= 0) {
      return last_socket_error("send");
    }
    sent += static_cast<std::size_t>(written);
  }
  return ok_status();
}

Outcome<std::size_t> Socket::receive_some(std::span<std::byte> buffer) {
  if (!valid()) {
    return fail<std::size_t>(EnsembleError::transport_error, "cannot receive on a closed socket");
  }
  if (buffer.empty()) {
    return fail<std::size_t>(EnsembleError::invalid_argument, "receive buffer is empty");
  }
  const int received = ::recv(static_cast<native_socket>(handle_),
                              reinterpret_cast<char*>(buffer.data()),
                              static_cast<int>(std::min<std::size_t>(buffer.size(), 1u << 20)), 0);
  if (received == 0) {
    return fail<std::size_t>(EnsembleError::transport_error, "the peer closed the connection");
  }
  if (received < 0) {
    return fail<std::size_t>(last_socket_error("recv").code(),
                             last_socket_error("recv").error().message);
  }
  return static_cast<std::size_t>(received);
}

NetworkRuntime::NetworkRuntime() {
#if defined(_WIN32)
  WSADATA data{};
  const int result = WSAStartup(MAKEWORD(2, 2), &data);
  ready_ = result == 0;
#else
  ready_ = true;
#endif
}

NetworkRuntime::~NetworkRuntime() {
#if defined(_WIN32)
  if (ready_) {
    WSACleanup();
  }
#endif
}

namespace {

/// Winsock must stay initialized for the whole process lifetime: tearing it
/// down while a socket is still open would silently break later operations on
/// other threads.  One process-lifetime instance owns that initialization.
[[nodiscard]] NetworkRuntime& process_network_runtime() {
  static NetworkRuntime runtime;
  return runtime;
}

}  // namespace

Outcome<Socket> connect_to(const Endpoint& endpoint) {
  NetworkRuntime& runtime = process_network_runtime();
  if (!runtime.ready()) {
    return fail<Socket>(EnsembleError::transport_error, "the networking layer failed to initialize");
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string port = std::to_string(endpoint.port);
  if (::getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &results) != 0 || results == nullptr) {
    return fail<Socket>(EnsembleError::transport_error,
                        "cannot resolve the endpoint " + endpoint.to_string());
  }
  Socket socket;
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    const native_socket handle =
        ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (handle == kInvalidSocket) {
      continue;
    }
    configure_socket(handle);
    if (::connect(handle, candidate->ai_addr, static_cast<socklen_t>(candidate->ai_addrlen)) == 0) {
      socket = Socket(static_cast<std::uintptr_t>(handle));
      break;
    }
    close_native(handle);
  }
  ::freeaddrinfo(results);
  if (!socket.valid()) {
    return fail<Socket>(EnsembleError::transport_error,
                        "cannot connect to " + endpoint.to_string());
  }
  return socket;
}

Outcome<Socket> listen_on(const Endpoint& endpoint, std::uint16_t& bound_port) {
  NetworkRuntime& runtime = process_network_runtime();
  if (!runtime.ready()) {
    return fail<Socket>(EnsembleError::transport_error, "the networking layer failed to initialize");
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* results = nullptr;
  const std::string port = std::to_string(endpoint.port);
  if (::getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &results) != 0 || results == nullptr) {
    return fail<Socket>(EnsembleError::transport_error,
                        "cannot resolve the bind endpoint " + endpoint.to_string());
  }
  native_socket handle = kInvalidSocket;
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    handle = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (handle == kInvalidSocket) {
      continue;
    }
    configure_socket(handle);
    if (::bind(handle, candidate->ai_addr, static_cast<socklen_t>(candidate->ai_addrlen)) == 0 &&
        ::listen(handle, SOMAXCONN) == 0) {
      break;
    }
    close_native(handle);
    handle = kInvalidSocket;
  }
  ::freeaddrinfo(results);
  if (handle == kInvalidSocket) {
    return fail<Socket>(EnsembleError::transport_error,
                        "cannot bind and listen on " + endpoint.to_string());
  }
  sockaddr_in address{};
  socklen_t length = static_cast<socklen_t>(sizeof(address));
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
    bound_port = ntohs(address.sin_port);
  } else {
    bound_port = endpoint.port;
  }
  return Socket(static_cast<std::uintptr_t>(handle));
}

Outcome<Socket> accept_one(const Socket& listener) {
  if (!listener.valid()) {
    return fail<Socket>(EnsembleError::transport_error, "the listener is closed");
  }
  sockaddr_in address{};
  socklen_t length = static_cast<socklen_t>(sizeof(address));
  const native_socket handle = ::accept(static_cast<native_socket>(listener.handle()),
                                        reinterpret_cast<sockaddr*>(&address), &length);
  if (handle == kInvalidSocket) {
    return fail<Socket>(EnsembleError::transport_error, "accept failed");
  }
  configure_socket(handle);
  return Socket(static_cast<std::uintptr_t>(handle));
}

FrameChannel::FrameChannel(Socket socket, Limits limits, bool owns_socket)
    : limits_(limits) {
  if (owns_socket) {
    socket_ = std::move(socket);
  } else {
    socket_ = std::move(socket);
  }
  inbound_.reserve(4096);
}

Status FrameChannel::send(const Frame& frame) {
  Outcome<std::vector<std::byte>> encoded = encode_frame(frame, limits_);
  if (!encoded.ok()) {
    return fail(encoded.code(), encoded.error().message);
  }
  // Concurrent senders are serialized here; ensemble state locks are never held
  // while this executes.
  const std::lock_guard<std::mutex> guard(write_mutex_);
  if (closed_) {
    return fail(EnsembleError::transport_error, "the channel is closed");
  }
  const Status status = socket_.send_all(
      std::span<const std::byte>(encoded.value().data(), encoded.value().size()));
  if (status.ok()) {
    ++frames_sent_;
  }
  return status;
}

Outcome<Frame> FrameChannel::receive() {
  std::vector<std::byte> scratch(16u << 10);
  for (;;) {
    const DecodeResult decoded =
        decode_frame(std::span<const std::byte>(inbound_.data(), inbound_.size()), limits_);
    if (decoded.status == DecodeStatus::complete) {
      inbound_.erase(inbound_.begin(),
                     inbound_.begin() + static_cast<std::ptrdiff_t>(decoded.consumed));
      ++frames_received_;
      return decoded.frame;
    }
    if (decoded.status == DecodeStatus::failed) {
      return fail<Frame>(decoded.error.code, decoded.error.message);
    }
    if (inbound_.size() > static_cast<std::size_t>(limits_.max_frame_bytes)) {
      return fail<Frame>(EnsembleError::payload_too_large,
                         "inbound buffer exceeded the maximum frame size");
    }
    Outcome<std::size_t> received =
        socket_.receive_some(std::span<std::byte>(scratch.data(), scratch.size()));
    if (!received.ok()) {
      return fail<Frame>(received.code(), received.error().message);
    }
    inbound_.insert(inbound_.end(), scratch.begin(),
                    scratch.begin() + static_cast<std::ptrdiff_t>(received.value()));
  }
}

void FrameChannel::close() noexcept {
  {
    const std::lock_guard<std::mutex> guard(write_mutex_);
    closed_ = true;
  }
  socket_.shutdown_both();
  socket_.close();
}

}  // namespace ensemble_fabric
