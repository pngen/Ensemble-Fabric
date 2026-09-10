#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ensemble_fabric/authority.hpp"
#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/outcome.hpp"
#include "ensemble_fabric/role.hpp"

namespace ensemble_fabric {

/// Explicit candidate lifecycle.
///
/// Not every declared candidate produces an output, and not every output is a
/// valid candidate.  Each state names the authority that may leave it.
enum class CandidateState : std::uint8_t {
  declared = 1,
  dispatched = 2,
  output_received = 3,
  valid = 4,
  invalid = 5,
  eligible = 6,
  rejected = 7,
  selected = 8,
  superseded = 9,
  cancelled = 10,
  failed = 11,
  abstained = 12,
  retired = 13,
};

[[nodiscard]] std::string_view to_string(CandidateState state) noexcept;
[[nodiscard]] std::optional<CandidateState> parse_candidate_state(std::string_view text) noexcept;

/// Guarded transition table.  An illegal transition is a typed rejection, never
/// a silent no-op.
[[nodiscard]] bool is_legal_transition(CandidateState from, CandidateState to) noexcept;
[[nodiscard]] bool is_terminal(CandidateState state) noexcept;
[[nodiscard]] bool is_content_state(CandidateState state) noexcept;
[[nodiscard]] bool permits_output(CandidateState state) noexcept;

/// One physical attempt to fill a logical candidate slot.  Retries create a new
/// attempt generation under the same logical CandidateId.
struct CandidateAttempt {
  std::uint32_t attempt{1};
  ParticipantId participant;
  ParticipantGeneration participant_generation;
  WorkerId worker;
  WorkerBootId worker_boot;
  EnsembleAttemptGeneration attempt_generation;
  CandidateState outcome{CandidateState::declared};
  EnsembleError failure{EnsembleError::ok};
};

/// Submission of a produced candidate payload by an authoritative participant.
struct CandidateSubmission {
  CandidateAuthority authority;
  std::vector<std::byte> payload;
};

/// Submission of a typed participant failure for one candidate slot.
struct CandidateFailureSubmission {
  CandidateAuthority authority;
  std::uint32_t participant_failure{0};
};

/// Submission of an abstention: the participant participated and produced no
/// content, but did not fail.  Abstention is evidence and never silently
/// becomes a vote.
struct CandidateAbstentionSubmission {
  CandidateAuthority authority;
  std::string rationale;
};

/// Point-in-time inspection view of one candidate.
struct CandidateSnapshot {
  CandidateId id;
  CandidateGeneration generation;
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  EnsembleExecutionId execution;
  EnsembleAttemptGeneration attempt_generation;
  ParticipantId participant;
  ParticipantGeneration participant_generation;
  ParticipantRole role{ParticipantRole::candidate};
  StageId stage;
  CandidateState state{CandidateState::declared};
  std::uint32_t attempt{1};
  std::uint32_t attempts_used{0};
  std::string domain;
  std::vector<std::byte> payload;
  std::uint64_t payload_digest{0};
  std::uint32_t payload_bytes{0};
  Sequence received_sequence;
  CoordinatorEpoch coordinator_epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  EnsembleError failure{EnsembleError::ok};
  std::string failure_detail;
  std::uint32_t evaluation_count{0};
  std::uint32_t hard_failures{0};
  bool fallback{false};
  bool current{true};
  std::string elimination_reason;
};

/// Bounded, deterministic fingerprint of a candidate payload.  Used to detect
/// substitution and to make duplicate-completion detection exact.
[[nodiscard]] std::uint64_t fingerprint_payload(const std::vector<std::byte>& payload) noexcept;

}  // namespace ensemble_fabric
