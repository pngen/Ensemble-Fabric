#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

namespace {

std::vector<std::byte> frame_bytes(MessageType type, std::vector<std::byte> payload,
                                   Limits limits = Limits::defaults()) {
  Frame frame;
  frame.type = type;
  frame.correlation = RequestId(11);
  frame.epoch = CoordinatorEpoch(3);
  frame.worker_boot = WorkerBootId(9);
  frame.sequence = Sequence(4);
  frame.payload = std::move(payload);
  Outcome<std::vector<std::byte>> encoded = encode_frame(frame, limits);
  return encoded.ok() ? encoded.value() : std::vector<std::byte>{};
}

}

EF_TEST(a_well_formed_frame_round_trips) {
  const std::vector<std::byte> bytes = frame_bytes(MessageType::goodbye, {});
  EF_CHECK(!bytes.empty());
  const DecodeResult decoded = decode_frame(std::span<const std::byte>(bytes.data(), bytes.size()),
                                            Limits::defaults());
  EF_CHECK(decoded.status == DecodeStatus::complete);
  EF_CHECK(decoded.frame.type == MessageType::goodbye);
  EF_CHECK_EQ(decoded.frame.correlation.value(), 11u);
  EF_CHECK_EQ(decoded.frame.epoch.value(), 3u);
  EF_CHECK_EQ(decoded.consumed, bytes.size());
}

EF_TEST(a_partial_frame_asks_for_more_data) {
  const std::vector<std::byte> bytes = frame_bytes(MessageType::heartbeat, {});
  const DecodeResult decoded =
      decode_frame(std::span<const std::byte>(bytes.data(), bytes.size() - 4u), Limits::defaults());
  EF_CHECK(decoded.status == DecodeStatus::need_more_data);
}

EF_TEST(bad_magic_is_rejected) {
  std::vector<std::byte> bytes = frame_bytes(MessageType::goodbye, {});
  bytes[0] = std::byte{0x00};
  const DecodeResult decoded = decode_frame(std::span<const std::byte>(bytes.data(), bytes.size()),
                                            Limits::defaults());
  EF_CHECK(decoded.status == DecodeStatus::failed);
  EF_CHECK(decoded.error.code == EnsembleError::malformed_frame);
}

EF_TEST(a_bad_checksum_is_rejected) {
  std::vector<std::byte> bytes = frame_bytes(MessageType::goodbye, {std::byte{0x01}});
  // Corrupt a payload byte: the reserved words and every other validated field
  // stay intact, so only the integrity check can reject this frame.
  bytes[bytes.size() - 1u] =
      static_cast<std::byte>(static_cast<unsigned char>(bytes.back()) ^ 0x5Au);
  const DecodeResult decoded = decode_frame(std::span<const std::byte>(bytes.data(), bytes.size()),
                                            Limits::defaults());
  EF_CHECK(decoded.status == DecodeStatus::failed);
  EF_CHECK(decoded.error.code == EnsembleError::checksum_mismatch);
}

EF_TEST(an_unknown_message_type_is_rejected) {
  std::vector<std::byte> bytes = frame_bytes(MessageType::goodbye, {});
  bytes[8] = std::byte{0xE8};
  bytes[9] = std::byte{0x03};
  const DecodeResult decoded = decode_frame(std::span<const std::byte>(bytes.data(), bytes.size()),
                                            Limits::defaults());
  EF_CHECK(decoded.status == DecodeStatus::failed);
  EF_CHECK(decoded.error.code == EnsembleError::invalid_enum_value);
}

EF_TEST(an_unsupported_protocol_version_is_rejected) {
  std::vector<std::byte> bytes = frame_bytes(MessageType::goodbye, {});
  bytes[4] = std::byte{0x63};
  const DecodeResult decoded = decode_frame(std::span<const std::byte>(bytes.data(), bytes.size()),
                                            Limits::defaults());
  EF_CHECK(decoded.status == DecodeStatus::failed);
  EF_CHECK(decoded.error.code == EnsembleError::unsupported_version);
}

EF_TEST(an_oversized_declared_length_is_rejected_before_allocation) {
  std::vector<std::byte> bytes = frame_bytes(MessageType::goodbye, {});
  bytes[12] = std::byte{0xFF};
  bytes[13] = std::byte{0xFF};
  bytes[14] = std::byte{0xFF};
  bytes[15] = std::byte{0x7F};
  const DecodeResult decoded = decode_frame(std::span<const std::byte>(bytes.data(), bytes.size()),
                                            Limits::defaults());
  EF_CHECK(decoded.status == DecodeStatus::failed);
  EF_CHECK(decoded.error.code == EnsembleError::payload_too_large);
}

