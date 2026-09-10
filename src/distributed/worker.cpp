#include "ensemble_fabric/worker.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <unordered_set>
#include <utility>

#include "ensemble_fabric/version.hpp"

#include "detail/text.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace ensemble_fabric {
namespace {

/// Maps a backend failure onto the typed participant failure class the wire
/// protocol carries.  The mapping is total and explicit.
[[nodiscard]] ParticipantFailureKind failure_kind_for(EnsembleError code) noexcept {
  switch (code) {
    case EnsembleError::participant_unavailable: return ParticipantFailureKind::unavailable;
    case EnsembleError::transport_error: return ParticipantFailureKind::transport_error;
    case EnsembleError::malformed_candidate: return ParticipantFailureKind::malformed_output;
    case EnsembleError::malformed_evaluation: return ParticipantFailureKind::malformed_output;
    default: return ParticipantFailureKind::internal_error;
  }
}

[[nodiscard]] std::uint64_t process_identity() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

}  // namespace

WorkerBootId generate_worker_boot_id() noexcept {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t serial = counter.fetch_add(1u) + 1u;
  const std::uint64_t process = process_identity();
  const std::uint64_t ticks = static_cast<std::uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
  std::uint64_t entropy = 0x9E3779B97F4A7C15ull;
  try {
    std::random_device device;
    entropy = (static_cast<std::uint64_t>(device()) << 32u) | static_cast<std::uint64_t>(device());
  } catch (...) {
    entropy = 0x9E3779B97F4A7C15ull;
  }
  std::uint64_t value = ticks ^ (process * 0x9E3779B97F4A7C15ull) ^
                        (serial * 0xBF58476D1CE4E5B9ull) ^ entropy;
  if (value == 0u) {
    value = 1u;
  }
  return WorkerBootId(value);
}

EnsembleWorker::EnsembleWorker(WorkerConfig config, std::shared_ptr<ParticipantBackend> backend)
    : config_(std::move(config)), backend_(std::move(backend)) {
  if (!config_.worker.valid()) {
    const std::uint64_t identity = process_identity();
    config_.worker = WorkerId(identity == 0u ? 1u : identity);
  }
  if (!config_.worker_boot.valid()) {
    config_.worker_boot = generate_worker_boot_id();
  }
}

EnsembleWorker::~EnsembleWorker() = default;

Status EnsembleWorker::connect_and_register() {
  Outcome<Socket> socket = connect_to(config_.coordinator);
  if (!socket.ok()) {
    return fail(socket.code(), socket.error().message);
  }
  channel_ = std::make_unique<FrameChannel>(std::move(socket).value(), config_.limits);

  HelloMessage hello;
  hello.worker = config_.worker;
  hello.worker_boot = config_.worker_boot;
  hello.coordinator_epoch = CoordinatorEpoch(0);
  hello.profile = config_.profile;
  hello.label = config_.label;
  Encoder payload(config_.limits, 256u);
  encode(hello, payload);
  Frame frame;
  frame.type = MessageType::hello;
  frame.correlation = RequestId(next_sequence().value());
  frame.worker_boot = config_.worker_boot;
  frame.sequence = sequence_;
  frame.payload = payload.take();
  const Status sent = channel_->send(frame);
  if (!sent.ok()) {
    return sent;
  }
  Outcome<Frame> response = channel_->receive();
  if (!response.ok()) {
    return fail(response.code(), response.error().message);
  }
  if (response.value().type != MessageType::hello_ack) {
    return fail(EnsembleError::protocol_error, "the coordinator did not acknowledge the handshake");
  }
  Decoder decoder(std::span<const std::byte>(response.value().payload.data(),
                                             response.value().payload.size()),
                  config_.limits);
  Outcome<CoordinatorAck> ack = decode_coordinator_ack(decoder);
  if (!ack.ok()) {
    return fail(ack.code(), ack.error().message);
  }
  if (!ack.value().accepted) {
    return fail(static_cast<EnsembleError>(ack.value().status_code),
                "the coordinator rejected the handshake: " + ack.value().detail);
  }
  coordinator_epoch_ = ack.value().coordinator_epoch;
  coordinator_boot_ = ack.value().coordinator_boot;

  for (const ParticipantBinding& binding : config_.bindings) {
    RegisterParticipantMessage registration;
    registration.registration.ensemble = binding.ensemble;
    registration.registration.ensemble_generation = binding.ensemble_generation;
    registration.registration.participant = binding.participant;
    registration.registration.worker = config_.worker;
    registration.registration.worker_boot = config_.worker_boot;
    registration.registration.coordinator_epoch = coordinator_epoch_;
    registration.registration.profile = config_.profile;
    registration.registration.claimed_roles = binding.roles;
    Encoder encoded(config_.limits, 256u);
    encode(registration, encoded);
    Frame request;
    request.type = MessageType::register_participant;
    request.correlation = RequestId(next_sequence().value());
    request.epoch = coordinator_epoch_;
    request.worker_boot = config_.worker_boot;
    request.sequence = sequence_;
    request.payload = encoded.take();
    const Status registration_sent = channel_->send(request);
    if (!registration_sent.ok()) {
      return registration_sent;
    }
    Outcome<Frame> reply = channel_->receive();
    if (!reply.ok()) {
      return fail(reply.code(), reply.error().message);
    }
    if (reply.value().type != MessageType::register_ack) {
      return fail(EnsembleError::protocol_error,
                  "the coordinator did not acknowledge a participant registration");
    }
    Decoder reply_decoder(std::span<const std::byte>(reply.value().payload.data(),
                                                     reply.value().payload.size()),
                          config_.limits);
    Outcome<RegistrationAck> registration_ack = decode_registration_ack(reply_decoder);
    if (!registration_ack.ok()) {
      return fail(registration_ack.code(), registration_ack.error().message);
    }
    if (!registration_ack.value().accepted) {
      return fail(registration_ack.value().code, registration_ack.value().detail);
    }
  }
  registered_ = true;
  return ok_status();
}

