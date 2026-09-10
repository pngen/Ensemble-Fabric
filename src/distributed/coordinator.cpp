#include "ensemble_fabric/coordinator.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <utility>

#include "ensemble_fabric/client.hpp"
#include "ensemble_fabric/version.hpp"
#include "ensemble_fabric/worker.hpp"

#include "detail/text.hpp"
#include "fabric_internal.hpp"

namespace ensemble_fabric {

/// One participant process connected to the coordinator.
struct EnsembleCoordinator::Connection {
  std::uint64_t id{0};
  std::shared_ptr<FrameChannel> channel;
  WorkerId worker;
  WorkerBootId worker_boot;
  std::string label;
  bool handshaken{false};
  /// Logical participants this connection registered, with the generation it
  /// holds.  A replacement process registers anew and appears here with a
  /// higher generation.
  std::unordered_map<std::uint64_t, ParticipantGeneration> registrations;
  bool closed{false};
};

namespace {

using Event = EnsembleCoordinator::InboundEvent;

struct PendingFinalize {
  std::shared_ptr<EnsembleCoordinator::Connection> connection;
  RequestId correlation;
  EnsembleId ensemble;
  EnsembleGeneration generation;
};

}  // namespace

struct EnsembleCoordinator::Impl {
  std::mutex pending_mutex;
  std::vector<PendingFinalize> pending_finalizes;
  bool state_dirty{false};
};

EnsembleCoordinator::EnsembleCoordinator(CoordinatorConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {
  FabricConfig fabric_config;
  fabric_config.limits = config_.limits;
  fabric_config.coordinator_boot = config_.coordinator_boot.valid() ? config_.coordinator_boot
                                                                    : generate_worker_boot_id();
  fabric_ = std::make_unique<EnsembleFabric>(fabric_config);
}

EnsembleCoordinator::~EnsembleCoordinator() { (void)stop(); }

CoordinatorEpoch EnsembleCoordinator::epoch() const { return fabric_->epoch(); }

Status EnsembleCoordinator::start() {
  if (!config_.state_path.empty()) {
    std::error_code error;
    if (std::filesystem::exists(config_.state_path, error)) {
      Outcome<CoordinatorEpoch> recovered = fabric_->recover_state(config_.state_path);
      if (!recovered.ok()) {
        return fail(recovered.code(), recovered.error().message);
      }
    }
  }
  network_ = std::make_unique<NetworkRuntime>();
  if (!network_->ready()) {
    return fail(EnsembleError::transport_error, "the networking layer failed to initialize");
  }
  std::uint16_t bound_port = 0;
  Outcome<Socket> listener = listen_on(config_.bind, bound_port);
  if (!listener.ok()) {
    return fail(listener.code(), listener.error().message);
  }
  listener_ = std::make_unique<Socket>(std::move(listener).value());
  port_ = bound_port;

  {
    const std::lock_guard<std::mutex> guard(mutex_);
    running_ = true;
    stopping_ = false;
  }
  if (!config_.port_file.empty()) {
    // The port file is published atomically: a reader that sees the file sees
    // its final content, never a half-written value.
    const std::filesystem::path target(config_.port_file);
    const std::filesystem::path temporary = target.string() + ".tmp";
    {
      std::ofstream stream(temporary, std::ios::trunc);
      stream << static_cast<unsigned>(port_) << "\n";
      stream.flush();
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, target, rename_error);
  }
  // Live process authority never survives a restart: anything recovered from
  // disk requires fresh evidence before it can act again.
  (void)fabric_->invalidate_dynamic_authority("coordinator start");
  event_thread_ = std::thread([this]() { event_loop(); });
  accept_thread_ = std::thread([this]() { accept_loop(); });
  return ok_status();
}

Status EnsembleCoordinator::stop() {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (!running_ && !stopping_) {
      return ok_status();
    }
    stopping_ = true;
    running_ = false;
  }
  condition_.notify_all();
  if (listener_ != nullptr) {
    listener_->close();
  }
  std::vector<std::shared_ptr<Connection>> connections;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    for (auto& entry : connections_) {
      connections.push_back(entry.second);
    }
  }
  for (const auto& connection : connections) {
    if (connection->channel != nullptr) {
      connection->channel->close();
    }
  }
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  if (event_thread_.joinable()) {
    event_thread_.join();
  }
  return ok_status();
}