EF_TEST(non_zero_reserved_words_are_rejected) {
  std::vector<std::byte> bytes = frame_bytes(MessageType::goodbye, {});
  bytes[48] = std::byte{0x01};
  const DecodeResult decoded = decode_frame(std::span<const std::byte>(bytes.data(), bytes.size()),
                                            Limits::defaults());
  EF_CHECK(decoded.status == DecodeStatus::failed);
  EF_CHECK(decoded.error.code == EnsembleError::malformed_frame);
}

EF_TEST(an_unknown_header_size_is_rejected) {
  std::vector<std::byte> bytes = frame_bytes(MessageType::goodbye, {});
  bytes[6] = std::byte{0x20};
  const DecodeResult decoded = decode_frame(std::span<const std::byte>(bytes.data(), bytes.size()),
                                            Limits::defaults());
  EF_CHECK(decoded.status == DecodeStatus::failed);
}

EF_TEST(message_payloads_round_trip_and_reject_trailing_bytes) {
  CandidateResultMessage message;
  message.authority.participant.ensemble = EnsembleId(1);
  message.authority.participant.ensemble_generation = EnsembleGeneration(1);
  message.authority.participant.participant = ParticipantId(2);
  message.authority.participant.participant_generation = ParticipantGeneration(1);
  message.authority.participant.worker = WorkerId(3);
  message.authority.participant.worker_boot = WorkerBootId(4);
  message.authority.participant.coordinator_epoch = CoordinatorEpoch(5);
  message.authority.execution = EnsembleExecutionId(6);
  message.authority.attempt_generation = EnsembleAttemptGeneration(1);
  message.authority.candidate = CandidateId(7);
  message.authority.candidate_generation = CandidateGeneration(2);
  message.payload = {std::byte{0x41}, std::byte{0x42}};
  message.produced_units = 12u;

  Encoder encoder;
  encode(message, encoder);
  const std::vector<std::byte> bytes = encoder.buffer();
  Decoder decoder(std::span<const std::byte>(bytes.data(), bytes.size()));
  Outcome<CandidateResultMessage> decoded = decode_candidate_result(decoder);
  EF_REQUIRE_OK(decoded);
  EF_CHECK_EQ(decoded.value().payload.size(), 2u);
  EF_CHECK_EQ(decoded.value().authority.candidate_generation.value(), 2u);
  EF_REQUIRE_OK(decoder.require_exhausted("candidate result"));

  Encoder extended;
  encode(message, extended);
  extended.u8(0x00);
  const std::vector<std::byte> longer = extended.buffer();
  Decoder trailing(std::span<const std::byte>(longer.data(), longer.size()));
  Outcome<CandidateResultMessage> second = decode_candidate_result(trailing);
  EF_REQUIRE_OK(second);
  EF_CHECK_ERR(trailing.require_exhausted("candidate result"), EnsembleError::trailing_data);
}

EF_TEST(a_dispatch_to_a_non_producing_role_is_rejected) {
  DispatchCandidateMessage message;
  message.ensemble = EnsembleId(1);
  message.ensemble_generation = EnsembleGeneration(1);
  message.execution = EnsembleExecutionId(1);
  message.attempt_generation = EnsembleAttemptGeneration(1);
  message.participant = ParticipantId(1);
  message.participant_generation = ParticipantGeneration(1);
  message.coordinator_epoch = CoordinatorEpoch(1);
  message.candidate = CandidateId(1);
  message.candidate_generation = CandidateGeneration(1);
  message.role = ParticipantRole::judge;
  message.stage = StageId(1);
  message.attempt = 1u;
  EF_CHECK_ERR(validate(message), EnsembleError::participant_role_mismatch);
  message.role = ParticipantRole::candidate;
  EF_REQUIRE_OK(validate(message));
}

EF_TEST(a_dispatch_missing_part_of_its_identity_is_rejected) {
  DispatchCandidateMessage message;
  message.ensemble = EnsembleId(1);
  message.role = ParticipantRole::candidate;
  message.attempt = 1u;
  EF_CHECK_ERR(validate(message), EnsembleError::invalid_identity_combination);
}

EF_TEST(an_evaluation_request_must_name_a_real_criterion) {
  EvaluationRequestMessage message;
  message.ensemble = EnsembleId(1);
  message.ensemble_generation = EnsembleGeneration(1);
  message.execution = EnsembleExecutionId(1);
  message.attempt_generation = EnsembleAttemptGeneration(1);
  message.judge = ParticipantId(1);
  message.judge_generation = ParticipantGeneration(1);
  message.coordinator_epoch = CoordinatorEpoch(1);
  message.evaluation = EvaluationId(1);
  message.evaluation_generation = EvaluationGeneration(1);
  message.role = ParticipantRole::judge;
  message.candidate = CandidateId(1);
  message.candidate_generation = CandidateGeneration(1);
  EF_CHECK_ERR(validate(message), EnsembleError::invalid_argument);
  message.criterion = CriterionRef{1u, 1u};
  EF_REQUIRE_OK(validate(message));
}

EF_TEST_MAIN
