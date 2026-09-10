#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ensemble_fabric/arbitration.hpp"
#include "ensemble_fabric/candidate.hpp"
#include "ensemble_fabric/consensus.hpp"
#include "ensemble_fabric/evaluation.hpp"
#include "ensemble_fabric/explanation.hpp"
#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/participant.hpp"

namespace ensemble_fabric {

/// Lifecycle decision attached to an ensemble execution generation.
enum class EnsembleDecision : std::uint8_t {
  pending = 0,
  committed = 1,
  cancelled = 2,
  superseded = 3,
  failed = 4,
  revalidation_required = 5,
  no_eligible_candidate = 6,
  fallback_required = 7,
};

[[nodiscard]] std::string_view to_string(EnsembleDecision decision) noexcept;

/// Per-participant contribution record carried by the final result.
struct ParticipantOutcome {
  ParticipantId id;
  ParticipantGeneration generation;
  RoleSet roles;
  bool required{true};
  ParticipantStatus final_status{ParticipantStatus::revalidation_required};
  std::uint32_t candidates{0};
  std::uint32_t candidates_valid{0};
  std::uint32_t candidates_failed{0};
  std::uint32_t candidates_abstained{0};
  std::uint32_t evaluations{0};
  std::uint32_t retries{0};
  bool fallback{false};
  bool authoritative{false};
};

/// The commit record makes the exactly-once logical commit auditable: one
/// record per (ensemble execution, attempt) that produced an authoritative result.
struct CommitRecord {
  ResultId result;
  ResultGeneration generation;
  Sequence sequence;
  CoordinatorEpoch coordinator_epoch;
  EnsembleExecutionId execution;
  EnsembleAttemptGeneration attempt_generation;
  EnsembleDecision decision{EnsembleDecision::pending};
  std::uint64_t commit_fingerprint{0};
};

/// The authoritative ensemble result.  Returning only a payload string would
/// discard the evidence that makes the payload authoritative.
struct EnsembleResult {
  ResultId id;
  ResultGeneration generation;
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  EnsembleExecutionId execution;
  EnsembleAttemptGeneration attempt_generation;
  CoordinatorEpoch coordinator_epoch;
  EnsembleDecision decision{EnsembleDecision::pending};

  bool has_selected{false};
  CandidateId selected_candidate;
  CandidateGeneration selected_candidate_generation;
  ParticipantId selected_participant;
  ParticipantGeneration selected_participant_generation;
  ParticipantRole selected_role{ParticipantRole::candidate};

  std::vector<std::byte> payload;
  std::uint64_t payload_digest{0};
  std::uint32_t payload_bytes{0};

  QuorumReport quorum;
  ConsensusReport consensus;
  ArbitrationReport arbitration;
  std::vector<ParticipantOutcome> participants;
  std::vector<CandidateSnapshot> candidates;
  std::vector<EvaluationSnapshot> evaluations;
  std::vector<ExplanationEntry> explanation;

  bool fallback_used{false};
  std::uint32_t fallback_depth{0};
  Sequence commit_sequence;
  std::uint64_t commit_fingerprint{0};

  [[nodiscard]] bool authoritative() const noexcept {
    return decision == EnsembleDecision::committed && has_selected;
  }
  [[nodiscard]] std::string render() const;
};

/// Fingerprint of everything that makes a commit authoritative.  Two commits
/// with the same fingerprint are idempotent repeats; different fingerprints for
/// the same execution generation are a conflict and are rejected.
[[nodiscard]] std::uint64_t commit_fingerprint(const EnsembleResult& result) noexcept;

}  // namespace ensemble_fabric