void EnsembleCoordinator::accept_loop() {
  while (true) {
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      if (!running_) {
        return;
      }
    }
    if (listener_ == nullptr || !listener_->valid()) {
      return;
    }
    Outcome<Socket> accepted = accept_one(*listener_);
    if (!accepted.ok()) {
      const std::lock_guard<std::mutex> guard(mutex_);
      if (!running_) {
        return;
      }
      continue;
    }
    auto connection = std::make_shared<Connection>();
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      if (connections_.size() >= config_.max_connections) {
        accepted.value().close();
        continue;
      }
      connection->id = next_connection_id_++;
      connection->channel = std::make_shared<FrameChannel>(std::move(accepted).value(), config_.limits);
      connections_.emplace(connection->id, connection);
    }
    std::thread([this, connection]() { reader_loop(connection); }).detach();
  }
}

void EnsembleCoordinator::reader_loop(std::shared_ptr<Connection> connection) {
  while (true) {
    Outcome<Frame> frame = connection->channel->receive();
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      if (stopping_) {
        return;
      }
      if (!frame.ok()) {
        connection->closed = true;
        Event event;
        event.connection = connection;
        event.connection_closed = true;
        queue_.push_back(std::move(event));
        condition_.notify_all();
        return;
      }
      Event event;
      event.connection = connection;
      event.frame = std::move(frame).value();
      queue_.push_back(std::move(event));
      condition_.notify_all();
    }
  }
}

void EnsembleCoordinator::event_loop() {
  for (;;) {
    std::vector<Event> events;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this]() { return !queue_.empty() || !running_; });
      if (!running_ && queue_.empty()) {
        return;
      }
      events.assign(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
      queue_.clear();
    }
    for (Event& event : events) {
      if (event.connection_closed) {
        std::vector<ParticipantId> participants;
        {
          const std::lock_guard<std::mutex> guard(mutex_);
          for (const auto& entry : event.connection->registrations) {
            participants.push_back(ParticipantId(entry.first));
          }
          connections_.erase(event.connection->id);
        }
        (void)participants;
        // A dead worker process loses its authority immediately.  The
        // coordinator, not the worker, decides what happens next.
        handle_connection_loss(event.connection);
        continue;
      }
      handle_frame(event.connection, event.frame);
    }
    flush_pending_finalize();
    maybe_persist();
  }
}

void EnsembleCoordinator::handle_connection_loss(const std::shared_ptr<Connection>& connection) {
  const Limits& limits = config_.limits;
  (void)limits;
  // Every logical participant bound to this process incarnation is reported
  // unavailable; the retry and fallback policies decide what happens next.
  std::vector<ParticipantAuthority> authorities;
  for (const auto& entry : connection->registrations) {
    ParticipantAuthority authority;
    authority.participant = ParticipantId(entry.first);
    authority.participant_generation = entry.second;
    authority.worker = connection->worker;
    authority.worker_boot = connection->worker_boot;
    authority.coordinator_epoch = fabric_->epoch();
    authorities.push_back(authority);
  }
  if (authorities.empty()) {
    return;
  }
  std::vector<EnsembleId> ensembles;
  for (const EnsembleSummary& summary : fabric_->list_ensembles()) {
    ensembles.push_back(summary.id);
  }
  for (EnsembleId id : ensembles) {
    Outcome<EnsembleSpec> spec = fabric_->spec(id);
    if (!spec.ok()) {
      continue;
    }
    for (ParticipantAuthority& authority : authorities) {
      authority.ensemble = id;
      authority.ensemble_generation = spec.value().generation;
      (void)fabric_->report_participant_failure(authority, ParticipantFailureKind::unavailable,
                                                "worker process connection closed");
    }
    pump(id);
  }
}

