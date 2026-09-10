#include "ensemble_fabric/spec.hpp"

#include <algorithm>
#include <unordered_set>

#include "ensemble_fabric/arbitration.hpp"

#include "detail/text.hpp"

namespace ensemble_fabric {

std::string_view to_string(TopologyMode mode) noexcept {
  switch (mode) {
    case TopologyMode::parallel: return "PARALLEL";
    case TopologyMode::sequential: return "SEQUENTIAL";
  }
  return "UNKNOWN_TOPOLOGY";
}

std::string_view to_string(VoteBasis basis) noexcept {
  switch (basis) {
    case VoteBasis::declared_participants: return "DECLARED_PARTICIPANTS";
    case VoteBasis::authoritative_participants: return "AUTHORITATIVE_PARTICIPANTS";
    case VoteBasis::contributing_participants: return "CONTRIBUTING_PARTICIPANTS";
    case VoteBasis::required_participants: return "REQUIRED_PARTICIPANTS";
    case VoteBasis::eligible_participants: return "ELIGIBLE_PARTICIPANTS";
  }
  return "UNKNOWN_BASIS";
}

std::string_view to_string(AggregationKind kind) noexcept {
  switch (kind) {
    case AggregationKind::best_of_n: return "BEST_OF_N";
    case AggregationKind::quorum_vote: return "QUORUM_VOTE";
    case AggregationKind::weighted_vote: return "WEIGHTED_VOTE";
    case AggregationKind::consensus_threshold: return "CONSENSUS_THRESHOLD";
    case AggregationKind::judge_arbitration: return "JUDGE_ARBITRATION";
    case AggregationKind::verifier_gated: return "VERIFIER_GATED";
  }
  return "UNKNOWN_AGGREGATION";
}

std::string_view to_string(ArbitrationFactor factor) noexcept {
  switch (factor) {
    case ArbitrationFactor::judge_acceptance_ratio: return "judge_acceptance_ratio";
    case ArbitrationFactor::verifier_state: return "verifier_state";
    case ArbitrationFactor::vote_count: return "vote_count";
    case ArbitrationFactor::weighted_vote: return "weighted_vote";
    case ArbitrationFactor::aggregate_score: return "aggregate_score";
    case ArbitrationFactor::min_score: return "min_score";
    case ArbitrationFactor::confidence: return "confidence";
    case ArbitrationFactor::participant_priority: return "participant_priority";
    case ArbitrationFactor::specialist_relevance: return "specialist_relevance";
    case ArbitrationFactor::execution_cost: return "execution_cost";
    case ArbitrationFactor::latency: return "latency";
    case ArbitrationFactor::provenance_quality: return "provenance_quality";
    case ArbitrationFactor::fallback_status: return "fallback_status";
  }
  return "unknown_factor";
}

std::string_view to_string(TieBreakPolicy policy) noexcept {
  switch (policy) {
    case TieBreakPolicy::report_tie: return "REPORT_TIE";
    case TieBreakPolicy::lowest_candidate_id: return "LOWEST_CANDIDATE_ID";
    case TieBreakPolicy::highest_score_then_id: return "HIGHEST_SCORE_THEN_ID";
    case TieBreakPolicy::prefer_required_participant: return "PREFER_REQUIRED_PARTICIPANT";
  }
  return "UNKNOWN_TIE_BREAK";
}

std::string_view to_string(FallbackTrigger trigger) noexcept {
  switch (trigger) {
    case FallbackTrigger::participant_unavailable: return "PARTICIPANT_UNAVAILABLE";
    case FallbackTrigger::retry_exhausted: return "RETRY_EXHAUSTED";
    case FallbackTrigger::quorum_impossible: return "QUORUM_IMPOSSIBLE";
    case FallbackTrigger::required_role_unavailable: return "REQUIRED_ROLE_UNAVAILABLE";
    case FallbackTrigger::all_primary_invalid: return "ALL_PRIMARY_INVALID";
    case FallbackTrigger::judge_disagreement: return "JUDGE_DISAGREEMENT";
    case FallbackTrigger::insufficient_evidence: return "INSUFFICIENT_EVIDENCE";
    case FallbackTrigger::required_participant_failed: return "REQUIRED_PARTICIPANT_FAILED";
  }
  return "UNKNOWN_TRIGGER";
}

const ParticipantSpec* find_participant(const EnsembleSpec& spec, ParticipantId id) noexcept {
  for (const ParticipantSpec& participant : spec.participants) {
    if (participant.id == id) {
      return &participant;
    }
  }
  return nullptr;
}

RoleSet declared_roles(const EnsembleSpec& spec) noexcept {
  RoleSet roles = RoleSet::none();
  for (const ParticipantSpec& participant : spec.participants) {
    roles = roles.merge(participant.roles);
  }
  return roles;
}

namespace {

[[nodiscard]] Status check_bounded(std::string_view text, std::size_t max_bytes,
                                   std::string_view what) {
  if (!detail::is_bounded_ascii(text, max_bytes)) {
    return fail(EnsembleError::invalid_argument,
                std::string("spec: ") + std::string(what) +
                    " must be printable ASCII within the configured metadata bound");
  }
  return ok_status();
}

}  // namespace

Status validate_spec(const EnsembleSpec& spec, const Limits& limits) {
  const Status limits_status = limits.validate();
  if (!limits_status.ok()) {
    return limits_status;
  }
  if (!spec.id.valid()) {
    return fail(EnsembleError::invalid_argument, "spec: ensemble identity is absent");
  }
  const Status label_status =
      check_bounded(spec.label, limits.max_label_bytes, "ensemble label");
  if (!label_status.ok()) {
    return label_status;
  }
  const Status task_status =
      check_bounded(spec.task_class, limits.max_metadata_bytes, "task class");
  if (!task_status.ok()) {
    return task_status;
  }
  if (spec.participants.empty()) {
    return fail(EnsembleError::invalid_argument, "spec: an ensemble needs at least one participant");
  }
  if (spec.participants.size() > limits.max_participants_per_ensemble) {
    return fail(EnsembleError::resource_limit_exceeded,
                "spec: participant count exceeds the configured bound");
  }
  if (spec.stages.size() > limits.max_stages_per_ensemble) {
    return fail(EnsembleError::resource_limit_exceeded,
                "spec: stage count exceeds the configured bound");
  }
  if (spec.quorum.role_quota.size() > limits.max_role_quota_entries) {
    return fail(EnsembleError::resource_limit_exceeded,
                "spec: role quota entry count exceeds the configured bound");
  }
  if (spec.quorum.consensus_threshold_percent > 100u) {
    return fail(EnsembleError::invalid_argument,
                "spec: consensus threshold percentage must be between 0 and 100");
  }
  if (spec.arbitration.max_selected != 1u) {
    return fail(EnsembleError::invalid_argument,
                "spec: exactly one authoritative result is selected; max_selected must be 1");
  }

  std::unordered_set<std::uint64_t> participant_ids;
  RoleSet all_roles = RoleSet::none();
  bool producing_role_present = false;
  for (const ParticipantSpec& participant : spec.participants) {
    if (!participant.id.valid()) {
      return fail(EnsembleError::invalid_argument, "spec: participant identity is absent");
    }
    if (!participant_ids.insert(participant.id.value()).second) {
      return fail(EnsembleError::participant_duplicate,
                  "spec: participant identity " + participant.id.to_string() + " is declared twice");
    }
    if (participant.roles.empty()) {
      return fail(EnsembleError::invalid_argument,
                  "spec: participant " + participant.id.to_string() + " has no role");
    }
    const Status participant_label =
        check_bounded(participant.label, limits.max_label_bytes, "participant label");
    if (!participant_label.ok()) {
      return participant_label;
    }
    const Status domain_status =
        check_bounded(participant.domain, limits.max_metadata_bytes, "participant domain");
    if (!domain_status.ok()) {
      return domain_status;
    }
    if (participant.compatibility.min_protocol_version == 0u) {
      return fail(EnsembleError::invalid_argument,
                  "spec: participant " + participant.id.to_string() +
                      " requires a zero protocol version");
    }
    all_roles = all_roles.merge(participant.roles);
    for (const ParticipantRole role :
         {ParticipantRole::candidate, ParticipantRole::specialist, ParticipantRole::fallback}) {
      if (participant.roles.contains(role)) {
        producing_role_present = true;
      }
    }
  }
  if (!producing_role_present) {
    return fail(EnsembleError::invalid_argument,
                "spec: an ensemble needs at least one candidate-producing participant");
  }

  std::unordered_set<std::uint64_t> stage_ids;
  for (const StageSpec& stage : spec.stages) {
    if (!stage.id.valid()) {
      return fail(EnsembleError::invalid_argument, "spec: stage identity is absent");
    }
    if (!stage_ids.insert(stage.id.value()).second) {
      return fail(EnsembleError::invalid_argument,
                  "spec: stage identity " + stage.id.to_string() + " is declared twice");
    }
    if (stage.participants.empty()) {
      return fail(EnsembleError::invalid_argument,
                  "spec: stage " + stage.id.to_string() + " has no participants");
    }
    if (stage.participants.size() > limits.max_parallel_branches) {
      return fail(EnsembleError::resource_limit_exceeded,
                  "spec: a stage exceeds the configured parallel-branch bound");
    }
    for (const ParticipantId participant_id : stage.participants) {
      const ParticipantSpec* participant = find_participant(spec, participant_id);
      if (participant == nullptr) {
        return fail(EnsembleError::invalid_argument,
                    "spec: stage " + stage.id.to_string() + " references undeclared participant " +
                        participant_id.to_string());
      }
      bool produces = false;
      for (const ParticipantRole role :
           {ParticipantRole::candidate, ParticipantRole::specialist, ParticipantRole::fallback}) {
        if (participant->roles.contains(role)) {
          produces = true;
        }
      }
      if (!produces) {
        return fail(EnsembleError::invalid_argument,
                    "spec: stage " + stage.id.to_string() + " references participant " +
                        participant_id.to_string() +
                        " which holds no candidate-producing role");
      }
    }
    for (const std::string& domain : stage.required_domains) {
      const Status domain_status =
          check_bounded(domain, limits.max_metadata_bytes, "required domain");
      if (!domain_status.ok()) {
        return domain_status;
      }
      bool satisfied = false;
      for (const ParticipantId participant_id : stage.participants) {
        const ParticipantSpec* participant = find_participant(spec, participant_id);
        if (participant != nullptr && participant->domain == domain) {
          satisfied = true;
        }
      }
      if (!satisfied) {
        return fail(EnsembleError::invalid_argument,
                    "spec: stage " + stage.id.to_string() + " requires domain '" + domain +
                        "' but no listed participant declares it");
      }
    }
  }

  for (std::size_t index = 0; index < spec.quorum.role_quota.size(); ++index) {
    const RoleQuota& quota = spec.quorum.role_quota[index];
    if (quota.min_count == 0u) {
      return fail(EnsembleError::invalid_argument, "spec: a role quota requires a positive count");
    }
    if (!all_roles.contains(quota.role)) {
      return fail(EnsembleError::invalid_argument,
                  "spec: role quota names role " +
                      std::string(ensemble_fabric::to_string(quota.role)) +
                      " which no participant holds");
    }
    for (std::size_t other = index + 1; other < spec.quorum.role_quota.size(); ++other) {
      if (spec.quorum.role_quota[other].role == quota.role) {
        return fail(EnsembleError::invalid_argument, "spec: duplicate role quota entry");
      }
    }
  }

  if (spec.aggregation.require_verification_pass && !all_roles.contains(ParticipantRole::verifier)) {
    return fail(EnsembleError::invalid_argument,
                "spec: verifier-gated aggregation requires a verifier participant");
  }
  if (spec.aggregation.require_judge_acceptance && !all_roles.contains(ParticipantRole::judge)) {
    return fail(EnsembleError::invalid_argument,
                "spec: judge-gated aggregation requires a judge participant");
  }

  std::unordered_set<std::uint32_t> factor_seen;
  for (const ArbitrationFactor factor : spec.arbitration.factors) {
    const std::uint32_t key = static_cast<std::uint32_t>(factor);
    if (key < 1u || key > 13u) {
      return fail(EnsembleError::invalid_enum_value, "spec: arbitration factor is out of range");
    }
    if (!factor_seen.insert(key).second) {
      return fail(EnsembleError::invalid_argument, "spec: duplicate arbitration factor");
    }
  }

  std::unordered_set<std::uint32_t> trigger_seen;
  for (const FallbackTrigger trigger : spec.fallback.triggers) {
    const std::uint32_t key = static_cast<std::uint32_t>(trigger);
    if (key < 1u || key > 8u) {
      return fail(EnsembleError::invalid_enum_value, "spec: fallback trigger is out of range");
    }
    if (!trigger_seen.insert(key).second) {
      return fail(EnsembleError::invalid_argument, "spec: duplicate fallback trigger");
    }
  }
  if (spec.fallback.enabled) {
    if (spec.fallback.triggers.empty()) {
      return fail(EnsembleError::invalid_argument,
                  "spec: fallback is enabled without an explicit activation condition");
    }
    if (spec.fallback.fallback_participants.empty()) {
      return fail(EnsembleError::invalid_argument,
                  "spec: fallback is enabled without fallback participants");
    }
    for (const ParticipantId fallback_id : spec.fallback.fallback_participants) {
      const ParticipantSpec* participant = find_participant(spec, fallback_id);
      if (participant == nullptr) {
        return fail(EnsembleError::invalid_argument,
                    "spec: fallback references undeclared participant " + fallback_id.to_string());
      }
      if (!participant->roles.contains(ParticipantRole::fallback)) {
        return fail(EnsembleError::invalid_argument,
                    "spec: fallback participant " + fallback_id.to_string() +
                        " does not hold the FALLBACK role");
      }
    }
    if (spec.fallback.max_depth == 0u || spec.fallback.max_depth > limits.max_fallback_depth) {
      return fail(EnsembleError::invalid_argument,
                  "spec: fallback depth is outside the configured bound");
    }
  } else if (!spec.fallback.fallback_participants.empty()) {
    return fail(EnsembleError::invalid_argument,
                "spec: fallback participants declared while fallback is disabled");
  }

  if (spec.retry.max_attempts == 0u) {
    return fail(EnsembleError::invalid_argument, "spec: retry policy must allow at least one attempt");
  }
  if (spec.retry.max_attempts > limits.max_retries + 1u) {
    return fail(EnsembleError::resource_limit_exceeded,
                "spec: retry attempts exceed the configured bound");
  }

  std::unordered_set<std::uint64_t> criterion_seen;
  for (const CriterionRef& criterion : spec.evidence.mandatory_criteria) {
    if (!criterion.valid()) {
      return fail(EnsembleError::invalid_argument, "spec: a mandatory criterion has a zero identity");
    }
    const std::uint64_t key =
        (static_cast<std::uint64_t>(criterion.id) << 32u) | criterion.version;
    if (!criterion_seen.insert(key).second) {
      return fail(EnsembleError::invalid_argument, "spec: duplicate mandatory criterion");
    }
  }
  std::unordered_set<std::uint64_t> judging_seen;
  for (const CriterionRef& criterion : spec.judging_criteria) {
    if (!criterion.valid()) {
      return fail(EnsembleError::invalid_argument, "spec: a judging criterion has a zero identity");
    }
    const std::uint64_t key =
        (static_cast<std::uint64_t>(criterion.id) << 32u) | criterion.version;
    if (!judging_seen.insert(key).second) {
      return fail(EnsembleError::invalid_argument, "spec: duplicate judging criterion");
    }
  }

  if (spec.quorum.min_valid_candidates > limits.max_candidates_per_execution) {
    return fail(EnsembleError::resource_limit_exceeded,
                "spec: minimum valid candidates exceeds the configured candidate bound");
  }
  {
    std::uint64_t total = 0;
    for (const StageSpec& stage : spec.stages) {
      std::uint64_t next = 0;
      if (!checked_add(total, static_cast<std::uint64_t>(stage.participants.size()),
                       limits.max_candidates_per_execution, next)) {
        return fail(EnsembleError::resource_limit_exceeded,
                    "spec: declared candidates exceed the configured candidate bound");
      }
      total = next;
    }
    if (spec.stages.empty()) {
      // A specification without explicit stages still dispatches every
      // candidate-producing participant in one implicit parallel stage.
      std::uint64_t declared = 0;
      for (const ParticipantSpec& participant : spec.participants) {
        for (const ParticipantRole role :
             {ParticipantRole::candidate, ParticipantRole::specialist}) {
          if (participant.roles.contains(role)) {
            declared += 1u;
            break;
          }
        }
      }
      if (declared > limits.max_candidates_per_execution) {
        return fail(EnsembleError::resource_limit_exceeded,
                    "spec: implied candidate count exceeds the configured bound");
      }
    }
  }

  if (spec.completion.require_all_required_participants && spec.quorum.max_failures != 0u &&
      spec.quorum.max_failures != unbounded) {
    // Requiring every required participant while also budgeting failures is
    // contradictory; reject it instead of resolving it silently at arbitration.
    if (spec.quorum.max_failures > 0u) {
      return fail(EnsembleError::invalid_argument,
                  "spec: completion requires all required participants while a failure budget "
                  "allows failures");
    }
  }
  return ok_status();
}

std::uint64_t compute_spec_fingerprint(const EnsembleSpec& spec) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  auto add = [&hash](std::uint64_t value) { hash = detail::fnv1a64_extend(hash, value); };
  auto add_text = [&hash](std::string_view text) { hash = detail::fnv1a64_extend(hash, text); };

