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

/// Evaluation is first-class evidence, not a detached score.
///
/// An evaluation always names its evaluator, its evaluator generation, the
/// exact candidate generation it judges, the criterion (and criterion version)
/// it applies, and the coordinator epoch that makes it current.
struct EvaluationSubmission {
  EvaluationAuthority authority;
  CriterionRef criterion;
  CriterionKind kind{CriterionKind::ranking_signal};
  VerificationState verification{VerificationState::unknown};
  CategoricalJudgment judgment{CategoricalJudgment::unknown};
  bool score_defined{false};
  double score{0.0};
  double weight{1.0};
  std::vector<std::byte> evidence;
  std::string rationale;
};

/// Point-in-time inspection view of one evaluation.
struct EvaluationSnapshot {
  EvaluationId id;
  EvaluationGeneration generation;
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  EnsembleExecutionId execution;
  EnsembleAttemptGeneration attempt_generation;
  ParticipantId evaluator;
  ParticipantGeneration evaluator_generation;
  ParticipantRole evaluator_role{ParticipantRole::judge};
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  CriterionRef criterion;
  CriterionKind kind{CriterionKind::ranking_signal};
  VerificationState verification{VerificationState::unknown};
  CategoricalJudgment judgment{CategoricalJudgment::unknown};
  bool score_defined{false};
  double score{0.0};
  double weight{1.0};
  std::vector<std::byte> evidence;
  std::string rationale;
  Sequence received_sequence;
  CoordinatorEpoch coordinator_epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  bool current{true};
  bool superseded{false};
};

/// Per-candidate evaluation summary used by arbitration.  Hard predicates and
/// ranking signals are never merged into a single number.
struct CandidateEvaluationSummary {
  CandidateId candidate;
  CandidateGeneration generation;
  std::uint32_t evaluations{0};
  std::uint32_t accepts{0};
  std::uint32_t rejects{0};
  std::uint32_t abstentions{0};
  std::uint32_t unknowns{0};
  std::uint32_t distinct_evaluators{0};
  std::uint32_t hard_predicates{0};
  std::uint32_t hard_failures{0};
  std::uint32_t hard_unknowns{0};
  std::uint32_t verifier_pass{0};
  std::uint32_t verifier_fail{0};
  std::uint32_t verifier_abstain{0};
  std::uint32_t verifier_unknown{0};
  double weighted_score_sum{0.0};
  double weight_sum{0.0};
  double min_score{0.0};
  double max_score{0.0};

  [[nodiscard]] double mean_score() const noexcept {
    return weight_sum > 0.0 ? weighted_score_sum / weight_sum : 0.0;
  }
};

/// Deterministic fingerprint of an evaluation's scored content, used for
/// idempotent duplicate detection.  Two submissions with the same evaluation
/// identity but different content are a conflict, not a duplicate.
[[nodiscard]] std::uint64_t fingerprint_evaluation(const EvaluationSubmission& submission) noexcept;

}  // namespace ensemble_fabric