void EnsembleCoordinator::send_frame(const std::shared_ptr<Connection>& connection,
                                     const Frame& frame) {
  if (connection == nullptr || connection->channel == nullptr) {
    return;
  }
  (void)connection->channel->send(frame);
}

void EnsembleCoordinator::send_error(const std::shared_ptr<Connection>& connection,
                                     RequestId correlation, EnsembleError code,
                                     const std::string& detail) {
  ErrorResponseMessage message;
  message.code = code;
  message.correlation = correlation;
  message.detail = detail::sanitize_text(detail, config_.limits.max_metadata_bytes);
  Encoder payload(config_.limits, 256u);
  encode(message, payload);
  Frame frame;
  frame.type = MessageType::error_response;
  frame.correlation = correlation;
  frame.epoch = fabric_->epoch();
  frame.worker_boot = fabric_->coordinator_boot();
  frame.payload = payload.take();
  send_frame(connection, frame);
}

std::shared_ptr<EnsembleCoordinator::Connection> EnsembleCoordinator::find_connection_for(
    ParticipantId participant) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  for (const auto& entry : connections_) {
    if (entry.second->registrations.count(participant.value()) != 0u) {
      return entry.second;
    }
  }
  return nullptr;
}

void EnsembleCoordinator::pump(EnsembleId ensemble) {
  // The runtime produces actions; the coordinator transports them.  The state
  // lock is released before any network operation happens.
  for (int round = 0; round < 8; ++round) {
    Outcome<EnsembleSpec> spec = fabric_->spec(ensemble);
    if (!spec.ok()) {
      return;
    }
    Outcome<std::vector<FabricAction>> actions = fabric_->advance(ensemble, spec.value().generation);
    if (!actions.ok() || actions.value().empty()) {
      return;
    }
    for (const FabricAction& action : actions.value()) {
      if (const auto* dispatch = std::get_if<DispatchAction>(&action)) {
        std::shared_ptr<Connection> connection = find_connection_for(dispatch->participant);
        if (connection == nullptr) {
          ParticipantAuthority authority;
          authority.ensemble = dispatch->execution.ensemble;
          authority.ensemble_generation = dispatch->execution.ensemble_generation;
          authority.participant = dispatch->participant;
          authority.participant_generation = dispatch->participant_generation;
          authority.coordinator_epoch = fabric_->epoch();
          (void)fabric_->report_participant_failure(authority,
                                                    ParticipantFailureKind::unavailable,
                                                    "no authoritative connection for dispatch");
          continue;
        }
        DispatchCandidateMessage message;
        message.ensemble = dispatch->execution.ensemble;
        message.ensemble_generation = dispatch->execution.ensemble_generation;
        message.execution = dispatch->execution.execution;
        message.attempt_generation = dispatch->execution.attempt_generation;
        message.participant = dispatch->participant;
        message.participant_generation = dispatch->participant_generation;
        message.coordinator_epoch = fabric_->epoch();
        message.candidate = dispatch->candidate;
        message.candidate_generation = dispatch->candidate_generation;
        message.role = dispatch->role;
        message.stage = dispatch->stage;
        message.attempt = dispatch->attempt;
        message.budget_units = dispatch->budget_units;
        message.deterministic_seed = dispatch->deterministic_seed;
        message.domain = dispatch->domain;
        message.request = dispatch->request;
        Encoder payload(config_.limits, 512u + message.request.size());
        encode(message, payload);
        Frame frame;
        frame.type = MessageType::dispatch_candidate;
        frame.correlation = RequestId(dispatch->sequence.value());
        frame.epoch = fabric_->epoch();
        frame.worker_boot = connection->worker_boot;
        frame.sequence = dispatch->sequence;
        frame.payload = payload.take();
        send_frame(connection, frame);
      } else if (const auto* evaluation = std::get_if<EvaluationAction>(&action)) {
        std::shared_ptr<Connection> connection = find_connection_for(evaluation->judge);
        if (connection == nullptr) {
          continue;
        }
        EvaluationRequestMessage message;
        message.ensemble = evaluation->execution.ensemble;
        message.ensemble_generation = evaluation->execution.ensemble_generation;
        message.execution = evaluation->execution.execution;
        message.attempt_generation = evaluation->execution.attempt_generation;
        message.judge = evaluation->judge;
        message.judge_generation = evaluation->judge_generation;
        message.coordinator_epoch = fabric_->epoch();
        message.evaluation = evaluation->evaluation;
        message.evaluation_generation = evaluation->evaluation_generation;
        message.role = evaluation->role;
        message.candidate = evaluation->candidate;
        message.candidate_generation = evaluation->candidate_generation;
        message.criterion = evaluation->criterion;
        message.criterion_kind = evaluation->criterion_kind;
        message.domain = evaluation->domain;
        Encoder payload(config_.limits, 512u);
        encode(message, payload);
        Frame frame;
        frame.type = MessageType::evaluation_request;
        frame.correlation = RequestId(evaluation->sequence.value());
        frame.epoch = fabric_->epoch();
        frame.worker_boot = connection->worker_boot;
        frame.sequence = evaluation->sequence;
        frame.payload = payload.take();
        send_frame(connection, frame);
      } else if (const auto* cancel = std::get_if<CancelAction>(&action)) {
        std::shared_ptr<Connection> connection = find_connection_for(cancel->participant);
        if (connection == nullptr) {
          continue;
        }
        CancelMessage message;
        message.ensemble = cancel->execution.ensemble;
        message.ensemble_generation = cancel->execution.ensemble_generation;
        message.execution = cancel->execution.execution;
        message.attempt_generation = cancel->execution.attempt_generation;
        message.coordinator_epoch = fabric_->epoch();
        message.candidate = cancel->candidate;
        message.candidate_generation = cancel->candidate_generation;
        message.participant = cancel->participant;
        message.participant_generation = cancel->participant_generation;
        message.reason = cancel->reason;
        Encoder payload(config_.limits, 512u);
        encode(message, payload);
        Frame frame;
        frame.type = MessageType::cancel;
        frame.correlation = RequestId(cancel->sequence.value());
        frame.epoch = fabric_->epoch();
        frame.worker_boot = connection->worker_boot;
        frame.sequence = cancel->sequence;
        frame.payload = payload.take();
        send_frame(connection, frame);
      }
      // FallbackAction is informational: the follow-on dispatch of the fallback
      // participant is produced by the same advance() call.
    }
  }
}

