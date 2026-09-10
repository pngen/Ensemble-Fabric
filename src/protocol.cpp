#include "ensemble_fabric/protocol.hpp"

#include <cstring>

#include "ensemble_fabric/version.hpp"

#include "detail/text.hpp"

namespace ensemble_fabric {
namespace {

inline constexpr std::size_t kMagicOffset = 0;
inline constexpr std::size_t kVersionOffset = 4;
inline constexpr std::size_t kHeaderBytesOffset = 6;
inline constexpr std::size_t kTypeOffset = 8;
inline constexpr std::size_t kFlagsOffset = 10;
inline constexpr std::size_t kPayloadLengthOffset = 12;
inline constexpr std::size_t kCorrelationOffset = 16;
inline constexpr std::size_t kEpochOffset = 24;
inline constexpr std::size_t kBootOffset = 32;
inline constexpr std::size_t kSequenceOffset = 40;
inline constexpr std::size_t kReservedOffset = 48;
inline constexpr std::size_t kChecksumOffset = 52;
inline constexpr std::size_t kTrailerOffset = 56;

void store_u16(std::byte* out, std::uint16_t value) {
  out[0] = static_cast<std::byte>(value & 0xFFu);
  out[1] = static_cast<std::byte>((value >> 8u) & 0xFFu);
}

void store_u32(std::byte* out, std::uint32_t value) {
  for (std::size_t index = 0; index < 4u; ++index) {
    out[index] = static_cast<std::byte>((value >> (index * 8u)) & 0xFFu);
  }
}

void store_u64(std::byte* out, std::uint64_t value) {
  for (std::size_t index = 0; index < 8u; ++index) {
    out[index] = static_cast<std::byte>((value >> (index * 8u)) & 0xFFu);
  }
}

[[nodiscard]] std::uint16_t load_u16(const std::byte* in) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0]) |
                                    (static_cast<std::uint16_t>(in[1]) << 8u));
}

[[nodiscard]] std::uint32_t load_u32(const std::byte* in) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4u; ++index) {
    value |= static_cast<std::uint32_t>(in[index]) << (index * 8u);
  }
  return value;
}

[[nodiscard]] std::uint64_t load_u64(const std::byte* in) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8u; ++index) {
    value |= static_cast<std::uint64_t>(in[index]) << (index * 8u);
  }
  return value;
}

[[nodiscard]] DecodeResult failure(EnsembleError code, std::string message) {
  DecodeResult result;
  result.status = DecodeStatus::failed;
  result.error = Error{code, std::move(message)};
  return result;
}

}  // namespace

std::string_view to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::hello: return "HELLO";
    case MessageType::hello_ack: return "HELLO_ACK";
    case MessageType::register_participant: return "REGISTER_PARTICIPANT";
    case MessageType::register_ack: return "REGISTER_ACK";
    case MessageType::heartbeat: return "HEARTBEAT";
    case MessageType::dispatch_candidate: return "DISPATCH_CANDIDATE";
    case MessageType::candidate_result: return "CANDIDATE_RESULT";
    case MessageType::candidate_failure: return "CANDIDATE_FAILURE";
    case MessageType::candidate_abstention: return "CANDIDATE_ABSTENTION";
    case MessageType::evaluation_request: return "EVALUATION_REQUEST";
    case MessageType::evaluation_result: return "EVALUATION_RESULT";
    case MessageType::cancel: return "CANCEL";
    case MessageType::define_ensemble: return "DEFINE_ENSEMBLE";
    case MessageType::define_ensemble_ack: return "DEFINE_ENSEMBLE_ACK";
    case MessageType::open_execution: return "OPEN_EXECUTION";
    case MessageType::open_execution_ack: return "OPEN_EXECUTION_ACK";
    case MessageType::finalize_request: return "FINALIZE_REQUEST";
    case MessageType::finalize_response: return "FINALIZE_RESPONSE";
    case MessageType::inspect_request: return "INSPECT_REQUEST";
    case MessageType::inspect_response: return "INSPECT_RESPONSE";
    case MessageType::worker_ready: return "WORKER_READY";
    case MessageType::goodbye: return "GOODBYE";
    case MessageType::error_response: return "ERROR_RESPONSE";
    case MessageType::cancel_ensemble: return "CANCEL_ENSEMBLE";
    case MessageType::query_result: return "QUERY_RESULT";
    case MessageType::result_response: return "RESULT_RESPONSE";
    case MessageType::status_response: return "STATUS_RESPONSE";
  }
  return "UNKNOWN_MESSAGE";
}

std::optional<MessageType> parse_message_type(std::uint16_t value) noexcept {
  if (value < 1u || value > 27u) {
    return std::nullopt;
  }
  return static_cast<MessageType>(value);
}

Outcome<std::vector<std::byte>> encode_frame(const Frame& frame, const Limits& limits) {
  if (frame.payload.size() > limits.max_frame_bytes) {
    return fail<std::vector<std::byte>>(EnsembleError::payload_too_large,
                                        "frame payload exceeds the configured maximum frame size");
  }
  const std::uint64_t total = static_cast<std::uint64_t>(frame_header_bytes) + frame.payload.size();
  if (total > limits.max_frame_bytes || total > 0xFFFFFFFFull) {
    return fail<std::vector<std::byte>>(EnsembleError::payload_too_large,
                                        "frame exceeds the configured maximum frame size");
  }
  std::vector<std::byte> out(frame_header_bytes + frame.payload.size());
  std::byte* header = out.data();
  std::memset(header, 0, frame_header_bytes);
  store_u32(header + kMagicOffset, frame_magic);
  store_u16(header + kVersionOffset, protocol_version);
  store_u16(header + kHeaderBytesOffset, static_cast<std::uint16_t>(frame_header_bytes));
  store_u16(header + kTypeOffset, static_cast<std::uint16_t>(frame.type));
  store_u16(header + kFlagsOffset, frame.flags);
  store_u32(header + kPayloadLengthOffset, static_cast<std::uint32_t>(frame.payload.size()));
  store_u64(header + kCorrelationOffset, frame.correlation.value());
  store_u64(header + kEpochOffset, frame.epoch.value());
  store_u64(header + kBootOffset, frame.worker_boot.value());
  store_u64(header + kSequenceOffset, frame.sequence.value());
  store_u32(header + kReservedOffset, 0u);
  store_u64(header + kTrailerOffset, 0u);
  std::copy(frame.payload.begin(), frame.payload.end(),
            out.begin() + static_cast<std::ptrdiff_t>(frame_header_bytes));
  std::uint32_t checksum =
      crc32c(std::span<const std::byte>(out.data(), frame_header_bytes));
  checksum = crc32c_extend(
      checksum, std::span<const std::byte>(out.data() + frame_header_bytes, frame.payload.size()));
  store_u32(header + kChecksumOffset, checksum);
  return out;
}

