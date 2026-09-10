#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ensemble_fabric/authority.hpp"
#include "ensemble_fabric/evaluation.hpp"
#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/limits.hpp"
#include "ensemble_fabric/outcome.hpp"
#include "ensemble_fabric/participant.hpp"
#include "ensemble_fabric/persistence.hpp"
#include "ensemble_fabric/result.hpp"
#include "ensemble_fabric/role.hpp"

namespace ensemble_fabric {

/// Wire message kinds.  Unknown values are rejected explicitly rather than being
/// ignored, because an ignored message desynchronizes the session.
enum class MessageType : std::uint16_t {
  hello = 1,
  hello_ack = 2,
  register_participant = 3,
  register_ack = 4,
  heartbeat = 5,
  dispatch_candidate = 6,
  candidate_result = 7,
  candidate_failure = 8,
  candidate_abstention = 9,
  evaluation_request = 10,
  evaluation_result = 11,
  cancel = 12,
  define_ensemble = 13,
  define_ensemble_ack = 14,
  open_execution = 15,
  open_execution_ack = 16,
  finalize_request = 17,
  finalize_response = 18,
  inspect_request = 19,
  inspect_response = 20,
  worker_ready = 21,
  goodbye = 22,
  error_response = 23,
  cancel_ensemble = 24,
  query_result = 25,
  result_response = 26,
  status_response = 27,
};

[[nodiscard]] std::string_view to_string(MessageType type) noexcept;
[[nodiscard]] std::optional<MessageType> parse_message_type(std::uint16_t value) noexcept;

/// Fixed frame header size.  The header is decoded before any payload allocation
/// and carries the length, so an oversized frame is rejected without allocating.
inline constexpr std::size_t frame_header_bytes = 64;
inline constexpr std::uint32_t frame_magic = 0x42464E45u;  // 'ENFB'

/// A decoded frame.
struct Frame {
  std::uint16_t version{0};
  MessageType type{MessageType::error_response};
  std::uint16_t flags{0};
  RequestId correlation;
  CoordinatorEpoch epoch;
  WorkerBootId worker_boot;
  Sequence sequence;
  std::vector<std::byte> payload;
};

/// Encodes a frame, including magic, version, bounded length, correlation
/// identity, authority identities, and an integrity checksum.
[[nodiscard]] Outcome<std::vector<std::byte>> encode_frame(const Frame& frame,
                                                           const Limits& limits);

/// Result of one incremental decode step.
enum class DecodeStatus : std::uint8_t {
  complete = 0,
  need_more_data = 1,
  failed = 2,
};

struct DecodeResult {
  DecodeStatus status{DecodeStatus::need_more_data};
  Frame frame;
  Error error;
  std::size_t consumed{0};
};

/// Decodes exactly one frame from the front of p buffer.  Bounded: a declared
/// payload length larger than the configured maximum frame size fails
/// immediately, before any allocation.
[[nodiscard]] DecodeResult decode_frame(std::span<const std::byte> buffer, const Limits& limits);

// ---------------------------------------------------------------------------
// Message payloads.  Each payload is encoded with the bounded Encoder and
// decoded with a Decoder that must be consumed exactly.
// ---------------------------------------------------------------------------

struct HelloMessage {
  WorkerId worker;
  WorkerBootId worker_boot;
  CoordinatorEpoch coordinator_epoch;
  CompatibilityProfile profile;
  std::string label;
};

struct WorkerReadyMessage {
  WorkerId worker;
  WorkerBootId worker_boot;
  CoordinatorEpoch coordinator_epoch;
  ParticipantId participant;
  RoleSet roles;
  CompatibilityProfile profile;
  std::string label;
};

struct CoordinatorAck {
  CoordinatorEpoch coordinator_epoch;
  WorkerBootId coordinator_boot;
  std::uint32_t status_code{0};
  bool accepted{false};
  std::string detail;
};

struct RegistrationAck {
  CoordinatorEpoch coordinator_epoch;
  ParticipantId participant;
  ParticipantGeneration participant_generation;
  bool accepted{false};
  EnsembleError code{EnsembleError::ok};
  std::string detail;
};

struct RegisterParticipantMessage {
  ParticipantRegistration registration;
};

struct DispatchCandidateMessage {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  EnsembleExecutionId execution;
  EnsembleAttemptGeneration attempt_generation;
  ParticipantId participant;
  ParticipantGeneration participant_generation;
  CoordinatorEpoch coordinator_epoch;
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  ParticipantRole role{ParticipantRole::candidate};
  StageId stage;
  std::uint32_t attempt{1};
  std::uint32_t budget_units{0};
  std::uint64_t deterministic_seed{0};
  std::string domain;
  std::vector<std::byte> request;
};

struct CandidateResultMessage {
  CandidateAuthority authority;
  std::vector<std::byte> payload;
  std::uint32_t produced_units{0};
};

struct CandidateFailureMessage {
  CandidateAuthority authority;
  ParticipantFailureKind kind{ParticipantFailureKind::internal_error};
  std::string detail;
};

struct CandidateAbstentionMessage {
  CandidateAuthority authority;
  std::string rationale;
};

struct EvaluationRequestMessage {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  EnsembleExecutionId execution;
  EnsembleAttemptGeneration attempt_generation;
  ParticipantId judge;
  ParticipantGeneration judge_generation;
  CoordinatorEpoch coordinator_epoch;
  EvaluationId evaluation;
  EvaluationGeneration evaluation_generation;
  ParticipantRole role{ParticipantRole::judge};
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  CriterionRef criterion;
  CriterionKind criterion_kind{CriterionKind::ranking_signal};
  std::vector<std::byte> candidate_payload;
  std::string domain;
};

struct EvaluationResultMessage {
  EvaluationAuthority authority;
  CriterionRef criterion;
  CriterionKind criterion_kind{CriterionKind::ranking_signal};
  VerificationState verification{VerificationState::unknown};
  CategoricalJudgment judgment{CategoricalJudgment::unknown};
  bool score_defined{false};
  double score{0.0};
  double weight{1.0};
  std::vector<std::byte> evidence;
  std::string rationale;
};

struct CancelMessage {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  EnsembleExecutionId execution;
  EnsembleAttemptGeneration attempt_generation;
  CoordinatorEpoch coordinator_epoch;
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  ParticipantId participant;
  ParticipantGeneration participant_generation;
  std::string reason;
};

struct DefineEnsembleMessage {
  std::vector<std::byte> encoded_spec;
};

struct DefineEnsembleAck {
  EnsembleId ensemble;
  EnsembleGeneration generation;
  bool accepted{false};
  EnsembleError code{EnsembleError::ok};
  std::string detail;
};

struct OpenExecutionMessage {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
};

struct OpenExecutionAck {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  EnsembleExecutionId execution;
  bool accepted{false};
  EnsembleError code{EnsembleError::ok};
  std::string detail;
};

struct FinalizeRequestMessage {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
};

struct FinalizeResponseMessage {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  EnsembleDecision decision{EnsembleDecision::pending};
  bool accepted{false};
  EnsembleError code{EnsembleError::ok};
  std::vector<std::byte> encoded_result;
  std::string detail;
};

struct CancelEnsembleMessage {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  std::string reason;
};

struct QueryResultMessage {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
};

struct ResultResponseMessage {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  bool has_result{false};
  EnsembleDecision decision{EnsembleDecision::pending};
  std::vector<std::byte> encoded_result;
  EnsembleError code{EnsembleError::ok};
};

struct InspectRequestMessage {
  EnsembleId ensemble;
};

struct InspectResponseMessage {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  std::vector<std::byte> encoded_report;
  EnsembleError code{EnsembleError::ok};
};

/// Generic acknowledgement for commands that have no dedicated response type.
struct StatusResponseMessage {
  EnsembleError code{EnsembleError::ok};
  std::string detail;
};

struct ErrorResponseMessage {
  EnsembleError code{EnsembleError::internal_error};
  RequestId correlation;
  std::string detail;
};

struct HeartbeatMessage {
  WorkerId worker;
  WorkerBootId worker_boot;
  CoordinatorEpoch coordinator_epoch;
  Sequence sequence;
};

// Encoders/decoders.  Every decoder requires exact payload consumption.
void encode(const HelloMessage& message, Encoder& out);
[[nodiscard]] Outcome<HelloMessage> decode_hello(Decoder& in);
void encode(const WorkerReadyMessage& message, Encoder& out);
[[nodiscard]] Outcome<WorkerReadyMessage> decode_worker_ready(Decoder& in);
void encode(const CoordinatorAck& message, Encoder& out);
[[nodiscard]] Outcome<CoordinatorAck> decode_coordinator_ack(Decoder& in);
void encode(const RegistrationAck& message, Encoder& out);
[[nodiscard]] Outcome<RegistrationAck> decode_registration_ack(Decoder& in);
void encode(const RegisterParticipantMessage& message, Encoder& out);
[[nodiscard]] Outcome<RegisterParticipantMessage> decode_register_participant(Decoder& in);
void encode(const DispatchCandidateMessage& message, Encoder& out);
[[nodiscard]] Outcome<DispatchCandidateMessage> decode_dispatch_candidate(Decoder& in);
void encode(const CandidateResultMessage& message, Encoder& out);
[[nodiscard]] Outcome<CandidateResultMessage> decode_candidate_result(Decoder& in);
void encode(const CandidateFailureMessage& message, Encoder& out);
[[nodiscard]] Outcome<CandidateFailureMessage> decode_candidate_failure(Decoder& in);
void encode(const CandidateAbstentionMessage& message, Encoder& out);
[[nodiscard]] Outcome<CandidateAbstentionMessage> decode_candidate_abstention(Decoder& in);
void encode(const EvaluationRequestMessage& message, Encoder& out);
[[nodiscard]] Outcome<EvaluationRequestMessage> decode_evaluation_request(Decoder& in);
void encode(const EvaluationResultMessage& message, Encoder& out);
[[nodiscard]] Outcome<EvaluationResultMessage> decode_evaluation_result(Decoder& in);
void encode(const CancelMessage& message, Encoder& out);
[[nodiscard]] Outcome<CancelMessage> decode_cancel(Decoder& in);
void encode(const DefineEnsembleMessage& message, Encoder& out);
[[nodiscard]] Outcome<DefineEnsembleMessage> decode_define_ensemble(Decoder& in);
void encode(const DefineEnsembleAck& message, Encoder& out);
[[nodiscard]] Outcome<DefineEnsembleAck> decode_define_ensemble_ack(Decoder& in);
void encode(const OpenExecutionMessage& message, Encoder& out);
[[nodiscard]] Outcome<OpenExecutionMessage> decode_open_execution(Decoder& in);
void encode(const OpenExecutionAck& message, Encoder& out);
[[nodiscard]] Outcome<OpenExecutionAck> decode_open_execution_ack(Decoder& in);
void encode(const FinalizeRequestMessage& message, Encoder& out);
[[nodiscard]] Outcome<FinalizeRequestMessage> decode_finalize_request(Decoder& in);
void encode(const FinalizeResponseMessage& message, Encoder& out);
[[nodiscard]] Outcome<FinalizeResponseMessage> decode_finalize_response(Decoder& in);
void encode(const CancelEnsembleMessage& message, Encoder& out);
[[nodiscard]] Outcome<CancelEnsembleMessage> decode_cancel_ensemble(Decoder& in);
void encode(const QueryResultMessage& message, Encoder& out);
[[nodiscard]] Outcome<QueryResultMessage> decode_query_result(Decoder& in);
void encode(const ResultResponseMessage& message, Encoder& out);
[[nodiscard]] Outcome<ResultResponseMessage> decode_result_response(Decoder& in);
void encode(const InspectRequestMessage& message, Encoder& out);
[[nodiscard]] Outcome<InspectRequestMessage> decode_inspect_request(Decoder& in);
void encode(const InspectResponseMessage& message, Encoder& out);
[[nodiscard]] Outcome<InspectResponseMessage> decode_inspect_response(Decoder& in);
void encode(const StatusResponseMessage& message, Encoder& out);
[[nodiscard]] Outcome<StatusResponseMessage> decode_status_response(Decoder& in);
void encode(const ErrorResponseMessage& message, Encoder& out);
[[nodiscard]] Outcome<ErrorResponseMessage> decode_error_response(Decoder& in);
void encode(const HeartbeatMessage& message, Encoder& out);
[[nodiscard]] Outcome<HeartbeatMessage> decode_heartbeat(Decoder& in);

/// Structural identity validation applied to every decoded message.  Rejects
/// partially populated identities and impossible identity combinations.
[[nodiscard]] Status validate(const DispatchCandidateMessage& message);
[[nodiscard]] Status validate(const CandidateResultMessage& message);
[[nodiscard]] Status validate(const CandidateFailureMessage& message);
[[nodiscard]] Status validate(const CandidateAbstentionMessage& message);
[[nodiscard]] Status validate(const EvaluationRequestMessage& message);
[[nodiscard]] Status validate(const EvaluationResultMessage& message);

}  // namespace ensemble_fabric