void EnsembleCoordinator::pump_all() {
  for (const EnsembleSummary& summary : fabric_->list_ensembles()) {
    pump(summary.id);
  }
}

void EnsembleCoordinator::flush_pending_finalize() {
  std::vector<PendingFinalize> waiters;
  {
    const std::lock_guard<std::mutex> guard(impl_->pending_mutex);
    if (impl_->pending_finalizes.empty()) {
      return;
    }
    waiters.swap(impl_->pending_finalizes);
  }
  std::vector<PendingFinalize> still_waiting;
  for (PendingFinalize& waiter : waiters) {
    Outcome<bool> settled = fabric_->is_settled(waiter.ensemble, waiter.generation);
    if (!settled.ok()) {
      FinalizeResponseMessage response;
      response.ensemble = waiter.ensemble;
      response.ensemble_generation = waiter.generation;
      response.accepted = false;
      response.code = settled.code();
      response.detail = settled.error().message;
      Encoder payload(config_.limits, 256u);
      encode(response, payload);
      Frame frame;
      frame.type = MessageType::finalize_response;
      frame.correlation = waiter.correlation;
      frame.payload = payload.take();
      send_frame(waiter.connection, frame);
      continue;
    }
    if (!settled.value()) {
      still_waiting.push_back(waiter);
      continue;
    }
    Outcome<EnsembleResult> committed = fabric_->finalize(waiter.ensemble, waiter.generation);
    FinalizeResponseMessage response;
    response.ensemble = waiter.ensemble;
    response.ensemble_generation = waiter.generation;
    if (committed.ok()) {
      response.accepted = true;
      response.code = EnsembleError::ok;
      response.decision = committed.value().decision;
      response.encoded_result = encode_result_payload(committed.value(), config_.limits);
    } else {
      response.accepted = false;
      response.code = committed.code();
      response.detail = committed.error().message;
      Outcome<EnsembleResult> current = fabric_->current_result(waiter.ensemble);
      if (current.ok()) {
        response.decision = current.value().decision;
        response.encoded_result = encode_result_payload(current.value(), config_.limits);
      }
    }
    Encoder payload(config_.limits, 512u + response.encoded_result.size());
    encode(response, payload);
    Frame frame;
    frame.type = MessageType::finalize_response;
    frame.correlation = waiter.correlation;
    frame.epoch = fabric_->epoch();
    frame.worker_boot = fabric_->coordinator_boot();
    frame.payload = payload.take();
    send_frame(waiter.connection, frame);
    {
      const std::lock_guard<std::mutex> guard(impl_->pending_mutex);
      impl_->state_dirty = true;
    }
  }
  if (!still_waiting.empty()) {
    const std::lock_guard<std::mutex> guard(impl_->pending_mutex);
    for (PendingFinalize& waiter : still_waiting) {
      impl_->pending_finalizes.push_back(std::move(waiter));
    }
  }
}