DecodeResult decode_frame(std::span<const std::byte> buffer, const Limits& limits) {
  if (buffer.size() < frame_header_bytes) {
    DecodeResult result;
    result.status = DecodeStatus::need_more_data;
    return result;
  }
  const std::byte* header = buffer.data();
  if (load_u32(header + kMagicOffset) != frame_magic) {
    return failure(EnsembleError::malformed_frame, "frame magic does not match");
  }
  const std::uint16_t version = load_u16(header + kVersionOffset);
  if (version != protocol_version) {
    return failure(EnsembleError::unsupported_version,
                   "frame protocol version " + std::to_string(version) +
                       " is not supported by this build");
  }
  const std::uint16_t header_bytes = load_u16(header + kHeaderBytesOffset);
  if (header_bytes != frame_header_bytes) {
    return failure(EnsembleError::malformed_frame,
                   "frame declares an unexpected header size");
  }
  const std::optional<MessageType> type = parse_message_type(load_u16(header + kTypeOffset));
  if (!type.has_value()) {
    return failure(EnsembleError::invalid_enum_value, "frame carries an unknown message type");
  }
  const std::uint32_t payload_length = load_u32(header + kPayloadLengthOffset);
  // The length is validated against the configured bound before any payload
  // allocation happens.
  if (payload_length > limits.max_frame_bytes ||
      static_cast<std::uint64_t>(payload_length) + frame_header_bytes > limits.max_frame_bytes) {
    return failure(EnsembleError::payload_too_large,
                   "frame declares a payload larger than the configured maximum frame size");
  }
  // Reserved words must be zero: a non-zero reserved word means the peer speaks
  // a variant this build does not understand.
  if (load_u32(header + kReservedOffset) != 0u || load_u64(header + kTrailerOffset) != 0u) {
    return failure(EnsembleError::malformed_frame, "frame reserved words are not zero");
  }
  const std::size_t total = frame_header_bytes + payload_length;
  if (buffer.size() < total) {
    DecodeResult result;
    result.status = DecodeStatus::need_more_data;
    return result;
  }
  const std::uint32_t stored_checksum = load_u32(header + kChecksumOffset);
  std::vector<std::byte> header_copy(buffer.begin(),
                                     buffer.begin() + static_cast<std::ptrdiff_t>(frame_header_bytes));
  store_u32(header_copy.data() + kChecksumOffset, 0u);
  std::uint32_t checksum = crc32c(std::span<const std::byte>(header_copy.data(), frame_header_bytes));
  checksum = crc32c_extend(
      checksum, std::span<const std::byte>(buffer.data() + frame_header_bytes, payload_length));
  if (checksum != stored_checksum) {
    return failure(EnsembleError::checksum_mismatch, "frame checksum does not match");
  }

  DecodeResult result;
  result.status = DecodeStatus::complete;
  result.consumed = total;
  result.frame.version = version;
  result.frame.type = type.value();
  result.frame.flags = load_u16(header + kFlagsOffset);
  result.frame.correlation = RequestId(load_u64(header + kCorrelationOffset));
  result.frame.epoch = CoordinatorEpoch(load_u64(header + kEpochOffset));
  result.frame.worker_boot = WorkerBootId(load_u64(header + kBootOffset));
  result.frame.sequence = Sequence(load_u64(header + kSequenceOffset));
  result.frame.payload.assign(buffer.begin() + static_cast<std::ptrdiff_t>(frame_header_bytes),
                              buffer.begin() + static_cast<std::ptrdiff_t>(total));
  return result;
}

// ---------------------------------------------------------------------------
// Message payloads
// ---------------------------------------------------------------------------

void encode(const HelloMessage& message, Encoder& out) {
  out.u64(message.worker.value());
  out.u64(message.worker_boot.value());
  out.u64(message.coordinator_epoch.value());
  out.u16(message.profile.protocol_version);
  out.u32(message.profile.capability_mask);
  out.u32(message.profile.task_class);
  out.u32(message.profile.output_schema);
  out.u32(message.profile.model_family);
  out.u32(message.profile.backend_features);
  out.string(message.label);
}