void EnsembleWorker::request_stop() {
  stop_.store(true);
  if (channel_ != nullptr) {
    channel_->close();
  }
}

void EnsembleWorker::handle_frame(const Frame& frame) {
  switch (frame.type) {
    case MessageType::dispatch_candidate: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()),
                      config_.limits);
      Outcome<DispatchCandidateMessage> request = decode_dispatch_candidate(decoder);
      if (!request.ok()) {
        return;
      }
      const Status exhausted = decoder.require_exhausted("candidate dispatch");
      if (!exhausted.ok()) {
        return;
      }
      const DispatchCandidateMessage& dispatch = request.value();
      const ParticipantAuthority authority = [&dispatch, this]() {
        ParticipantAuthority value;
        value.ensemble = dispatch.ensemble;
        value.ensemble_generation = dispatch.ensemble_generation;
        value.participant = dispatch.participant;
        value.participant_generation = dispatch.participant_generation;
        value.worker = config_.worker;
        value.worker_boot = config_.worker_boot;
        value.coordinator_epoch = coordinator_epoch_;
        return value;
      }();
      Outcome<ParticipantProduction> production = backend_->produce(dispatch);
      Encoder encoded(config_.limits, 512u);
      Frame outbound;
      outbound.correlation = frame.correlation;
      outbound.epoch = coordinator_epoch_;
      outbound.worker_boot = config_.worker_boot;
      outbound.sequence = next_sequence();
      if (!production.ok() || production.value().kind == ParticipantProduction::Kind::failure) {
        CandidateFailureMessage failure;
        failure.authority.participant = authority;
        failure.authority.execution = dispatch.execution;
        failure.authority.attempt_generation = dispatch.attempt_generation;
        failure.authority.candidate = dispatch.candidate;
        failure.authority.candidate_generation = dispatch.candidate_generation;
        if (production.ok()) {
          failure.kind = production.value().failure;
          failure.detail = production.value().detail;
        } else {
          failure.kind = failure_kind_for(production.code());
          failure.detail = production.error().message;
        }
        encode(failure, encoded);
        outbound.type = MessageType::candidate_failure;
      } else if (production.value().kind == ParticipantProduction::Kind::abstention) {
        CandidateAbstentionMessage abstention;
        abstention.authority.participant = authority;
        abstention.authority.execution = dispatch.execution;
        abstention.authority.attempt_generation = dispatch.attempt_generation;
        abstention.authority.candidate = dispatch.candidate;
        abstention.authority.candidate_generation = dispatch.candidate_generation;
        abstention.rationale = production.value().detail;
        encode(abstention, encoded);
        outbound.type = MessageType::candidate_abstention;
      } else {
        CandidateResultMessage result;
        result.authority.participant = authority;
        result.authority.execution = dispatch.execution;
        result.authority.attempt_generation = dispatch.attempt_generation;
        result.authority.candidate = dispatch.candidate;
        result.authority.candidate_generation = dispatch.candidate_generation;
        result.payload = production.value().payload;
        result.produced_units = production.value().budget_units_used;
        encode(result, encoded);
        outbound.type = MessageType::candidate_result;
      }
      outbound.payload = encoded.take();
      (void)channel_->send(outbound);
      ++handled_;
      return;
    }
    case MessageType::evaluation_request: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()),
                      config_.limits);
      Outcome<EvaluationRequestMessage> request = decode_evaluation_request(decoder);
      if (!request.ok()) {
        return;
      }
      const Status exhausted = decoder.require_exhausted("evaluation request");
      if (!exhausted.ok()) {
        return;
      }
      const EvaluationRequestMessage& evaluation_request = request.value();
      Outcome<ParticipantEvaluation> evaluation = backend_->evaluate(evaluation_request);
      if (!evaluation.ok()) {
        return;
      }
      EvaluationResultMessage message;
      message.authority.evaluator.ensemble = evaluation_request.ensemble;
      message.authority.evaluator.ensemble_generation = evaluation_request.ensemble_generation;
      message.authority.evaluator.participant = evaluation_request.judge;
      message.authority.evaluator.participant_generation = evaluation_request.judge_generation;
      message.authority.evaluator.worker = config_.worker;
      message.authority.evaluator.worker_boot = config_.worker_boot;
      message.authority.evaluator.coordinator_epoch = coordinator_epoch_;
      message.authority.execution = evaluation_request.execution;
      message.authority.attempt_generation = evaluation_request.attempt_generation;
      message.authority.evaluation = evaluation_request.evaluation;
      message.authority.evaluation_generation = evaluation_request.evaluation_generation;
      message.authority.candidate = evaluation_request.candidate;
      message.authority.candidate_generation = evaluation_request.candidate_generation;
      message.criterion = evaluation_request.criterion;
      message.criterion_kind = evaluation_request.criterion_kind;
      message.verification = evaluation.value().verification;
      message.judgment = evaluation.value().judgment;
      message.score_defined = evaluation.value().score_defined;
      message.score = evaluation.value().score;
      message.weight = evaluation.value().weight;
      message.evidence = evaluation.value().evidence;
      message.rationale = evaluation.value().rationale;
      Encoder encoded(config_.limits, 512u + message.evidence.size());
      encode(message, encoded);
      Frame outbound;
      outbound.type = MessageType::evaluation_result;
      outbound.correlation = frame.correlation;
      outbound.epoch = coordinator_epoch_;
      outbound.worker_boot = config_.worker_boot;
      outbound.sequence = next_sequence();
      outbound.payload = encoded.take();
      (void)channel_->send(outbound);
      ++handled_;
      return;
    }
    case MessageType::cancel: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()),
                      config_.limits);
      Outcome<CancelMessage> cancel = decode_cancel(decoder);
      if (!cancel.ok()) {
        return;
      }
      // The worker records the fence locally.  The coordinator remains the
      // authority that rejects late output, so a publication that races this
      // message is still fenced there.
      cancelled_.insert(cancel.value().candidate.value());
      ++handled_;
      return;
    }
    case MessageType::goodbye: {
      stop_.store(true);
      return;
    }
    case MessageType::error_response:
    case MessageType::heartbeat:
    case MessageType::hello_ack:
    default:
      return;
  }
}

Status EnsembleWorker::serve() {
  if (channel_ == nullptr) {
    return fail(EnsembleError::transport_error, "the worker is not connected");
  }
  while (!stop_.load()) {
    Outcome<Frame> frame = channel_->receive();
    if (!frame.ok()) {
      if (stop_.load()) {
        return ok_status();
      }
      return fail(frame.code(), frame.error().message);
    }
    handle_frame(frame.value());
  }
  return ok_status();
}

}  // namespace ensemble_fabric
