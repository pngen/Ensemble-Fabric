#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ensemble_fabric/fabric.hpp"
#include "ensemble_fabric/limits.hpp"
#include "ensemble_fabric/outcome.hpp"
#include "ensemble_fabric/transport.hpp"

namespace ensemble_fabric {

struct CoordinatorConfig {
  Endpoint bind;
  Limits limits{};
  /// Durable state file.  Empty disables persistence.
  std::string state_path;
  bool persist_on_change{true};
  WorkerBootId coordinator_boot;
  std::uint32_t max_connections{256};
  /// Writes the bound port to this file once listening.  Used by the
  /// multi-process reference deployment and its tests.
  std::string port_file;
};

/// Distributed reference coordinator.
///
/// The coordinator owns a single event loop.  Reader threads decode frames and
/// enqueue events; the loop is the only place ensemble state is mutated; and all
/// sends happen outside the runtime's state lock.
class EnsembleCoordinator {
 public:
  explicit EnsembleCoordinator(CoordinatorConfig config);
  ~EnsembleCoordinator();

  EnsembleCoordinator(const EnsembleCoordinator&) = delete;
  EnsembleCoordinator& operator=(const EnsembleCoordinator&) = delete;

  /// Recovers durable state (when configured), binds the listener, and starts
  /// the accept thread, the event loop, and the persistence worker.
  [[nodiscard]] Status start();

  /// Stops the event loop and every connection.  Safe to call twice.
  [[nodiscard]] Status stop();

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] EnsembleFabric& fabric() noexcept { return *fabric_; }

  /// One connected participant process.  Declared here, defined inside the
  /// library, so that reader threads and the event loop can share it without
  /// exposing transport internals.
  struct Connection;

  /// One decoded inbound event, or a connection-loss notification.
  struct InboundEvent {
    std::shared_ptr<Connection> connection;
    Frame frame;
    bool connection_closed{false};
  };

 private:
  void accept_loop();
  void reader_loop(std::shared_ptr<Connection> connection);
  void event_loop();

  void handle_frame(std::shared_ptr<Connection> connection, const Frame& frame);
  void handle_connection_loss(const std::shared_ptr<Connection>& connection);
  [[nodiscard]] std::shared_ptr<Connection> find_connection_for(ParticipantId participant) const;
  void pump(EnsembleId ensemble);
  void pump_all();
  void send_frame(const std::shared_ptr<Connection>& connection, const Frame& frame);
  void send_error(const std::shared_ptr<Connection>& connection, RequestId correlation,
                  EnsembleError code, const std::string& detail);
  void flush_pending_finalize();
  void maybe_persist();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  CoordinatorConfig config_;
  std::unique_ptr<EnsembleFabric> fabric_;
  std::unique_ptr<Socket> listener_;
  std::unique_ptr<NetworkRuntime> network_;
  std::uint16_t port_{0};

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<InboundEvent> queue_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Connection>> connections_;
  std::uint64_t next_connection_id_{1};
  bool running_{false};
  bool stopping_{false};

  std::thread accept_thread_;
  std::thread event_thread_;
};

}  // namespace ensemble_fabric