Outcome<HelloMessage> decode_hello(Decoder& in) {
  HelloMessage message;
  Outcome<std::uint64_t> worker = in.u64();
  if (!worker.ok()) return fail<HelloMessage>(worker.code(), worker.error().message);
  message.worker = WorkerId(worker.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<HelloMessage>(boot.code(), boot.error().message);
  message.worker_boot = WorkerBootId(boot.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<HelloMessage>(epoch.code(), epoch.error().message);
  message.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint16_t> version = in.u16();
  if (!version.ok()) return fail<HelloMessage>(version.code(), version.error().message);
  message.profile.protocol_version = version.value();
  std::uint32_t* fields[] = {&message.profile.capability_mask, &message.profile.task_class,
                             &message.profile.output_schema, &message.profile.model_family,
                             &message.profile.backend_features};
  for (std::uint32_t* field : fields) {
    Outcome<std::uint32_t> value = in.u32();
    if (!value.ok()) return fail<HelloMessage>(value.code(), value.error().message);
    *field = value.value();
  }
  Outcome<std::string> label = in.string(4096u);
  if (!label.ok()) return fail<HelloMessage>(label.code(), label.error().message);
  message.label = std::move(label).value();
  return message;
}

void encode(const WorkerReadyMessage& message, Encoder& out) {
  out.u64(message.worker.value());
  out.u64(message.worker_boot.value());
  out.u64(message.coordinator_epoch.value());
  out.u64(message.participant.value());
  out.u32(message.roles.mask());
  out.u16(message.profile.protocol_version);
  out.u32(message.profile.capability_mask);
  out.u32(message.profile.task_class);
  out.u32(message.profile.output_schema);
  out.u32(message.profile.model_family);
  out.u32(message.profile.backend_features);
  out.string(message.label);
}

Outcome<WorkerReadyMessage> decode_worker_ready(Decoder& in) {
  WorkerReadyMessage message;
  Outcome<std::uint64_t> worker = in.u64();
  if (!worker.ok()) return fail<WorkerReadyMessage>(worker.code(), worker.error().message);
  message.worker = WorkerId(worker.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<WorkerReadyMessage>(boot.code(), boot.error().message);
  message.worker_boot = WorkerBootId(boot.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<WorkerReadyMessage>(epoch.code(), epoch.error().message);
  message.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> participant = in.u64();
  if (!participant.ok()) return fail<WorkerReadyMessage>(participant.code(), participant.error().message);
  message.participant = ParticipantId(participant.value());
  Outcome<std::uint32_t> roles = in.u32();
  if (!roles.ok()) return fail<WorkerReadyMessage>(roles.code(), roles.error().message);
  if ((roles.value() & ~0x1Fu) != 0u) {
    return fail<WorkerReadyMessage>(EnsembleError::invalid_enum_value,
                                    "worker claims an unknown role bit");
  }
  message.roles = RoleSet(roles.value());
  Outcome<std::uint16_t> version = in.u16();
  if (!version.ok()) return fail<WorkerReadyMessage>(version.code(), version.error().message);
  message.profile.protocol_version = version.value();
  std::uint32_t* fields[] = {&message.profile.capability_mask, &message.profile.task_class,
                             &message.profile.output_schema, &message.profile.model_family,
                             &message.profile.backend_features};
  for (std::uint32_t* field : fields) {
    Outcome<std::uint32_t> value = in.u32();
    if (!value.ok()) return fail<WorkerReadyMessage>(value.code(), value.error().message);
    *field = value.value();
  }
  Outcome<std::string> label = in.string(4096u);
  if (!label.ok()) return fail<WorkerReadyMessage>(label.code(), label.error().message);
  message.label = std::move(label).value();
  return message;
}

void encode(const CoordinatorAck& message, Encoder& out) {
  out.u64(message.coordinator_epoch.value());
  out.u64(message.coordinator_boot.value());
  out.u32(message.status_code);
  out.boolean(message.accepted);
  out.string(message.detail);
}

Outcome<CoordinatorAck> decode_coordinator_ack(Decoder& in) {
  CoordinatorAck message;
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<CoordinatorAck>(epoch.code(), epoch.error().message);
  message.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<CoordinatorAck>(boot.code(), boot.error().message);
  message.coordinator_boot = WorkerBootId(boot.value());
  Outcome<std::uint32_t> status = in.u32();
  if (!status.ok()) return fail<CoordinatorAck>(status.code(), status.error().message);
  message.status_code = status.value();
  Outcome<bool> accepted = in.boolean();
  if (!accepted.ok()) return fail<CoordinatorAck>(accepted.code(), accepted.error().message);
  message.accepted = accepted.value();
  Outcome<std::string> detail = in.string(4096u);
  if (!detail.ok()) return fail<CoordinatorAck>(detail.code(), detail.error().message);
  message.detail = std::move(detail).value();
  return message;
}

void encode(const RegistrationAck& message, Encoder& out) {
  out.u64(message.coordinator_epoch.value());
  out.u64(message.participant.value());
  out.u64(message.participant_generation.value());
  out.boolean(message.accepted);
  out.u16(static_cast<std::uint16_t>(message.code));
  out.string(message.detail);
}

Outcome<RegistrationAck> decode_registration_ack(Decoder& in) {
  RegistrationAck message;
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<RegistrationAck>(epoch.code(), epoch.error().message);
  message.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> participant = in.u64();
  if (!participant.ok()) return fail<RegistrationAck>(participant.code(), participant.error().message);
  message.participant = ParticipantId(participant.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<RegistrationAck>(generation.code(), generation.error().message);
  message.participant_generation = ParticipantGeneration(generation.value());
  Outcome<bool> accepted = in.boolean();
  if (!accepted.ok()) return fail<RegistrationAck>(accepted.code(), accepted.error().message);
  message.accepted = accepted.value();
  Outcome<std::uint16_t> code = in.u16();
  if (!code.ok()) return fail<RegistrationAck>(code.code(), code.error().message);
  if (code.value() > static_cast<std::uint16_t>(EnsembleError::internal_error)) {
    return fail<RegistrationAck>(EnsembleError::invalid_enum_value, "invalid error code in ack");
  }
  message.code = static_cast<EnsembleError>(code.value());
  Outcome<std::string> detail = in.string(4096u);
  if (!detail.ok()) return fail<RegistrationAck>(detail.code(), detail.error().message);
  message.detail = std::move(detail).value();
  return message;
}

void encode(const RegisterParticipantMessage& message, Encoder& out) {
  out.u64(message.registration.ensemble.value());
  out.u64(message.registration.ensemble_generation.value());
  out.u64(message.registration.participant.value());
  out.u64(message.registration.worker.value());
  out.u64(message.registration.worker_boot.value());
  out.u64(message.registration.coordinator_epoch.value());
  out.u32(message.registration.claimed_roles.mask());
  out.u16(message.registration.profile.protocol_version);
  out.u32(message.registration.profile.capability_mask);
  out.u32(message.registration.profile.task_class);
  out.u32(message.registration.profile.output_schema);
  out.u32(message.registration.profile.model_family);
  out.u32(message.registration.profile.backend_features);
}

Outcome<RegisterParticipantMessage> decode_register_participant(Decoder& in) {
  RegisterParticipantMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<RegisterParticipantMessage>(ensemble.code(), ensemble.error().message);
  message.registration.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok())
    return fail<RegisterParticipantMessage>(generation.code(), generation.error().message);
  message.registration.ensemble_generation = EnsembleGeneration(generation.value());
  Outcome<std::uint64_t> participant = in.u64();
  if (!participant.ok())
    return fail<RegisterParticipantMessage>(participant.code(), participant.error().message);
  message.registration.participant = ParticipantId(participant.value());
  Outcome<std::uint64_t> worker = in.u64();
  if (!worker.ok()) return fail<RegisterParticipantMessage>(worker.code(), worker.error().message);
  message.registration.worker = WorkerId(worker.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<RegisterParticipantMessage>(boot.code(), boot.error().message);
  message.registration.worker_boot = WorkerBootId(boot.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<RegisterParticipantMessage>(epoch.code(), epoch.error().message);
  message.registration.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint32_t> roles = in.u32();
  if (!roles.ok()) return fail<RegisterParticipantMessage>(roles.code(), roles.error().message);
  if ((roles.value() & ~0x1Fu) != 0u) {
    return fail<RegisterParticipantMessage>(EnsembleError::invalid_enum_value,
                                            "registration claims an unknown role bit");
  }
  message.registration.claimed_roles = RoleSet(roles.value());
  Outcome<std::uint16_t> version = in.u16();
  if (!version.ok()) return fail<RegisterParticipantMessage>(version.code(), version.error().message);
  message.registration.profile.protocol_version = version.value();
  std::uint32_t* fields[] = {&message.registration.profile.capability_mask,
                             &message.registration.profile.task_class,
                             &message.registration.profile.output_schema,
                             &message.registration.profile.model_family,
                             &message.registration.profile.backend_features};
  for (std::uint32_t* field : fields) {
    Outcome<std::uint32_t> value = in.u32();
    if (!value.ok()) return fail<RegisterParticipantMessage>(value.code(), value.error().message);
    *field = value.value();
  }
  return message;
}

void encode(const DispatchCandidateMessage& message, Encoder& out) {
  out.u64(message.ensemble.value());
  out.u64(message.ensemble_generation.value());
  out.u64(message.execution.value());
  out.u64(message.attempt_generation.value());
  out.u64(message.participant.value());
  out.u64(message.participant_generation.value());
  out.u64(message.coordinator_epoch.value());
  out.u64(message.candidate.value());
  out.u64(message.candidate_generation.value());
  out.u8(static_cast<std::uint8_t>(message.role));
  out.u64(message.stage.value());
  out.u32(message.attempt);
  out.u32(message.budget_units);
  out.u64(message.deterministic_seed);
  out.string(message.domain);
  out.bytes(message.request);
}

Outcome<DispatchCandidateMessage> decode_dispatch_candidate(Decoder& in) {
  DispatchCandidateMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<DispatchCandidateMessage>(ensemble.code(), ensemble.error().message);
  message.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> ensemble_generation = in.u64();
  if (!ensemble_generation.ok())
    return fail<DispatchCandidateMessage>(ensemble_generation.code(), ensemble_generation.error().message);
  message.ensemble_generation = EnsembleGeneration(ensemble_generation.value());
  Outcome<std::uint64_t> execution = in.u64();
  if (!execution.ok()) return fail<DispatchCandidateMessage>(execution.code(), execution.error().message);
  message.execution = EnsembleExecutionId(execution.value());
  Outcome<std::uint64_t> attempt_generation = in.u64();
  if (!attempt_generation.ok())
    return fail<DispatchCandidateMessage>(attempt_generation.code(), attempt_generation.error().message);
  message.attempt_generation = EnsembleAttemptGeneration(attempt_generation.value());
  Outcome<std::uint64_t> participant = in.u64();
  if (!participant.ok())
    return fail<DispatchCandidateMessage>(participant.code(), participant.error().message);
  message.participant = ParticipantId(participant.value());
  Outcome<std::uint64_t> participant_generation = in.u64();
  if (!participant_generation.ok())
    return fail<DispatchCandidateMessage>(participant_generation.code(),
                                          participant_generation.error().message);
  message.participant_generation = ParticipantGeneration(participant_generation.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<DispatchCandidateMessage>(epoch.code(), epoch.error().message);
  message.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> candidate = in.u64();
  if (!candidate.ok()) return fail<DispatchCandidateMessage>(candidate.code(), candidate.error().message);
  message.candidate = CandidateId(candidate.value());
  Outcome<std::uint64_t> candidate_generation = in.u64();
  if (!candidate_generation.ok())
    return fail<DispatchCandidateMessage>(candidate_generation.code(), candidate_generation.error().message);
  message.candidate_generation = CandidateGeneration(candidate_generation.value());
  Outcome<std::uint8_t> role = in.u8();
  if (!role.ok()) return fail<DispatchCandidateMessage>(role.code(), role.error().message);
  if (role.value() < 1u || role.value() > 5u) {
    return fail<DispatchCandidateMessage>(EnsembleError::invalid_enum_value,
                                          "dispatch carries an unknown participant role");
  }
  message.role = static_cast<ParticipantRole>(role.value());
  Outcome<std::uint64_t> stage = in.u64();
  if (!stage.ok()) return fail<DispatchCandidateMessage>(stage.code(), stage.error().message);
  message.stage = StageId(stage.value());
  Outcome<std::uint32_t> attempt = in.u32();
  if (!attempt.ok()) return fail<DispatchCandidateMessage>(attempt.code(), attempt.error().message);
  message.attempt = attempt.value();
  Outcome<std::uint32_t> budget = in.u32();
  if (!budget.ok()) return fail<DispatchCandidateMessage>(budget.code(), budget.error().message);
  message.budget_units = budget.value();
  Outcome<std::uint64_t> seed = in.u64();
  if (!seed.ok()) return fail<DispatchCandidateMessage>(seed.code(), seed.error().message);
  message.deterministic_seed = seed.value();
  Outcome<std::string> domain = in.string(4096u);
  if (!domain.ok()) return fail<DispatchCandidateMessage>(domain.code(), domain.error().message);
  message.domain = std::move(domain).value();
  Outcome<std::vector<std::byte>> request = in.bytes(1u << 20);
  if (!request.ok()) return fail<DispatchCandidateMessage>(request.code(), request.error().message);
  message.request = std::move(request).value();
  return message;
}

void encode(const CandidateResultMessage& message, Encoder& out) {
  const CandidateAuthority& authority = message.authority;
  out.u64(authority.participant.ensemble.value());
  out.u64(authority.participant.ensemble_generation.value());
  out.u64(authority.participant.participant.value());
  out.u64(authority.participant.participant_generation.value());
  out.u64(authority.participant.worker.value());
  out.u64(authority.participant.worker_boot.value());
  out.u64(authority.participant.coordinator_epoch.value());
  out.u64(authority.execution.value());
  out.u64(authority.attempt_generation.value());
  out.u64(authority.candidate.value());
  out.u64(authority.candidate_generation.value());
  out.bytes(message.payload);
  out.u32(message.produced_units);
}

Outcome<CandidateResultMessage> decode_candidate_result(Decoder& in) {
  CandidateResultMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<CandidateResultMessage>(ensemble.code(), ensemble.error().message);
  message.authority.participant.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> ensemble_generation = in.u64();
  if (!ensemble_generation.ok())
    return fail<CandidateResultMessage>(ensemble_generation.code(), ensemble_generation.error().message);
  message.authority.participant.ensemble_generation = EnsembleGeneration(ensemble_generation.value());
  Outcome<std::uint64_t> participant = in.u64();
  if (!participant.ok())
    return fail<CandidateResultMessage>(participant.code(), participant.error().message);
  message.authority.participant.participant = ParticipantId(participant.value());
  Outcome<std::uint64_t> participant_generation = in.u64();
  if (!participant_generation.ok())
    return fail<CandidateResultMessage>(participant_generation.code(),
                                        participant_generation.error().message);
  message.authority.participant.participant_generation =
      ParticipantGeneration(participant_generation.value());
  Outcome<std::uint64_t> worker = in.u64();
  if (!worker.ok()) return fail<CandidateResultMessage>(worker.code(), worker.error().message);
  message.authority.participant.worker = WorkerId(worker.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<CandidateResultMessage>(boot.code(), boot.error().message);
  message.authority.participant.worker_boot = WorkerBootId(boot.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<CandidateResultMessage>(epoch.code(), epoch.error().message);
  message.authority.participant.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> execution = in.u64();
  if (!execution.ok()) return fail<CandidateResultMessage>(execution.code(), execution.error().message);
  message.authority.execution = EnsembleExecutionId(execution.value());
  Outcome<std::uint64_t> attempt = in.u64();
  if (!attempt.ok()) return fail<CandidateResultMessage>(attempt.code(), attempt.error().message);
  message.authority.attempt_generation = EnsembleAttemptGeneration(attempt.value());
  Outcome<std::uint64_t> candidate = in.u64();
  if (!candidate.ok()) return fail<CandidateResultMessage>(candidate.code(), candidate.error().message);
  message.authority.candidate = CandidateId(candidate.value());
  Outcome<std::uint64_t> candidate_generation = in.u64();
  if (!candidate_generation.ok())
    return fail<CandidateResultMessage>(candidate_generation.code(), candidate_generation.error().message);
  message.authority.candidate_generation = CandidateGeneration(candidate_generation.value());
  Outcome<std::vector<std::byte>> payload = in.bytes(64u << 20);
  if (!payload.ok()) return fail<CandidateResultMessage>(payload.code(), payload.error().message);
  message.payload = std::move(payload).value();
  Outcome<std::uint32_t> units = in.u32();
  if (!units.ok()) return fail<CandidateResultMessage>(units.code(), units.error().message);
  message.produced_units = units.value();
  return message;
}

void encode(const CandidateFailureMessage& message, Encoder& out) {
  const CandidateAuthority& authority = message.authority;
  out.u64(authority.participant.ensemble.value());
  out.u64(authority.participant.ensemble_generation.value());
  out.u64(authority.participant.participant.value());
  out.u64(authority.participant.participant_generation.value());
  out.u64(authority.participant.worker.value());
  out.u64(authority.participant.worker_boot.value());
  out.u64(authority.participant.coordinator_epoch.value());
  out.u64(authority.execution.value());
  out.u64(authority.attempt_generation.value());
  out.u64(authority.candidate.value());
  out.u64(authority.candidate_generation.value());
  out.u8(static_cast<std::uint8_t>(message.kind));
  out.string(message.detail);
}

Outcome<CandidateFailureMessage> decode_candidate_failure(Decoder& in) {
  CandidateFailureMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<CandidateFailureMessage>(ensemble.code(), ensemble.error().message);
  message.authority.participant.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> ensemble_generation = in.u64();
  if (!ensemble_generation.ok())
    return fail<CandidateFailureMessage>(ensemble_generation.code(), ensemble_generation.error().message);
  message.authority.participant.ensemble_generation = EnsembleGeneration(ensemble_generation.value());
  Outcome<std::uint64_t> participant = in.u64();
  if (!participant.ok())
    return fail<CandidateFailureMessage>(participant.code(), participant.error().message);
  message.authority.participant.participant = ParticipantId(participant.value());
  Outcome<std::uint64_t> participant_generation = in.u64();
  if (!participant_generation.ok())
    return fail<CandidateFailureMessage>(participant_generation.code(),
                                         participant_generation.error().message);
  message.authority.participant.participant_generation =
      ParticipantGeneration(participant_generation.value());
  Outcome<std::uint64_t> worker = in.u64();
  if (!worker.ok()) return fail<CandidateFailureMessage>(worker.code(), worker.error().message);
  message.authority.participant.worker = WorkerId(worker.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<CandidateFailureMessage>(boot.code(), boot.error().message);
  message.authority.participant.worker_boot = WorkerBootId(boot.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<CandidateFailureMessage>(epoch.code(), epoch.error().message);
  message.authority.participant.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> execution = in.u64();
  if (!execution.ok()) return fail<CandidateFailureMessage>(execution.code(), execution.error().message);
  message.authority.execution = EnsembleExecutionId(execution.value());
  Outcome<std::uint64_t> attempt = in.u64();
  if (!attempt.ok()) return fail<CandidateFailureMessage>(attempt.code(), attempt.error().message);
  message.authority.attempt_generation = EnsembleAttemptGeneration(attempt.value());
  Outcome<std::uint64_t> candidate = in.u64();
  if (!candidate.ok()) return fail<CandidateFailureMessage>(candidate.code(), candidate.error().message);
  message.authority.candidate = CandidateId(candidate.value());
  Outcome<std::uint64_t> candidate_generation = in.u64();
  if (!candidate_generation.ok())
    return fail<CandidateFailureMessage>(candidate_generation.code(), candidate_generation.error().message);
  message.authority.candidate_generation = CandidateGeneration(candidate_generation.value());
  Outcome<std::uint8_t> kind = in.u8();
  if (!kind.ok()) return fail<CandidateFailureMessage>(kind.code(), kind.error().message);
  if (kind.value() < 1u || kind.value() > 6u) {
    return fail<CandidateFailureMessage>(EnsembleError::invalid_enum_value,
                                         "unknown participant failure kind");
  }
  message.kind = static_cast<ParticipantFailureKind>(kind.value());
  Outcome<std::string> detail = in.string(4096u);
  if (!detail.ok()) return fail<CandidateFailureMessage>(detail.code(), detail.error().message);
  message.detail = std::move(detail).value();
  return message;
}

void encode(const CandidateAbstentionMessage& message, Encoder& out) {
  const CandidateAuthority& authority = message.authority;
  out.u64(authority.participant.ensemble.value());
  out.u64(authority.participant.ensemble_generation.value());
  out.u64(authority.participant.participant.value());
  out.u64(authority.participant.participant_generation.value());
  out.u64(authority.participant.worker.value());
  out.u64(authority.participant.worker_boot.value());
  out.u64(authority.participant.coordinator_epoch.value());
  out.u64(authority.execution.value());
  out.u64(authority.attempt_generation.value());
  out.u64(authority.candidate.value());
  out.u64(authority.candidate_generation.value());
  out.string(message.rationale);
}

Outcome<CandidateAbstentionMessage> decode_candidate_abstention(Decoder& in) {
  CandidateAbstentionMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<CandidateAbstentionMessage>(ensemble.code(), ensemble.error().message);
  message.authority.participant.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> ensemble_generation = in.u64();
  if (!ensemble_generation.ok())
    return fail<CandidateAbstentionMessage>(ensemble_generation.code(),
                                            ensemble_generation.error().message);
  message.authority.participant.ensemble_generation = EnsembleGeneration(ensemble_generation.value());
  Outcome<std::uint64_t> participant = in.u64();
  if (!participant.ok())
    return fail<CandidateAbstentionMessage>(participant.code(), participant.error().message);
  message.authority.participant.participant = ParticipantId(participant.value());
  Outcome<std::uint64_t> participant_generation = in.u64();
  if (!participant_generation.ok())
    return fail<CandidateAbstentionMessage>(participant_generation.code(),
                                            participant_generation.error().message);
  message.authority.participant.participant_generation =
      ParticipantGeneration(participant_generation.value());
  Outcome<std::uint64_t> worker = in.u64();
  if (!worker.ok()) return fail<CandidateAbstentionMessage>(worker.code(), worker.error().message);
  message.authority.participant.worker = WorkerId(worker.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<CandidateAbstentionMessage>(boot.code(), boot.error().message);
  message.authority.participant.worker_boot = WorkerBootId(boot.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<CandidateAbstentionMessage>(epoch.code(), epoch.error().message);
  message.authority.participant.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> execution = in.u64();
  if (!execution.ok())
    return fail<CandidateAbstentionMessage>(execution.code(), execution.error().message);
  message.authority.execution = EnsembleExecutionId(execution.value());
  Outcome<std::uint64_t> attempt = in.u64();
  if (!attempt.ok()) return fail<CandidateAbstentionMessage>(attempt.code(), attempt.error().message);
  message.authority.attempt_generation = EnsembleAttemptGeneration(attempt.value());
  Outcome<std::uint64_t> candidate = in.u64();
  if (!candidate.ok()) return fail<CandidateAbstentionMessage>(candidate.code(), candidate.error().message);
  message.authority.candidate = CandidateId(candidate.value());
  Outcome<std::uint64_t> candidate_generation = in.u64();
  if (!candidate_generation.ok())
    return fail<CandidateAbstentionMessage>(candidate_generation.code(),
                                            candidate_generation.error().message);
  message.authority.candidate_generation = CandidateGeneration(candidate_generation.value());
  Outcome<std::string> rationale = in.string(4096u);
  if (!rationale.ok()) return fail<CandidateAbstentionMessage>(rationale.code(), rationale.error().message);
  message.rationale = std::move(rationale).value();
  return message;
}

void encode(const EvaluationRequestMessage& message, Encoder& out) {
  out.u64(message.ensemble.value());
  out.u64(message.ensemble_generation.value());
  out.u64(message.execution.value());
  out.u64(message.attempt_generation.value());
  out.u64(message.judge.value());
  out.u64(message.judge_generation.value());
  out.u64(message.coordinator_epoch.value());
  out.u64(message.evaluation.value());
  out.u64(message.evaluation_generation.value());
  out.u8(static_cast<std::uint8_t>(message.role));
  out.u64(message.candidate.value());
  out.u64(message.candidate_generation.value());
  out.u32(message.criterion.id);
  out.u32(message.criterion.version);
  out.u8(static_cast<std::uint8_t>(message.criterion_kind));
  out.bytes(message.candidate_payload);
  out.string(message.domain);
}

Outcome<EvaluationRequestMessage> decode_evaluation_request(Decoder& in) {
  EvaluationRequestMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<EvaluationRequestMessage>(ensemble.code(), ensemble.error().message);
  message.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> ensemble_generation = in.u64();
  if (!ensemble_generation.ok())
    return fail<EvaluationRequestMessage>(ensemble_generation.code(), ensemble_generation.error().message);
  message.ensemble_generation = EnsembleGeneration(ensemble_generation.value());
  Outcome<std::uint64_t> execution = in.u64();
  if (!execution.ok()) return fail<EvaluationRequestMessage>(execution.code(), execution.error().message);
  message.execution = EnsembleExecutionId(execution.value());
  Outcome<std::uint64_t> attempt = in.u64();
  if (!attempt.ok()) return fail<EvaluationRequestMessage>(attempt.code(), attempt.error().message);
  message.attempt_generation = EnsembleAttemptGeneration(attempt.value());
  Outcome<std::uint64_t> judge = in.u64();
  if (!judge.ok()) return fail<EvaluationRequestMessage>(judge.code(), judge.error().message);
  message.judge = ParticipantId(judge.value());
  Outcome<std::uint64_t> judge_generation = in.u64();
  if (!judge_generation.ok())
    return fail<EvaluationRequestMessage>(judge_generation.code(), judge_generation.error().message);
  message.judge_generation = ParticipantGeneration(judge_generation.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<EvaluationRequestMessage>(epoch.code(), epoch.error().message);
  message.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> evaluation = in.u64();
  if (!evaluation.ok()) return fail<EvaluationRequestMessage>(evaluation.code(), evaluation.error().message);
  message.evaluation = EvaluationId(evaluation.value());
  Outcome<std::uint64_t> evaluation_generation = in.u64();
  if (!evaluation_generation.ok())
    return fail<EvaluationRequestMessage>(evaluation_generation.code(),
                                          evaluation_generation.error().message);
  message.evaluation_generation = EvaluationGeneration(evaluation_generation.value());
  Outcome<std::uint8_t> role = in.u8();
  if (!role.ok()) return fail<EvaluationRequestMessage>(role.code(), role.error().message);
  if (role.value() < 1u || role.value() > 5u) {
    return fail<EvaluationRequestMessage>(EnsembleError::invalid_enum_value,
                                          "evaluation request carries an unknown role");
  }
  message.role = static_cast<ParticipantRole>(role.value());
  Outcome<std::uint64_t> candidate = in.u64();
  if (!candidate.ok()) return fail<EvaluationRequestMessage>(candidate.code(), candidate.error().message);
  message.candidate = CandidateId(candidate.value());
  Outcome<std::uint64_t> candidate_generation = in.u64();
  if (!candidate_generation.ok())
    return fail<EvaluationRequestMessage>(candidate_generation.code(), candidate_generation.error().message);
  message.candidate_generation = CandidateGeneration(candidate_generation.value());
  Outcome<std::uint32_t> criterion_id = in.u32();
  if (!criterion_id.ok()) return fail<EvaluationRequestMessage>(criterion_id.code(), criterion_id.error().message);
  message.criterion.id = criterion_id.value();
  Outcome<std::uint32_t> criterion_version = in.u32();
  if (!criterion_version.ok())
    return fail<EvaluationRequestMessage>(criterion_version.code(), criterion_version.error().message);
  message.criterion.version = criterion_version.value();
  Outcome<std::uint8_t> kind = in.u8();
  if (!kind.ok()) return fail<EvaluationRequestMessage>(kind.code(), kind.error().message);
  if (kind.value() < 1u || kind.value() > 2u) {
    return fail<EvaluationRequestMessage>(EnsembleError::invalid_enum_value,
                                          "evaluation request carries an unknown criterion kind");
  }
  message.criterion_kind = static_cast<CriterionKind>(kind.value());
  Outcome<std::vector<std::byte>> payload = in.bytes(64u << 20);
  if (!payload.ok()) return fail<EvaluationRequestMessage>(payload.code(), payload.error().message);
  message.candidate_payload = std::move(payload).value();
  Outcome<std::string> domain = in.string(4096u);
  if (!domain.ok()) return fail<EvaluationRequestMessage>(domain.code(), domain.error().message);
  message.domain = std::move(domain).value();
  return message;
}

void encode(const EvaluationResultMessage& message, Encoder& out) {
  const EvaluationAuthority& authority = message.authority;
  out.u64(authority.evaluator.ensemble.value());
  out.u64(authority.evaluator.ensemble_generation.value());
  out.u64(authority.evaluator.participant.value());
  out.u64(authority.evaluator.participant_generation.value());
  out.u64(authority.evaluator.worker.value());
  out.u64(authority.evaluator.worker_boot.value());
  out.u64(authority.evaluator.coordinator_epoch.value());
  out.u64(authority.execution.value());
  out.u64(authority.attempt_generation.value());
  out.u64(authority.evaluation.value());
  out.u64(authority.evaluation_generation.value());
  out.u64(authority.candidate.value());
  out.u64(authority.candidate_generation.value());
  out.u32(message.criterion.id);
  out.u32(message.criterion.version);
  out.u8(static_cast<std::uint8_t>(message.criterion_kind));
  out.u8(static_cast<std::uint8_t>(message.verification));
  out.u8(static_cast<std::uint8_t>(message.judgment));
  out.boolean(message.score_defined);
  out.f64(message.score);
  out.f64(message.weight);
  out.bytes(message.evidence);
  out.string(message.rationale);
}

Outcome<EvaluationResultMessage> decode_evaluation_result(Decoder& in) {
  EvaluationResultMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<EvaluationResultMessage>(ensemble.code(), ensemble.error().message);
  message.authority.evaluator.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> ensemble_generation = in.u64();
  if (!ensemble_generation.ok())
    return fail<EvaluationResultMessage>(ensemble_generation.code(), ensemble_generation.error().message);
  message.authority.evaluator.ensemble_generation = EnsembleGeneration(ensemble_generation.value());
  Outcome<std::uint64_t> participant = in.u64();
  if (!participant.ok())
    return fail<EvaluationResultMessage>(participant.code(), participant.error().message);
  message.authority.evaluator.participant = ParticipantId(participant.value());
  Outcome<std::uint64_t> participant_generation = in.u64();
  if (!participant_generation.ok())
    return fail<EvaluationResultMessage>(participant_generation.code(),
                                         participant_generation.error().message);
  message.authority.evaluator.participant_generation =
      ParticipantGeneration(participant_generation.value());
  Outcome<std::uint64_t> worker = in.u64();
  if (!worker.ok()) return fail<EvaluationResultMessage>(worker.code(), worker.error().message);
  message.authority.evaluator.worker = WorkerId(worker.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<EvaluationResultMessage>(boot.code(), boot.error().message);
  message.authority.evaluator.worker_boot = WorkerBootId(boot.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<EvaluationResultMessage>(epoch.code(), epoch.error().message);
  message.authority.evaluator.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> execution = in.u64();
  if (!execution.ok()) return fail<EvaluationResultMessage>(execution.code(), execution.error().message);
  message.authority.execution = EnsembleExecutionId(execution.value());
  Outcome<std::uint64_t> attempt = in.u64();
  if (!attempt.ok()) return fail<EvaluationResultMessage>(attempt.code(), attempt.error().message);
  message.authority.attempt_generation = EnsembleAttemptGeneration(attempt.value());
  Outcome<std::uint64_t> evaluation = in.u64();
  if (!evaluation.ok()) return fail<EvaluationResultMessage>(evaluation.code(), evaluation.error().message);
  message.authority.evaluation = EvaluationId(evaluation.value());
  Outcome<std::uint64_t> evaluation_generation = in.u64();
  if (!evaluation_generation.ok())
    return fail<EvaluationResultMessage>(evaluation_generation.code(),
                                         evaluation_generation.error().message);
  message.authority.evaluation_generation = EvaluationGeneration(evaluation_generation.value());
  Outcome<std::uint64_t> candidate = in.u64();
  if (!candidate.ok()) return fail<EvaluationResultMessage>(candidate.code(), candidate.error().message);
  message.authority.candidate = CandidateId(candidate.value());
  Outcome<std::uint64_t> candidate_generation = in.u64();
  if (!candidate_generation.ok())
    return fail<EvaluationResultMessage>(candidate_generation.code(), candidate_generation.error().message);
  message.authority.candidate_generation = CandidateGeneration(candidate_generation.value());
  Outcome<std::uint32_t> criterion_id = in.u32();
  if (!criterion_id.ok()) return fail<EvaluationResultMessage>(criterion_id.code(), criterion_id.error().message);
  message.criterion.id = criterion_id.value();
  Outcome<std::uint32_t> criterion_version = in.u32();
  if (!criterion_version.ok())
    return fail<EvaluationResultMessage>(criterion_version.code(), criterion_version.error().message);
  message.criterion.version = criterion_version.value();
  Outcome<std::uint8_t> kind = in.u8();
  if (!kind.ok()) return fail<EvaluationResultMessage>(kind.code(), kind.error().message);
  if (kind.value() < 1u || kind.value() > 2u) {
    return fail<EvaluationResultMessage>(EnsembleError::invalid_enum_value,
                                          "evaluation carries an unknown criterion kind");
  }
  message.criterion_kind = static_cast<CriterionKind>(kind.value());
  Outcome<std::uint8_t> verification = in.u8();
  if (!verification.ok()) return fail<EvaluationResultMessage>(verification.code(), verification.error().message);
  if (verification.value() < 1u || verification.value() > 4u) {
    return fail<EvaluationResultMessage>(EnsembleError::invalid_enum_value,
                                          "evaluation carries an unknown verification state");
  }
  message.verification = static_cast<VerificationState>(verification.value());
  Outcome<std::uint8_t> judgment = in.u8();
  if (!judgment.ok()) return fail<EvaluationResultMessage>(judgment.code(), judgment.error().message);
  if (judgment.value() < 1u || judgment.value() > 4u) {
    return fail<EvaluationResultMessage>(EnsembleError::invalid_enum_value,
                                          "evaluation carries an unknown categorical judgment");
  }
  message.judgment = static_cast<CategoricalJudgment>(judgment.value());
  Outcome<bool> score_defined = in.boolean();
  if (!score_defined.ok())
    return fail<EvaluationResultMessage>(score_defined.code(), score_defined.error().message);
  message.score_defined = score_defined.value();
  Outcome<double> score = in.f64();
  if (!score.ok()) return fail<EvaluationResultMessage>(score.code(), score.error().message);
  message.score = score.value();
  Outcome<double> weight = in.f64();
  if (!weight.ok()) return fail<EvaluationResultMessage>(weight.code(), weight.error().message);
  message.weight = weight.value();
  Outcome<std::vector<std::byte>> evidence = in.bytes(1u << 20);
  if (!evidence.ok()) return fail<EvaluationResultMessage>(evidence.code(), evidence.error().message);
  message.evidence = std::move(evidence).value();
  Outcome<std::string> rationale = in.string(4096u);
  if (!rationale.ok()) return fail<EvaluationResultMessage>(rationale.code(), rationale.error().message);
  message.rationale = std::move(rationale).value();
  return message;
}

void encode(const CancelMessage& message, Encoder& out) {
  out.u64(message.ensemble.value());
  out.u64(message.ensemble_generation.value());
  out.u64(message.execution.value());
  out.u64(message.attempt_generation.value());
  out.u64(message.coordinator_epoch.value());
  out.u64(message.candidate.value());
  out.u64(message.candidate_generation.value());
  out.u64(message.participant.value());
  out.u64(message.participant_generation.value());
  out.string(message.reason);
}

Outcome<CancelMessage> decode_cancel(Decoder& in) {
  CancelMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<CancelMessage>(ensemble.code(), ensemble.error().message);
  message.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> ensemble_generation = in.u64();
  if (!ensemble_generation.ok())
    return fail<CancelMessage>(ensemble_generation.code(), ensemble_generation.error().message);
  message.ensemble_generation = EnsembleGeneration(ensemble_generation.value());
  Outcome<std::uint64_t> execution = in.u64();
  if (!execution.ok()) return fail<CancelMessage>(execution.code(), execution.error().message);
  message.execution = EnsembleExecutionId(execution.value());
  Outcome<std::uint64_t> attempt = in.u64();
  if (!attempt.ok()) return fail<CancelMessage>(attempt.code(), attempt.error().message);
  message.attempt_generation = EnsembleAttemptGeneration(attempt.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<CancelMessage>(epoch.code(), epoch.error().message);
  message.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> candidate = in.u64();
  if (!candidate.ok()) return fail<CancelMessage>(candidate.code(), candidate.error().message);
  message.candidate = CandidateId(candidate.value());
  Outcome<std::uint64_t> candidate_generation = in.u64();
  if (!candidate_generation.ok())
    return fail<CancelMessage>(candidate_generation.code(), candidate_generation.error().message);
  message.candidate_generation = CandidateGeneration(candidate_generation.value());
  Outcome<std::uint64_t> participant = in.u64();
  if (!participant.ok()) return fail<CancelMessage>(participant.code(), participant.error().message);
  message.participant = ParticipantId(participant.value());
  Outcome<std::uint64_t> participant_generation = in.u64();
  if (!participant_generation.ok())
    return fail<CancelMessage>(participant_generation.code(), participant_generation.error().message);
  message.participant_generation = ParticipantGeneration(participant_generation.value());
  Outcome<std::string> reason = in.string(4096u);
  if (!reason.ok()) return fail<CancelMessage>(reason.code(), reason.error().message);
  message.reason = std::move(reason).value();
  return message;
}

void encode(const DefineEnsembleMessage& message, Encoder& out) {
  out.bytes(message.encoded_spec);
}

Outcome<DefineEnsembleMessage> decode_define_ensemble(Decoder& in) {
  DefineEnsembleMessage message;
  Outcome<std::vector<std::byte>> spec = in.bytes(4u << 20);
  if (!spec.ok()) return fail<DefineEnsembleMessage>(spec.code(), spec.error().message);
  message.encoded_spec = std::move(spec).value();
  return message;
}

void encode(const DefineEnsembleAck& message, Encoder& out) {
  out.u64(message.ensemble.value());
  out.u64(message.generation.value());
  out.boolean(message.accepted);
  out.u16(static_cast<std::uint16_t>(message.code));
  out.string(message.detail);
}

Outcome<DefineEnsembleAck> decode_define_ensemble_ack(Decoder& in) {
  DefineEnsembleAck message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<DefineEnsembleAck>(ensemble.code(), ensemble.error().message);
  message.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<DefineEnsembleAck>(generation.code(), generation.error().message);
  message.generation = EnsembleGeneration(generation.value());
  Outcome<bool> accepted = in.boolean();
  if (!accepted.ok()) return fail<DefineEnsembleAck>(accepted.code(), accepted.error().message);
  message.accepted = accepted.value();
  Outcome<std::uint16_t> code = in.u16();
  if (!code.ok()) return fail<DefineEnsembleAck>(code.code(), code.error().message);
  if (code.value() > static_cast<std::uint16_t>(EnsembleError::internal_error)) {
    return fail<DefineEnsembleAck>(EnsembleError::invalid_enum_value, "invalid error code in ack");
  }
  message.code = static_cast<EnsembleError>(code.value());
  Outcome<std::string> detail = in.string(4096u);
  if (!detail.ok()) return fail<DefineEnsembleAck>(detail.code(), detail.error().message);
  message.detail = std::move(detail).value();
  return message;
}

void encode(const OpenExecutionMessage& message, Encoder& out) {
  out.u64(message.ensemble.value());
  out.u64(message.ensemble_generation.value());
}

Outcome<OpenExecutionMessage> decode_open_execution(Decoder& in) {
  OpenExecutionMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<OpenExecutionMessage>(ensemble.code(), ensemble.error().message);
  message.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<OpenExecutionMessage>(generation.code(), generation.error().message);
  message.ensemble_generation = EnsembleGeneration(generation.value());
  return message;
}

void encode(const OpenExecutionAck& message, Encoder& out) {
  out.u64(message.ensemble.value());
  out.u64(message.ensemble_generation.value());
  out.u64(message.execution.value());
  out.boolean(message.accepted);
  out.u16(static_cast<std::uint16_t>(message.code));
  out.string(message.detail);
}

Outcome<OpenExecutionAck> decode_open_execution_ack(Decoder& in) {
  OpenExecutionAck message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<OpenExecutionAck>(ensemble.code(), ensemble.error().message);
  message.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<OpenExecutionAck>(generation.code(), generation.error().message);
  message.ensemble_generation = EnsembleGeneration(generation.value());
  Outcome<std::uint64_t> execution = in.u64();
  if (!execution.ok()) return fail<OpenExecutionAck>(execution.code(), execution.error().message);
  message.execution = EnsembleExecutionId(execution.value());
  Outcome<bool> accepted = in.boolean();
  if (!accepted.ok()) return fail<OpenExecutionAck>(accepted.code(), accepted.error().message);
  message.accepted = accepted.value();
  Outcome<std::uint16_t> code = in.u16();
  if (!code.ok()) return fail<OpenExecutionAck>(code.code(), code.error().message);
  if (code.value() > static_cast<std::uint16_t>(EnsembleError::internal_error)) {
    return fail<OpenExecutionAck>(EnsembleError::invalid_enum_value, "invalid error code in ack");
  }
  message.code = static_cast<EnsembleError>(code.value());
  Outcome<std::string> detail = in.string(4096u);
  if (!detail.ok()) return fail<OpenExecutionAck>(detail.code(), detail.error().message);
  message.detail = std::move(detail).value();
  return message;
}

void encode(const FinalizeRequestMessage& message, Encoder& out) {
  out.u64(message.ensemble.value());
  out.u64(message.ensemble_generation.value());
}

Outcome<FinalizeRequestMessage> decode_finalize_request(Decoder& in) {
  FinalizeRequestMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<FinalizeRequestMessage>(ensemble.code(), ensemble.error().message);
  message.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<FinalizeRequestMessage>(generation.code(), generation.error().message);
  message.ensemble_generation = EnsembleGeneration(generation.value());
  return message;
}

void encode(const FinalizeResponseMessage& message, Encoder& out) {
  out.u64(message.ensemble.value());
  out.u64(message.ensemble_generation.value());
  out.u8(static_cast<std::uint8_t>(message.decision));
  out.boolean(message.accepted);
  out.u16(static_cast<std::uint16_t>(message.code));
  out.bytes(message.encoded_result);
  out.string(message.detail);
}

Outcome<FinalizeResponseMessage> decode_finalize_response(Decoder& in) {
  FinalizeResponseMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<FinalizeResponseMessage>(ensemble.code(), ensemble.error().message);
  message.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<FinalizeResponseMessage>(generation.code(), generation.error().message);
  message.ensemble_generation = EnsembleGeneration(generation.value());
  Outcome<std::uint8_t> decision = in.u8();
  if (!decision.ok()) return fail<FinalizeResponseMessage>(decision.code(), decision.error().message);
  if (decision.value() > 7u) {
    return fail<FinalizeResponseMessage>(EnsembleError::invalid_enum_value, "invalid decision value");
  }
  message.decision = static_cast<EnsembleDecision>(decision.value());
  Outcome<bool> accepted = in.boolean();
  if (!accepted.ok()) return fail<FinalizeResponseMessage>(accepted.code(), accepted.error().message);
  message.accepted = accepted.value();
  Outcome<std::uint16_t> code = in.u16();
  if (!code.ok()) return fail<FinalizeResponseMessage>(code.code(), code.error().message);
  if (code.value() > static_cast<std::uint16_t>(EnsembleError::internal_error)) {
    return fail<FinalizeResponseMessage>(EnsembleError::invalid_enum_value, "invalid error code");
  }
  message.code = static_cast<EnsembleError>(code.value());
  Outcome<std::vector<std::byte>> result = in.bytes(32u << 20);
  if (!result.ok()) return fail<FinalizeResponseMessage>(result.code(), result.error().message);
  message.encoded_result = std::move(result).value();
  Outcome<std::string> detail = in.string(4096u);
  if (!detail.ok()) return fail<FinalizeResponseMessage>(detail.code(), detail.error().message);
  message.detail = std::move(detail).value();
  return message;
}

void encode(const CancelEnsembleMessage& message, Encoder& out) {
  out.u64(message.ensemble.value());
  out.u64(message.ensemble_generation.value());
  out.string(message.reason);
}

Outcome<CancelEnsembleMessage> decode_cancel_ensemble(Decoder& in) {
  CancelEnsembleMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<CancelEnsembleMessage>(ensemble.code(), ensemble.error().message);
  message.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<CancelEnsembleMessage>(generation.code(), generation.error().message);
  message.ensemble_generation = EnsembleGeneration(generation.value());
  Outcome<std::string> reason = in.string(4096u);
  if (!reason.ok()) return fail<CancelEnsembleMessage>(reason.code(), reason.error().message);
  message.reason = std::move(reason).value();
  return message;
}

void encode(const QueryResultMessage& message, Encoder& out) {
  out.u64(message.ensemble.value());
  out.u64(message.ensemble_generation.value());
}

Outcome<QueryResultMessage> decode_query_result(Decoder& in) {
  QueryResultMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<QueryResultMessage>(ensemble.code(), ensemble.error().message);
  message.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<QueryResultMessage>(generation.code(), generation.error().message);
  message.ensemble_generation = EnsembleGeneration(generation.value());
  return message;
}

void encode(const ResultResponseMessage& message, Encoder& out) {
  out.u64(message.ensemble.value());
  out.u64(message.ensemble_generation.value());
  out.boolean(message.has_result);
  out.u8(static_cast<std::uint8_t>(message.decision));
  out.bytes(message.encoded_result);
  out.u16(static_cast<std::uint16_t>(message.code));
}

Outcome<ResultResponseMessage> decode_result_response(Decoder& in) {
  ResultResponseMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<ResultResponseMessage>(ensemble.code(), ensemble.error().message);
  message.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<ResultResponseMessage>(generation.code(), generation.error().message);
  message.ensemble_generation = EnsembleGeneration(generation.value());
  Outcome<bool> has_result = in.boolean();
  if (!has_result.ok()) return fail<ResultResponseMessage>(has_result.code(), has_result.error().message);
  message.has_result = has_result.value();
  Outcome<std::uint8_t> decision = in.u8();
  if (!decision.ok()) return fail<ResultResponseMessage>(decision.code(), decision.error().message);
  if (decision.value() > 7u) {
    return fail<ResultResponseMessage>(EnsembleError::invalid_enum_value, "invalid decision value");
  }
  message.decision = static_cast<EnsembleDecision>(decision.value());
  Outcome<std::vector<std::byte>> result = in.bytes(32u << 20);
  if (!result.ok()) return fail<ResultResponseMessage>(result.code(), result.error().message);
  message.encoded_result = std::move(result).value();
  Outcome<std::uint16_t> code = in.u16();
  if (!code.ok()) return fail<ResultResponseMessage>(code.code(), code.error().message);
  if (code.value() > static_cast<std::uint16_t>(EnsembleError::internal_error)) {
    return fail<ResultResponseMessage>(EnsembleError::invalid_enum_value, "invalid error code");
  }
  message.code = static_cast<EnsembleError>(code.value());
  return message;
}

void encode(const InspectRequestMessage& message, Encoder& out) {
  out.u64(message.ensemble.value());
}

Outcome<InspectRequestMessage> decode_inspect_request(Decoder& in) {
  InspectRequestMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<InspectRequestMessage>(ensemble.code(), ensemble.error().message);
  message.ensemble = EnsembleId(ensemble.value());
  return message;
}

void encode(const InspectResponseMessage& message, Encoder& out) {
  out.u64(message.ensemble.value());
  out.u64(message.ensemble_generation.value());
  out.bytes(message.encoded_report);
  out.u16(static_cast<std::uint16_t>(message.code));
}

Outcome<InspectResponseMessage> decode_inspect_response(Decoder& in) {
  InspectResponseMessage message;
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<InspectResponseMessage>(ensemble.code(), ensemble.error().message);
  message.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<InspectResponseMessage>(generation.code(), generation.error().message);
  message.ensemble_generation = EnsembleGeneration(generation.value());
  Outcome<std::vector<std::byte>> report = in.bytes(8u << 20);
  if (!report.ok()) return fail<InspectResponseMessage>(report.code(), report.error().message);
  message.encoded_report = std::move(report).value();
  Outcome<std::uint16_t> code = in.u16();
  if (!code.ok()) return fail<InspectResponseMessage>(code.code(), code.error().message);
  if (code.value() > static_cast<std::uint16_t>(EnsembleError::internal_error)) {
    return fail<InspectResponseMessage>(EnsembleError::invalid_enum_value, "invalid error code");
  }
  message.code = static_cast<EnsembleError>(code.value());
  return message;
}

void encode(const StatusResponseMessage& message, Encoder& out) {
  out.u16(static_cast<std::uint16_t>(message.code));
  out.string(message.detail);
}

Outcome<StatusResponseMessage> decode_status_response(Decoder& in) {
  StatusResponseMessage message;
  Outcome<std::uint16_t> code = in.u16();
  if (!code.ok()) return fail<StatusResponseMessage>(code.code(), code.error().message);
  if (code.value() > static_cast<std::uint16_t>(EnsembleError::internal_error)) {
    return fail<StatusResponseMessage>(EnsembleError::invalid_enum_value, "invalid error code");
  }
  message.code = static_cast<EnsembleError>(code.value());
  Outcome<std::string> detail = in.string(4096u);
  if (!detail.ok()) return fail<StatusResponseMessage>(detail.code(), detail.error().message);
  message.detail = std::move(detail).value();
  return message;
}

void encode(const ErrorResponseMessage& message, Encoder& out) {
  out.u16(static_cast<std::uint16_t>(message.code));
  out.u64(message.correlation.value());
  out.string(message.detail);
}

Outcome<ErrorResponseMessage> decode_error_response(Decoder& in) {
  ErrorResponseMessage message;
  Outcome<std::uint16_t> code = in.u16();
  if (!code.ok()) return fail<ErrorResponseMessage>(code.code(), code.error().message);
  if (code.value() > static_cast<std::uint16_t>(EnsembleError::internal_error)) {
    return fail<ErrorResponseMessage>(EnsembleError::invalid_enum_value, "invalid error code");
  }
  message.code = static_cast<EnsembleError>(code.value());
  Outcome<std::uint64_t> correlation = in.u64();
  if (!correlation.ok()) return fail<ErrorResponseMessage>(correlation.code(), correlation.error().message);
  message.correlation = RequestId(correlation.value());
  Outcome<std::string> detail = in.string(4096u);
  if (!detail.ok()) return fail<ErrorResponseMessage>(detail.code(), detail.error().message);
  message.detail = std::move(detail).value();
  return message;
}

void encode(const HeartbeatMessage& message, Encoder& out) {
  out.u64(message.worker.value());
  out.u64(message.worker_boot.value());
  out.u64(message.coordinator_epoch.value());
  out.u64(message.sequence.value());
}

Outcome<HeartbeatMessage> decode_heartbeat(Decoder& in) {
  HeartbeatMessage message;
  Outcome<std::uint64_t> worker = in.u64();
  if (!worker.ok()) return fail<HeartbeatMessage>(worker.code(), worker.error().message);
  message.worker = WorkerId(worker.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<HeartbeatMessage>(boot.code(), boot.error().message);
  message.worker_boot = WorkerBootId(boot.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<HeartbeatMessage>(epoch.code(), epoch.error().message);
  message.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> sequence = in.u64();
  if (!sequence.ok()) return fail<HeartbeatMessage>(sequence.code(), sequence.error().message);
  message.sequence = Sequence(sequence.value());
  return message;
}

Status validate(const DispatchCandidateMessage& message) {
  if (!message.ensemble.valid() || !message.ensemble_generation.valid() || !message.execution.valid() ||
      !message.attempt_generation.valid() || !message.participant.valid() ||
      !message.participant_generation.valid() || !message.coordinator_epoch.valid() ||
      !message.candidate.valid() || !message.candidate_generation.valid() || !message.stage.valid()) {
    return fail(EnsembleError::invalid_identity_combination,
                "dispatch is missing part of its identity");
  }
  if (!role_produces_candidates(message.role)) {
    return fail(EnsembleError::participant_role_mismatch,
                "dispatch targets a role that does not produce candidates");
  }
  if (message.attempt == 0u) {
    return fail(EnsembleError::invalid_argument, "dispatch attempt number must be positive");
  }
  return ok_status();
}

Status validate(const CandidateResultMessage& message) {
  return validate_authority(message.authority);
}

Status validate(const CandidateFailureMessage& message) {
  return validate_authority(message.authority);
}

Status validate(const CandidateAbstentionMessage& message) {
  return validate_authority(message.authority);
}

Status validate(const EvaluationRequestMessage& message) {
  if (!message.ensemble.valid() || !message.ensemble_generation.valid() || !message.execution.valid() ||
      !message.attempt_generation.valid() || !message.judge.valid() ||
      !message.judge_generation.valid() || !message.coordinator_epoch.valid() ||
      !message.evaluation.valid() || !message.evaluation_generation.valid() ||
      !message.candidate.valid() || !message.candidate_generation.valid()) {
    return fail(EnsembleError::invalid_identity_combination,
                "evaluation request is missing part of its identity");
  }
  if (!message.criterion.valid()) {
    return fail(EnsembleError::invalid_argument, "evaluation request names a zero criterion");
  }
  if (!role_evaluates(message.role)) {
    return fail(EnsembleError::participant_role_mismatch,
                "evaluation request targets a role that does not evaluate");
  }
  return ok_status();
}

Status validate(const EvaluationResultMessage& message) {
  const Status authority = validate_authority(message.authority);
  if (!authority.ok()) {
    return authority;
  }
  if (!message.criterion.valid()) {
    return fail(EnsembleError::invalid_argument, "evaluation names a zero criterion");
  }
  return ok_status();
}

}  // namespace ensemble_fabric