void EnsembleCoordinator::handle_frame(std::shared_ptr<Connection> connection,
                                       const Frame& frame) {
  const Limits& limits = config_.limits;
  switch (frame.type) {
    case MessageType::hello: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()), limits);
      Outcome<HelloMessage> hello = decode_hello(decoder);
      if (!hello.ok()) {
        send_error(connection, frame.correlation, hello.code(), hello.error().message);
        return;
      }
      {
        const std::lock_guard<std::mutex> guard(mutex_);
        connection->worker = hello.value().worker;
        connection->worker_boot = hello.value().worker_boot;
        connection->label = detail::sanitize_text(hello.value().label, limits.max_label_bytes);
        connection->handshaken = true;
      }
      CoordinatorAck ack;
      ack.coordinator_epoch = fabric_->epoch();
      ack.coordinator_boot = fabric_->coordinator_boot();
      ack.accepted = true;
      ack.status_code = 0u;
      ack.detail = "handshake accepted";
      Encoder payload(limits, 256u);
      encode(ack, payload);
      Frame response;
      response.type = MessageType::hello_ack;
      response.correlation = frame.correlation;
      response.epoch = ack.coordinator_epoch;
      response.worker_boot = ack.coordinator_boot;
      response.payload = payload.take();
      send_frame(connection, response);
      return;
    }
    case MessageType::register_participant: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()), limits);
      Outcome<RegisterParticipantMessage> registration = decode_register_participant(decoder);
      if (!registration.ok()) {
        send_error(connection, frame.correlation, registration.code(),
                   registration.error().message);
        return;
      }
      const Status exhausted = decoder.require_exhausted("participant registration");
      if (!exhausted.ok()) {
        send_error(connection, frame.correlation, exhausted.code(), exhausted.error().message);
        return;
      }
      // The connection carries the physical incarnation; the message is not
      // trusted to name it.
      // The connection carries the physical incarnation; the message may not
      // name a different one.  The declared epoch is honoured rather than
      // rewritten, so a replayed registration from an older epoch is rejected.
      ParticipantRegistration record = registration.value().registration;
      record.worker = connection->worker;
      record.worker_boot = connection->worker_boot;
      Outcome<ParticipantGeneration> registered = fabric_->register_participant(record);
      RegistrationAck ack;
      ack.coordinator_epoch = fabric_->epoch();
      ack.participant = record.participant;
      if (registered.ok()) {
        ack.accepted = true;
        ack.participant_generation = registered.value();
        ack.detail = "registration accepted";
        const std::lock_guard<std::mutex> guard(mutex_);
        connection->registrations[record.participant.value()] = registered.value();
        {
          const std::lock_guard<std::mutex> pending_guard(impl_->pending_mutex);
          impl_->state_dirty = true;
        }
      } else {
        ack.accepted = false;
        ack.code = registered.code();
        ack.detail = detail::sanitize_text(registered.error().message, limits.max_metadata_bytes);
      }
      Encoder payload(limits, 256u);
      encode(ack, payload);
      Frame response;
      response.type = MessageType::register_ack;
      response.correlation = frame.correlation;
      response.epoch = ack.coordinator_epoch;
      response.payload = payload.take();
      send_frame(connection, response);
      if (registered.ok()) {
        for (const EnsembleSummary& summary : fabric_->list_ensembles()) {
          pump(summary.id);
        }
      }
      return;
    }
    case MessageType::candidate_result: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()), limits);
      Outcome<CandidateResultMessage> message = decode_candidate_result(decoder);
      if (!message.ok()) {
        send_error(connection, frame.correlation, message.code(), message.error().message);
        return;
      }
      const Status exhausted = decoder.require_exhausted("candidate result");
      if (!exhausted.ok()) {
        send_error(connection, frame.correlation, exhausted.code(), exhausted.error().message);
        return;
      }
      CandidateSubmission submission;
      submission.authority = message.value().authority;
      submission.payload = message.value().payload;
      const Status submitted = fabric_->submit_candidate(submission);
      if (!submitted.ok()) {
        // A rejected submission is a typed outcome, not a silent no-op; the
        // worker is told exactly why its evidence was not admitted.
        std::uint64_t digest = 0;
        bool duplicate = false;
        (void)digest;
        (void)duplicate;
        send_error(connection, frame.correlation, submitted.code(), submitted.error().message);
      }
      pump(submission.authority.participant.ensemble);
      return;
    }
    case MessageType::candidate_failure: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()), limits);
      Outcome<CandidateFailureMessage> message = decode_candidate_failure(decoder);
      if (!message.ok()) {
        send_error(connection, frame.correlation, message.code(), message.error().message);
        return;
      }
      const Status exhausted = decoder.require_exhausted("candidate failure");
      if (!exhausted.ok()) {
        send_error(connection, frame.correlation, exhausted.code(), exhausted.error().message);
        return;
      }
      CandidateFailureSubmission submission;
      submission.authority = message.value().authority;
      submission.participant_failure = static_cast<std::uint32_t>(message.value().kind);
      const Status submitted = fabric_->submit_candidate_failure(submission);
      if (!submitted.ok()) {
        send_error(connection, frame.correlation, submitted.code(), submitted.error().message);
      }
      pump(submission.authority.participant.ensemble);
      return;
    }
    case MessageType::candidate_abstention: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()), limits);
      Outcome<CandidateAbstentionMessage> message = decode_candidate_abstention(decoder);
      if (!message.ok()) {
        send_error(connection, frame.correlation, message.code(), message.error().message);
        return;
      }
      const Status exhausted = decoder.require_exhausted("candidate abstention");
      if (!exhausted.ok()) {
        send_error(connection, frame.correlation, exhausted.code(), exhausted.error().message);
        return;
      }
      CandidateAbstentionSubmission submission;
      submission.authority = message.value().authority;
      submission.rationale = message.value().rationale;
      const Status submitted = fabric_->submit_candidate_abstention(submission);
      if (!submitted.ok()) {
        send_error(connection, frame.correlation, submitted.code(), submitted.error().message);
      }
      pump(submission.authority.participant.ensemble);
      return;
    }
    case MessageType::evaluation_result: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()), limits);
      Outcome<EvaluationResultMessage> message = decode_evaluation_result(decoder);
      if (!message.ok()) {
        send_error(connection, frame.correlation, message.code(), message.error().message);
        return;
      }
      const Status exhausted = decoder.require_exhausted("evaluation result");
      if (!exhausted.ok()) {
        send_error(connection, frame.correlation, exhausted.code(), exhausted.error().message);
        return;
      }
      EvaluationSubmission submission;
      submission.authority = message.value().authority;
      submission.criterion = message.value().criterion;
      submission.kind = message.value().criterion_kind;
      submission.verification = message.value().verification;
      submission.judgment = message.value().judgment;
      submission.score_defined = message.value().score_defined;
      submission.score = message.value().score;
      submission.weight = message.value().weight;
      submission.evidence = message.value().evidence;
      submission.rationale = message.value().rationale;
      Outcome<EvaluationId> submitted = fabric_->submit_evaluation(submission);
      if (!submitted.ok()) {
        send_error(connection, frame.correlation, submitted.code(), submitted.error().message);
      }
      pump(submission.authority.evaluator.ensemble);
      return;
    }
    case MessageType::define_ensemble: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()), limits);
      Outcome<DefineEnsembleMessage> message = decode_define_ensemble(decoder);
      if (!message.ok()) {
        send_error(connection, frame.correlation, message.code(), message.error().message);
        return;
      }
      Outcome<EnsembleSpec> spec = decode_spec(
          std::span<const std::byte>(message.value().encoded_spec.data(),
                                     message.value().encoded_spec.size()),
          limits);
      DefineEnsembleAck ack;
      if (!spec.ok()) {
        ack.accepted = false;
        ack.code = spec.code();
        ack.detail = detail::sanitize_text(spec.error().message, limits.max_metadata_bytes);
      } else {
        Outcome<EnsembleGeneration> generation = fabric_->define_ensemble(spec.value());
        ack.ensemble = spec.value().id;
        if (generation.ok()) {
          ack.accepted = true;
          ack.generation = generation.value();
          ack.detail = "ensemble defined";
          const std::lock_guard<std::mutex> pending_guard(impl_->pending_mutex);
          impl_->state_dirty = true;
        } else {
          ack.accepted = false;
          ack.code = generation.code();
          ack.detail = detail::sanitize_text(generation.error().message, limits.max_metadata_bytes);
        }
      }
      Encoder payload(limits, 256u);
      encode(ack, payload);
      Frame response;
      response.type = MessageType::define_ensemble_ack;
      response.correlation = frame.correlation;
      response.payload = payload.take();
      send_frame(connection, response);
      return;
    }
    case MessageType::open_execution: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()), limits);
      Outcome<OpenExecutionMessage> message = decode_open_execution(decoder);
      if (!message.ok()) {
        send_error(connection, frame.correlation, message.code(), message.error().message);
        return;
      }
      Outcome<EnsembleExecutionId> execution =
          fabric_->open_execution(message.value().ensemble, message.value().ensemble_generation);
      OpenExecutionAck ack;
      ack.ensemble = message.value().ensemble;
      ack.ensemble_generation = message.value().ensemble_generation;
      if (execution.ok()) {
        ack.accepted = true;
        ack.execution = execution.value();
        ack.detail = "execution opened";
        const std::lock_guard<std::mutex> pending_guard(impl_->pending_mutex);
        impl_->state_dirty = true;
      } else {
        ack.accepted = false;
        ack.code = execution.code();
        ack.detail = detail::sanitize_text(execution.error().message, limits.max_metadata_bytes);
      }
      Encoder payload(limits, 256u);
      encode(ack, payload);
      Frame response;
      response.type = MessageType::open_execution_ack;
      response.correlation = frame.correlation;
      response.payload = payload.take();
      send_frame(connection, response);
      if (execution.ok()) {
        pump(message.value().ensemble);
      }
      return;
    }
    case MessageType::cancel_ensemble: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()), limits);
      Outcome<CancelEnsembleMessage> message = decode_cancel_ensemble(decoder);
      if (!message.ok()) {
        send_error(connection, frame.correlation, message.code(), message.error().message);
        return;
      }
      const Status cancelled = fabric_->cancel_ensemble(
          message.value().ensemble, message.value().ensemble_generation, message.value().reason);
      if (cancelled.ok()) {
        pump(message.value().ensemble);
      }
      StatusResponseMessage ack;
      ack.code = cancelled.ok() ? EnsembleError::ok : cancelled.code();
      ack.detail = cancelled.ok()
                       ? "cancellation recorded"
                       : detail::sanitize_text(cancelled.error().message, limits.max_metadata_bytes);
      Encoder payload(limits, 256u);
      encode(ack, payload);
      Frame response;
      response.type = MessageType::status_response;
      response.correlation = frame.correlation;
      response.epoch = fabric_->epoch();
      response.payload = payload.take();
      send_frame(connection, response);
      {
        const std::lock_guard<std::mutex> pending_guard(impl_->pending_mutex);
        impl_->state_dirty = true;
      }
      return;
    }
    case MessageType::finalize_request: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()), limits);
      Outcome<FinalizeRequestMessage> message = decode_finalize_request(decoder);
      if (!message.ok()) {
        send_error(connection, frame.correlation, message.code(), message.error().message);
        return;
      }
      // The request is parked: it is answered only once the execution has
      // genuinely settled, which keeps the controller free of polling and
      // free of timeouts.
      PendingFinalize waiter;
      waiter.connection = connection;
      waiter.correlation = frame.correlation;
      waiter.ensemble = message.value().ensemble;
      waiter.generation = message.value().ensemble_generation;
      const std::lock_guard<std::mutex> guard(impl_->pending_mutex);
      impl_->pending_finalizes.push_back(std::move(waiter));
      return;
    }
    case MessageType::query_result: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()), limits);
      Outcome<QueryResultMessage> message = decode_query_result(decoder);
      if (!message.ok()) {
        send_error(connection, frame.correlation, message.code(), message.error().message);
        return;
      }
      ResultResponseMessage response;
      response.ensemble = message.value().ensemble;
      response.ensemble_generation = message.value().ensemble_generation;
      Outcome<EnsembleResult> result = fabric_->current_result(message.value().ensemble);
      if (result.ok()) {
        response.has_result = true;
        response.code = EnsembleError::ok;
        response.decision = result.value().decision;
        response.encoded_result = encode_result_payload(result.value(), limits);
      } else {
        response.has_result = false;
        response.code = result.code();
      }
      Encoder payload(limits, 512u + response.encoded_result.size());
      encode(response, payload);
      Frame outbound;
      outbound.type = MessageType::result_response;
      outbound.correlation = frame.correlation;
      outbound.epoch = fabric_->epoch();
      outbound.payload = payload.take();
      send_frame(connection, outbound);
      return;
    }
    case MessageType::inspect_request: {
      Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()), limits);
      Outcome<InspectRequestMessage> message = decode_inspect_request(decoder);
      if (!message.ok()) {
        send_error(connection, frame.correlation, message.code(), message.error().message);
        return;
      }
      InspectResponseMessage response;
      response.ensemble = message.value().ensemble;
      Outcome<EnsembleSnapshot> snapshot = fabric_->inspect(message.value().ensemble);
      if (snapshot.ok()) {
        response.code = EnsembleError::ok;
        response.ensemble_generation = snapshot.value().generation;
        response.encoded_report = encode_report(snapshot.value().render(), limits);
      } else {
        response.code = snapshot.code();
      }
      Encoder payload(limits, 512u + response.encoded_report.size());
      encode(response, payload);
      Frame outbound;
      outbound.type = MessageType::inspect_response;
      outbound.correlation = frame.correlation;
      outbound.payload = payload.take();
      send_frame(connection, outbound);
      return;
    }
    case MessageType::heartbeat:
      return;
    case MessageType::goodbye: {
      handle_connection_loss(connection);
      return;
    }
    default: {
      send_error(connection, frame.correlation, EnsembleError::protocol_error,
                 "the coordinator does not accept this message type");
      return;
    }
  }
}

void EnsembleCoordinator::maybe_persist() {
  if (!config_.persist_on_change || config_.state_path.empty()) {
    return;
  }
  bool dirty = false;
  {
    const std::lock_guard<std::mutex> guard(impl_->pending_mutex);
    dirty = impl_->state_dirty;
    impl_->state_dirty = false;
  }
  if (!dirty) {
    return;
  }
  const Status saved = fabric_->save_state(config_.state_path);
  if (!saved.ok()) {
    // A failed durable write is reported, never silently ignored; the state
    // file from the previous successful write remains authoritative.
    (void)saved;
  }
}

}  // namespace ensemble_fabric
