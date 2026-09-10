#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "ensemble_fabric/limits.hpp"
#include "ensemble_fabric/outcome.hpp"
#include "ensemble_fabric/participant.hpp"
#include "ensemble_fabric/reference_backend.hpp"
#include "ensemble_fabric/transport.hpp"

namespace ensemble_fabric {

/// Declares that one logical participant of one ensemble is served by this
/// worker process.
struct ParticipantBinding {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  ParticipantId participant;
  RoleSet roles;
  std::string label;
};

struct WorkerConfig {
  Endpoint coordinator;
  WorkerId worker;
  /// Fresh boot identity for this process incarnation.  A restarted worker must
  /// never reuse a previous boot identity.
  WorkerBootId worker_boot;
  std::string label;
  CompatibilityProfile profile;
  std::vector<ParticipantBinding> bindings;
  Limits limits{};
};

/// A participant process.
///
/// The worker owns no ensemble authority: it may only act on work the
/// coordinator dispatches under its current boot identity and generation.
class EnsembleWorker {
 public:
  EnsembleWorker(WorkerConfig config, std::shared_ptr<ParticipantBackend> backend);
  ~EnsembleWorker();

  EnsembleWorker(const EnsembleWorker&) = delete;
  EnsembleWorker& operator=(const EnsembleWorker&) = delete;

  /// Connects, performs the handshake, and registers every binding.
  [[nodiscard]] Status connect_and_register();

  /// Serves requests until stopped or the coordinator disconnects.  Blocking.
  [[nodiscard]] Status serve();

  void request_stop();

  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept {
    return coordinator_epoch_;
  }
  [[nodiscard]] const WorkerBootId& worker_boot() const noexcept { return config_.worker_boot; }
  [[nodiscard]] bool registered() const noexcept { return registered_; }
  [[nodiscard]] std::uint64_t handled_requests() const noexcept { return handled_.load(); }

 private:
  void handle_frame(const Frame& frame);

  [[nodiscard]] Sequence next_sequence() noexcept {
    sequence_ = Sequence(sequence_.value() + 1u);
    return sequence_;
  }

  WorkerConfig config_;
  std::shared_ptr<ParticipantBackend> backend_;
  std::unique_ptr<FrameChannel> channel_;
  CoordinatorEpoch coordinator_epoch_;
  WorkerBootId coordinator_boot_;
  Sequence sequence_;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> handled_{0};
  bool registered_{false};
  /// Candidates this worker has been told to stop working on.  The coordinator
  /// remains the authority that fences late output.
  std::unordered_set<std::uint64_t> cancelled_;
  std::mutex send_mutex_;
};

/// Generates a fresh process-incarnation boot identity from the operating
/// system's process identity, a monotonic timestamp, and a random draw.  Never
/// returns zero.
[[nodiscard]] WorkerBootId generate_worker_boot_id() noexcept;

}  // namespace ensemble_fabric