  add_text(spec.label);
  add_text(spec.task_class);
  add(spec.task_class_id);
  add(spec.participants.size());
  for (const ParticipantSpec& participant : spec.participants) {
    add(participant.id.value());
    add_text(participant.label);
    add(participant.roles.mask());
    add(participant.required ? 1ull : 0ull);
    add_text(participant.domain);
    add(participant.priority);
    add(participant.execution_budget_units);
    add(participant.compatibility.min_protocol_version);
    add(participant.compatibility.required_capabilities);
    add(participant.compatibility.required_task_class);
    add(participant.compatibility.required_output_schema);
    add(participant.compatibility.excluded_model_family);
    add(participant.compatibility.required_backend_features);
  }
  add(spec.stages.size());
  for (const StageSpec& stage : spec.stages) {
    add(stage.id.value());
    add_text(stage.label);
    add(static_cast<std::uint64_t>(stage.mode));
    add(stage.participants.size());
    for (const ParticipantId id : stage.participants) {
      add(id.value());
    }
    for (const std::string& domain : stage.required_domains) {
      add_text(domain);
    }
  }
  add(spec.quorum.min_participants);
  add(spec.quorum.role_quota.size());
  for (const RoleQuota& quota : spec.quorum.role_quota) {
    add(static_cast<std::uint64_t>(quota.role));
    add(quota.min_count);
  }
  add(spec.quorum.min_evaluations);
  add(spec.quorum.min_valid_candidates);
  add(spec.quorum.consensus_threshold_percent);
  add(static_cast<std::uint64_t>(spec.quorum.vote_basis));
  add(spec.quorum.count_abstentions ? 1ull : 0ull);
  add(spec.quorum.count_optional_participants ? 1ull : 0ull);
  add(spec.quorum.count_failed_in_denominator ? 1ull : 0ull);
  add(spec.quorum.count_unavailable_in_denominator ? 1ull : 0ull);
  add(spec.quorum.max_failures);
  add(spec.quorum.max_abstentions);
  add(static_cast<std::uint64_t>(spec.aggregation.kind));
  add(spec.aggregation.use_weights ? 1ull : 0ull);
  add(spec.aggregation.require_verification_pass ? 1ull : 0ull);
  add(spec.aggregation.require_judge_acceptance ? 1ull : 0ull);
  hash = detail::fnv1a64_extend(hash, spec.aggregation.min_aggregate_score);
  hash = detail::fnv1a64_extend(hash, spec.aggregation.min_judge_acceptance_ratio);
  add(spec.arbitration.factors.size());
  for (const ArbitrationFactor factor : spec.arbitration.factors) {
    add(static_cast<std::uint64_t>(factor));
  }
  add(static_cast<std::uint64_t>(spec.arbitration.tie_break));
  add(spec.arbitration.require_unique_winner ? 1ull : 0ull);
  add(spec.arbitration.max_selected);
  add(spec.fallback.enabled ? 1ull : 0ull);
  add(spec.fallback.triggers.size());
  for (const FallbackTrigger trigger : spec.fallback.triggers) {
    add(static_cast<std::uint64_t>(trigger));
  }
  add(spec.fallback.fallback_participants.size());
  for (const ParticipantId id : spec.fallback.fallback_participants) {
    add(id.value());
  }
  add(spec.fallback.eager_speculative ? 1ull : 0ull);
  add(spec.fallback.max_depth);
  add(spec.completion.require_consensus ? 1ull : 0ull);
  add(spec.completion.require_all_required_participants ? 1ull : 0ull);
  add(spec.completion.allow_partial_results ? 1ull : 0ull);
  add(spec.retry.max_attempts);
  add(spec.retry.retry_on_unavailable ? 1ull : 0ull);
  add(spec.retry.retry_on_transport_error ? 1ull : 0ull);
  add(spec.retry.retry_on_malformed_candidate ? 1ull : 0ull);
  add(spec.retry.retry_on_participant_failed ? 1ull : 0ull);
  add(spec.retry.replace_participant ? 1ull : 0ull);
  add(spec.retry.rerun_evaluations ? 1ull : 0ull);
  add(spec.cancellation.cancel_on_required_failure ? 1ull : 0ull);
  add(spec.cancellation.cancel_on_quorum_impossible ? 1ull : 0ull);
  add(spec.cancellation.preserve_committed_results ? 1ull : 0ull);
  add(spec.evidence.min_candidate_payload_bytes);
  add(spec.evidence.min_evaluations_per_candidate);
  add(spec.evidence.require_rationale ? 1ull : 0ull);
  add(spec.evidence.mandatory_criteria.size());
  for (const CriterionRef& criterion : spec.evidence.mandatory_criteria) {
    add(criterion.id);
    add(criterion.version);
  }
  add(spec.evidence.unknown_verification_is_failure ? 1ull : 0ull);
  add(spec.evidence.abstention_is_failure ? 1ull : 0ull);
  add(spec.judging_criteria.size());
  for (const CriterionRef& criterion : spec.judging_criteria) {
    add(criterion.id);
    add(criterion.version);
  }
  add(spec.requirements.min_protocol_version);
  add(spec.requirements.required_capabilities);
  add(spec.requirements.required_task_class);
  add(spec.requirements.required_output_schema);
  add(spec.requirements.excluded_model_family);
  add(spec.requirements.required_backend_features);
  add(spec.requirements.require_model_family_diversity ? 1ull : 0ull);
  add(spec.overall_budget_units);
  add(spec.deterministic_seed);
  return hash;
}

