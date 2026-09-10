#include <algorithm>
#include <utility>

#include "detail/text.hpp"
#include "fabric_internal.hpp"

namespace ensemble_fabric {

Outcome<EnsembleSnapshot> EnsembleFabric::inspect(EnsembleId id) const {
  std::shared_lock lock(impl_->mutex);
  const EnsembleRecord* ensemble = impl_->find(id);
  if (ensemble == nullptr) {
    return fail<EnsembleSnapshot>(EnsembleError::unknown_ensemble,
                                  "unknown ensemble " + id.to_string());
  }
  EnsembleSnapshot snapshot;
  snapshot.id = ensemble->spec.id;
  snapshot.generation = ensemble->spec.generation;
  snapshot.label = ensemble->spec.label;
  snapshot.task_class = ensemble->spec.task_class;
  snapshot.spec_fingerprint = ensemble->spec.fingerprint;
  snapshot.coordinator_epoch = impl_->epoch;
  snapshot.superseded = ensemble->superseded;
  snapshot.spec = ensemble->spec;

  for (const ParticipantRecord& participant : ensemble->participants) {
    ParticipantSnapshot view;
    view.id = participant.id;
    view.generation = participant.generation;
    view.label = participant.declaration.label;
    view.roles = participant.declaration.roles;
    view.required = participant.declaration.required;
    view.domain = participant.declaration.domain;
    view.priority = participant.declaration.priority;
    view.status = participant.status;
    view.worker = participant.worker;
    view.worker_boot = participant.worker_boot;
    view.coordinator_epoch = participant.coordinator_epoch;
    view.profile = participant.profile;
    view.replacement_count = participant.replacement_count;
    view.candidates_completed = participant.candidates_completed;
    view.candidates_failed = participant.candidates_failed;
    view.candidates_abstained = participant.candidates_abstained;
    view.evaluations_submitted = participant.evaluations_submitted;
    view.retries_consumed = participant.retries_consumed;
    view.fallback_activated = participant.fallback_activated;
    view.authoritative = participant.authoritative(impl_->epoch);
    snapshot.participants.push_back(std::move(view));
  }

  if (!ensemble->has_execution) {
    snapshot.execution_status = ExecutionStatus::none;
    return snapshot;
  }
  const ExecutionRecord& execution = ensemble->execution;
  snapshot.execution = execution.id;
  snapshot.attempt_generation = execution.attempt_generation;
  snapshot.execution_status = execution.status;
  snapshot.cancelled = execution.status == ExecutionStatus::cancelled;
  snapshot.revalidation_required = execution.status == ExecutionStatus::revalidation_required;
  snapshot.fallback_activated = execution.fallback_activated;
  snapshot.fallback_depth = execution.fallback_depth;
  snapshot.quorum = execution.quorum;
  snapshot.consensus = execution.consensus;
  snapshot.arbitration = execution.arbitration;
  snapshot.quorum.evaluations = static_cast<std::uint32_t>(execution.evaluations.size());
  snapshot.commits = execution.commits;
  snapshot.has_result = execution.has_result;
  if (execution.has_result) {
    snapshot.result = execution.result;
  }
  snapshot.explanation = execution.explanation;

  const std::uint32_t bound = impl_->config.limits.max_records_per_snapshot;
  std::uint32_t kept = 0;
  for (const CandidateRecord& candidate : execution.candidates) {
    if (kept >= bound) {
      break;
    }
    CandidateSnapshot view = snapshot_candidate(candidate);
    view.ensemble = ensemble->spec.id;
    view.ensemble_generation = ensemble->spec.generation;
    view.execution = execution.id;
    view.attempt_generation = execution.attempt_generation;
    snapshot.candidates.push_back(std::move(view));
    kept += 1u;
  }
  kept = 0;
  for (const EvaluationRecord& evaluation : execution.evaluations) {
    if (kept >= bound) {
      break;
    }
    EvaluationSnapshot view = snapshot_evaluation(evaluation);
    view.ensemble = ensemble->spec.id;
    view.ensemble_generation = ensemble->spec.generation;
    view.execution = execution.id;
    view.attempt_generation = execution.attempt_generation;
    snapshot.evaluations.push_back(std::move(view));
    kept += 1u;
  }
  return snapshot;
}

std::vector<EnsembleSummary> EnsembleFabric::list_ensembles() const {
  std::shared_lock lock(impl_->mutex);
  std::vector<EnsembleSummary> out;
  out.reserve(impl_->ensembles.size());
  for (const auto& entry : impl_->ensembles) {
    const EnsembleRecord& ensemble = entry.second;
    EnsembleSummary summary;
    summary.id = ensemble.spec.id;
    summary.generation = ensemble.spec.generation;
    summary.label = ensemble.spec.label;
    summary.coordinator_epoch = impl_->epoch;
    summary.participants = static_cast<std::uint32_t>(ensemble.participants.size());
    if (ensemble.has_execution) {
      summary.execution_status = ensemble.execution.status;
      summary.candidates = static_cast<std::uint32_t>(ensemble.execution.candidates.size());
      summary.evaluations = static_cast<std::uint32_t>(ensemble.execution.evaluations.size());
      summary.has_result = ensemble.execution.has_result;
    } else {
      summary.execution_status = ExecutionStatus::none;
    }
    out.push_back(std::move(summary));
  }
  return out;
}

std::string EnsembleSnapshot::render() const {
  std::string out = "ensemble " + id.to_string();
  out.append(" generation=").append(generation.to_string());
  out.append(" label='").append(label).append("'");
  out.append(" epoch=").append(coordinator_epoch.to_string());
  out.append(" spec_fingerprint=").append(std::to_string(spec_fingerprint));
  out.append("\nexecution=").append(execution.to_string());
  out.append(" attempt=").append(attempt_generation.to_string());
  out.append(" status=").append(to_string(execution_status));
  if (superseded) {
    out.append(" superseded=true");
  }
  if (cancelled) {
    out.append(" cancelled=true");
  }
  if (revalidation_required) {
    out.append(" revalidation_required=true");
  }
  if (fallback_activated) {
    out.append(" fallback_depth=").append(std::to_string(fallback_depth));
  }
  for (const ParticipantSnapshot& participant : participants) {
    out.append("\n  participant ").append(participant.id.to_string());
    out.append(" generation=").append(participant.generation.to_string());
    out.append(" roles=").append(participant.roles.to_string());
    out.append(participant.required ? " required" : " optional");
    out.append(" status=").append(to_string(participant.status));
    out.append(participant.authoritative ? " authoritative" : " not_authoritative");
    out.append(" worker=").append(participant.worker.to_string());
    out.append(" boot=").append(participant.worker_boot.to_string());
    if (participant.replacement_count != 0u) {
      out.append(" replacements=").append(std::to_string(participant.replacement_count));
    }
  }
  for (const CandidateSnapshot& candidate : candidates) {
    out.append("\n  candidate ").append(candidate.id.to_string());
    out.append(" generation=").append(candidate.generation.to_string());
    out.append(" participant=").append(candidate.participant.to_string());
    out.append(" role=").append(to_string(candidate.role));
    out.append(" state=").append(to_string(candidate.state));
    out.append(" attempt=").append(std::to_string(candidate.attempt));
    out.append(" bytes=").append(std::to_string(candidate.payload_bytes));
    if (candidate.failure != EnsembleError::ok) {
      out.append(" failure=").append(to_string(candidate.failure));
    }
    if (!candidate.elimination_reason.empty()) {
      out.append(" eliminated=").append(candidate.elimination_reason);
    }
  }
  for (const EvaluationSnapshot& evaluation : evaluations) {
    out.append("\n  evaluation ").append(evaluation.id.to_string());
    out.append(" evaluator=").append(evaluation.evaluator.to_string());
    out.append(" eg=").append(evaluation.evaluator_generation.to_string());
    out.append(" candidate=").append(evaluation.candidate.to_string());
    out.append(" cg=").append(evaluation.candidate_generation.to_string());
    out.append(" criterion=").append(std::to_string(evaluation.criterion.id))
        .append(".")
        .append(std::to_string(evaluation.criterion.version));
    out.append(" kind=").append(to_string(evaluation.kind));
    out.append(" judgment=").append(to_string(evaluation.judgment));
    out.append(" verification=").append(to_string(evaluation.verification));
    if (evaluation.score_defined) {
      out.append(" score=").append(detail::format_double(evaluation.score));
    }
    out.append(evaluation.current ? " current" : " superseded");
  }
  out.push_back('\n');
  out.append("  ").append(quorum.render()).push_back('\n');
  out.append("  ").append(consensus.render()).push_back('\n');
  out.append("  ").append(arbitration.render());
  for (const CommitRecord& record : commits) {
    out.append("\n  commit result=").append(record.result.to_string());
    out.append(" sequence=").append(record.sequence.to_string());
    out.append(" epoch=").append(record.coordinator_epoch.to_string());
    out.append(" decision=").append(to_string(record.decision));
    out.append(" fingerprint=").append(std::to_string(record.commit_fingerprint));
  }
  if (has_result) {
    out.push_back('\n');
    out.append("  ").append(result.render());
  }
  return out;
}

}  // namespace ensemble_fabric
