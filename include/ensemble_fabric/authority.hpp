#pragma once

#include <cstdint>
#include <string>

#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/outcome.hpp"

namespace ensemble_fabric {

/// Authority of a logical participant inside one ensemble generation.
///
/// A participant is authoritative only when *every* component below matches the
/// current state of the coordinator.  Availability of a worker process is not
/// authority: a worker may be reachable while its boot identity, participant
/// generation, or coordinator epoch is stale.
struct ParticipantAuthority {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  ParticipantId participant;
  ParticipantGeneration participant_generation;
  WorkerId worker;
  WorkerBootId worker_boot;
  CoordinatorEpoch coordinator_epoch;

  [[nodiscard]] bool complete() const noexcept {
    return ensemble.valid() && ensemble_generation.valid() && participant.valid() &&
           participant_generation.valid() && worker.valid() && worker_boot.valid() &&
           coordinator_epoch.valid();
  }

  [[nodiscard]] std::string describe() const;
};

/// Authority of one logical candidate slot inside an execution attempt.
struct CandidateAuthority {
  ParticipantAuthority participant;
  EnsembleExecutionId execution;
  EnsembleAttemptGeneration attempt_generation;
  CandidateId candidate;
  CandidateGeneration candidate_generation;

  [[nodiscard]] bool complete() const noexcept {
    return participant.complete() && execution.valid() && attempt_generation.valid() &&
           candidate.valid() && candidate_generation.valid();
  }

  [[nodiscard]] std::string describe() const;
};

/// Authority of one evaluation submission.
struct EvaluationAuthority {
  ParticipantAuthority evaluator;
  EnsembleExecutionId execution;
  EnsembleAttemptGeneration attempt_generation;
  EvaluationId evaluation;
  EvaluationGeneration evaluation_generation;
  CandidateId candidate;
  CandidateGeneration candidate_generation;

  [[nodiscard]] bool complete() const noexcept {
    return evaluator.complete() && execution.valid() && attempt_generation.valid() &&
           evaluation.valid() && evaluation_generation.valid() && candidate.valid() &&
           candidate_generation.valid();
  }

  [[nodiscard]] std::string describe() const;
};

/// Identity of one execution attempt inside one ensemble generation.
struct ExecutionRef {
  EnsembleId ensemble;
  EnsembleGeneration ensemble_generation;
  EnsembleExecutionId execution;
  EnsembleAttemptGeneration attempt_generation;

  [[nodiscard]] bool complete() const noexcept {
    return ensemble.valid() && ensemble_generation.valid() && execution.valid() &&
           attempt_generation.valid();
  }
};

/// The epoch of the coordinator that currently owns the ensemble's authority.
/// Traffic produced under an older epoch can never mutate current state, and a
/// restart never resurrects live process authority.
struct CoordinatorIdentity {
  CoordinatorEpoch epoch;
  WorkerBootId boot;
  std::uint64_t started_at_unix_nanos{0};

  [[nodiscard]] bool valid() const noexcept { return epoch.valid() && boot.valid(); }
};

/// Structural validation shared by the wire decoder and the in-process API so
/// that a partially populated identity is rejected identically on both paths.
[[nodiscard]] Status validate_authority(const ParticipantAuthority& authority);
[[nodiscard]] Status validate_authority(const CandidateAuthority& authority);
[[nodiscard]] Status validate_authority(const EvaluationAuthority& authority);

/// Compares only the "who is allowed to act" components, ignoring the target.
/// Used to fence stale epochs, boots, and generations uniformly.
[[nodiscard]] bool same_actor(const ParticipantAuthority& lhs,
                              const ParticipantAuthority& rhs) noexcept;

}  // namespace ensemble_fabric