std::string render_spec(const EnsembleSpec& spec) {
  std::string out;
  out.append("ensemble ").append(spec.id.to_string());
  out.append(" generation ").append(spec.generation.to_string());
  out.append(" label='").append(spec.label).append("'");
  out.append(" task='").append(spec.task_class).append("'");
  out.append(" fingerprint=").append(std::to_string(spec.fingerprint));
  std::vector<std::string> parts;
  for (const ParticipantSpec& participant : spec.participants) {
    std::string line = "  participant " + participant.id.to_string() + " roles=" +
                       participant.roles.to_string() +
                       (participant.required ? " required" : " optional");
    if (!participant.domain.empty()) {
      line.append(" domain=").append(participant.domain);
    }
    line.append(" priority=").append(std::to_string(participant.priority));
    parts.push_back(std::move(line));
  }
  for (const StageSpec& stage : spec.stages) {
    std::string line = "  stage " + stage.id.to_string() + " mode=" +
                       std::string(ensemble_fabric::to_string(stage.mode)) + " participants=";
    for (std::size_t index = 0; index < stage.participants.size(); ++index) {
      if (index != 0u) {
        line.push_back(',');
      }
      line.append(stage.participants[index].to_string());
    }
    parts.push_back(std::move(line));
  }
  parts.push_back("  quorum basis=" +
                  std::string(ensemble_fabric::to_string(spec.quorum.vote_basis)) +
                  " min_participants=" + std::to_string(spec.quorum.min_participants) +
                  " min_evaluations=" + std::to_string(spec.quorum.min_evaluations) +
                  " threshold=" + std::to_string(spec.quorum.consensus_threshold_percent) + "%");
  parts.push_back("  aggregation=" +
                  std::string(ensemble_fabric::to_string(spec.aggregation.kind)));
  parts.push_back("  arbitration factors=" + render_factors(spec.arbitration.factors) +
                  " tie_break=" + std::string(ensemble_fabric::to_string(spec.arbitration.tie_break)));
  parts.push_back(std::string("  fallback=") + (spec.fallback.enabled ? "enabled" : "disabled"));
  parts.push_back("  retry max_attempts=" + std::to_string(spec.retry.max_attempts));
  for (const std::string& part : parts) {
    out.push_back('\n');
    out.append(part);
  }
  return out;
}

}  // namespace ensemble_fabric
