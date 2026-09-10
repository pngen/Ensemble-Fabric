#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/limits.hpp"
#include "ensemble_fabric/outcome.hpp"
#include "ensemble_fabric/protocol.hpp"
#include "ensemble_fabric/spec.hpp"
#include "ensemble_fabric/transport.hpp"

namespace ensemble_fabric {

/// Controller-side client of a coordinator.
///
/// The client is a control surface, not a source of truth: every decision it
/// observes was made by the coordinator under an explicit epoch.
class EnsembleClient {
 public:
  EnsembleClient() = default;
  ~EnsembleClient();
  EnsembleClient(const EnsembleClient&) = delete;
  EnsembleClient& operator=(const EnsembleClient&) = delete;
  EnsembleClient(EnsembleClient&&) noexcept;
  EnsembleClient& operator=(EnsembleClient&&) noexcept;

  /// Connects and performs the handshake.
  [[nodiscard]] static Outcome<EnsembleClient> connect(const Endpoint& endpoint,
                                                       Limits limits = Limits::defaults());

  [[nodiscard]] Outcome<DefineEnsembleAck> define_ensemble(const EnsembleSpec& spec);
  [[nodiscard]] Outcome<OpenExecutionAck> open_execution(EnsembleId ensemble,
                                                         EnsembleGeneration generation);
  [[nodiscard]] Outcome<FinalizeResponseMessage> finalize(EnsembleId ensemble,
                                                          EnsembleGeneration generation);
  [[nodiscard]] Outcome<ResultResponseMessage> query_result(EnsembleId ensemble,
                                                            EnsembleGeneration generation);
  [[nodiscard]] Outcome<InspectResponseMessage> inspect(EnsembleId ensemble);
  [[nodiscard]] Status cancel_ensemble(EnsembleId ensemble, EnsembleGeneration generation,
                                       std::string reason);

  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept { return coordinator_epoch_; }
  [[nodiscard]] const Limits& limits() const noexcept { return limits_; }
  [[nodiscard]] bool connected() const noexcept { return channel_ != nullptr && channel_->valid(); }
  void close() noexcept;

 private:
  [[nodiscard]] Outcome<Frame> request(MessageType type, std::vector<std::byte> payload);
  [[nodiscard]] RequestId next_correlation() noexcept;

  std::unique_ptr<FrameChannel> channel_;
  IdAllocator<RequestIdTag> sequences_;
  Limits limits_{};
  CoordinatorEpoch coordinator_epoch_;
  WorkerBootId coordinator_boot_;
  Sequence sequence_;
};

/// Serializes an ensemble specification into its canonical wire encoding.
[[nodiscard]] std::vector<std::byte> encode_spec(const EnsembleSpec& spec,
                                                 const Limits& limits);
[[nodiscard]] Outcome<EnsembleSpec> decode_spec(std::span<const std::byte> data,
                                                const Limits& limits);

/// Serializes an ensemble result and an inspection snapshot for the wire.
[[nodiscard]] std::vector<std::byte> encode_result_payload(const EnsembleResult& result,
                                                           const Limits& limits);
[[nodiscard]] Outcome<EnsembleResult> decode_result_payload(std::span<const std::byte> data,
                                                            const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_report(const std::string& text,
                                                   const Limits& limits);
[[nodiscard]] Outcome<std::string> decode_report(std::span<const std::byte> data,
                                                 const Limits& limits);

}  // namespace ensemble_fabric
