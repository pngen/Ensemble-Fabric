#include "ensemble_fabric/local_driver.hpp"

#include <utility>

#include "ensemble_fabric/version.hpp"

namespace ensemble_fabric {

bool LocalEnsembleDriver::perform_dispatch(const DispatchAction& dispatch) {
  LocalParticipant* participant = find(dispatch.participant);
  if (participant == nullptr || !participant->authoritative() || participant->backend == nullptr) {
    return false;
  }
  DispatchCandidateMessage request;
  request.ensemble = dispatch.execution.ensemble;
  request.ensemble_generation = dispatch.execution.ensemble_generation;
  request.execution = dispatch.execution.execution;
  request.attempt_generation = dispatch.execution.attempt_generation;
  request.participant = dispatch.participant;
  request.participant_generation = dispatch.participant_generation;
  request.coordinator_epoch = fabric_.epoch();
  request.candidate = dispatch.candidate;
  request.candidate_generation = dispatch.candidate_generation;
  request.role = dispatch.role;
  request.stage = dispatch.stage;
  request.attempt = dispatch.attempt;
  request.budget_units = dispatch.budget_units;
  request.deterministic_seed = dispatch.deterministic_seed;
  request.domain = dispatch.domain;
  request.request = dispatch.request;

  CandidateAuthority authority;
  authority.participant = authority_of(*participant);
  authority.execution = dispatch.execution.execution;
  authority.attempt_generation = dispatch.execution.attempt_generation;
  authority.candidate = dispatch.candidate;
  authority.candidate_generation = dispatch.candidate_generation;

  Outcome<ParticipantProduction> production = participant->backend->produce(request);
  if (!production.ok()) {
    CandidateFailureSubmission submission;
    submission.authority = authority;
    submission.participant_failure =
        static_cast<std::uint32_t>(ParticipantFailureKind::internal_error);
    return fabric_.submit_candidate_failure(submission).ok();
  }
  switch (production.value().kind) {
    case ParticipantProduction::Kind::content: {
      CandidateSubmission submission;
      submission.authority = authority;
      submission.payload = production.value().payload;
      return fabric_.submit_candidate(submission).ok();
    }
    case ParticipantProduction::Kind::abstention: {
      CandidateAbstentionSubmission submission;
      submission.authority = authority;
      submission.rationale = production.value().detail;
      return fabric_.submit_candidate_abstention(submission).ok();
    }
    case ParticipantProduction::Kind::failure: {
      CandidateFailureSubmission submission;
      submission.authority = authority;
      submission.participant_failure = static_cast<std::uint32_t>(production.value().failure);
      return fabric_.submit_candidate_failure(submission).ok();
    }
  }
  return false;
}

bool LocalEnsembleDriver::perform_evaluation(const EvaluationAction& action) {
  LocalParticipant* participant = find(action.judge);
  if (participant == nullptr || !participant->authoritative() || participant->backend == nullptr) {
    return false;
  }
  Outcome<EnsembleSnapshot> snapshot = fabric_.inspect(ensemble_);
  if (!snapshot.ok()) {
    return false;
  }
  std::vector<std::byte> payload;
  for (const CandidateSnapshot& candidate : snapshot.value().candidates) {
    if (candidate.id == action.candidate) {
      payload = candidate.payload;
    }
  }
  EvaluationRequestMessage request;
  request.ensemble = action.execution.ensemble;
  request.ensemble_generation = action.execution.ensemble_generation;
  request.execution = action.execution.execution;
  request.attempt_generation = action.execution.attempt_generation;
  request.judge = action.judge;
  request.judge_generation = action.judge_generation;
  request.coordinator_epoch = fabric_.epoch();
  request.evaluation = action.evaluation;
  request.evaluation_generation = action.evaluation_generation;
  request.role = action.role;
  request.candidate = action.candidate;
  request.candidate_generation = action.candidate_generation;
  request.criterion = action.criterion;
  request.criterion_kind = action.criterion_kind;
  request.candidate_payload = payload;
  request.domain = action.domain;

  Outcome<ParticipantEvaluation> evaluation = participant->backend->evaluate(request);
  if (!evaluation.ok()) {
    return false;
  }
  EvaluationSubmission submission;
  submission.authority.evaluator = authority_of(*participant);
  submission.authority.execution = action.execution.execution;
  submission.authority.attempt_generation = action.execution.attempt_generation;
  submission.authority.evaluation = action.evaluation;
  submission.authority.evaluation_generation = action.evaluation_generation;
  submission.authority.candidate = action.candidate;
  submission.authority.candidate_generation = action.candidate_generation;
  submission.criterion = action.criterion;
  submission.kind = action.criterion_kind;
  submission.verification = evaluation.value().verification;
  submission.judgment = evaluation.value().judgment;
  submission.score_defined = evaluation.value().score_defined;
  submission.score = evaluation.value().score;
  submission.weight = evaluation.value().weight;
  submission.evidence = evaluation.value().evidence;
  submission.rationale = evaluation.value().rationale;
  return fabric_.submit_evaluation(submission).ok();
}

Outcome<std::uint32_t> LocalEnsembleDriver::step() {
  Outcome<std::vector<FabricAction>> actions = fabric_.advance(ensemble_, ensemble_generation_);
  if (!actions.ok()) {
    return fail<std::uint32_t>(actions.code(), actions.error().message);
  }
  std::uint32_t performed = 0;
  for (const FabricAction& action : actions.value()) {
    if (const auto* dispatch = std::get_if<DispatchAction>(&action)) {
      if (perform_dispatch(*dispatch)) {
        ++performed;
      }
    } else if (const auto* evaluation = std::get_if<EvaluationAction>(&action)) {
      if (perform_evaluation(*evaluation)) {
        ++performed;
      }
    } else {
      ++performed;
    }
  }
  return performed;
}

Status LocalEnsembleDriver::run(std::uint32_t max_rounds) {
  for (std::uint32_t round = 0; round < max_rounds; ++round) {
    Outcome<std::uint32_t> performed = step();
    if (!performed.ok()) {
      return fail(performed.code(), performed.error().message);
    }
    if (performed.value() == 0u) {
      return ok_status();
    }
  }
  return fail(EnsembleError::resource_limit_exceeded,
              "the local driver exceeded its round budget");
}

Status LocalEnsembleDriver::kill(LocalParticipant& participant, std::string detail) {
  if (!participant.registered) {
    return fail(EnsembleError::participant_not_authoritative,
                "participant is not registered and cannot be killed");
  }
  const ParticipantAuthority authority = authority_of(participant);
  participant.alive = false;
  participant.registered = false;
  return fabric_.report_participant_failure(authority, ParticipantFailureKind::unavailable,
                                            std::move(detail));
}

}  // namespace ensemble_fabric
