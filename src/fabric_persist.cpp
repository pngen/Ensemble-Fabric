#include <algorithm>
#include <utility>

#include "ensemble_fabric/client.hpp"
#include "ensemble_fabric/version.hpp"
#include "ensemble_fabric/worker.hpp"

#include "detail/text.hpp"
#include "fabric_internal.hpp"

namespace ensemble_fabric {
namespace {

template <typename Enum>
void put_enum(Encoder& out, Enum value) {
  out.u8(static_cast<std::uint8_t>(value));
}

template <typename Enum>
[[nodiscard]] Outcome<Enum> get_enum(Decoder& in, std::uint8_t low, std::uint8_t high,
                                     const char* what) {
  Outcome<std::uint8_t> raw = in.u8();
  if (!raw.ok()) {
    return fail<Enum>(raw.code(), raw.error().message);
  }
  if (raw.value() < low || raw.value() > high) {
    return fail<Enum>(EnsembleError::invalid_enum_value,
                      std::string("invalid ") + what + " value in serialized input");
  }
  return static_cast<Enum>(raw.value());
}

void put_id(Encoder& out, std::uint64_t value) { out.u64(value); }

[[nodiscard]] Status put_string(Encoder& out, const std::string& value, std::uint32_t max_bytes,
                                const char* what) {
  if (value.size() > max_bytes) {
    return fail(EnsembleError::resource_limit_exceeded,
                std::string("serialization: ") + what + " exceeds the configured bound");
  }
  out.string(value);
  return ok_status();
}

[[nodiscard]] Outcome<std::string> get_string(Decoder& in, std::uint32_t max_bytes,
                                              const char* what) {
  Outcome<std::string> value = in.string(max_bytes);
  if (!value.ok()) {
    return fail<std::string>(value.code(), std::string(what) + ": " + value.error().message);
  }
  return value;
}

void put_criterion(Encoder& out, const CriterionRef& criterion) {
  out.u32(criterion.id);
  out.u32(criterion.version);
}

[[nodiscard]] Outcome<CriterionRef> get_criterion(Decoder& in) {
  Outcome<std::uint32_t> id = in.u32();
  if (!id.ok()) return fail<CriterionRef>(id.code(), id.error().message);
  Outcome<std::uint32_t> version = in.u32();
  if (!version.ok()) return fail<CriterionRef>(version.code(), version.error().message);
  CriterionRef criterion;
  criterion.id = id.value();
  criterion.version = version.value();
  return criterion;
}

void put_compatibility_profile(Encoder& out, const CompatibilityProfile& profile) {
  out.u16(profile.protocol_version);
  out.u32(profile.capability_mask);
  out.u32(profile.task_class);
  out.u32(profile.output_schema);
  out.u32(profile.model_family);
  out.u32(profile.backend_features);
}

[[nodiscard]] Outcome<CompatibilityProfile> get_compatibility_profile(Decoder& in) {
  CompatibilityProfile profile;
  Outcome<std::uint16_t> version = in.u16();
  if (!version.ok()) return fail<CompatibilityProfile>(version.code(), version.error().message);
  profile.protocol_version = version.value();
  Outcome<std::uint32_t> capability = in.u32();
  if (!capability.ok()) return fail<CompatibilityProfile>(capability.code(), capability.error().message);
  profile.capability_mask = capability.value();
  Outcome<std::uint32_t> task = in.u32();
  if (!task.ok()) return fail<CompatibilityProfile>(task.code(), task.error().message);
  profile.task_class = task.value();
  Outcome<std::uint32_t> schema = in.u32();
  if (!schema.ok()) return fail<CompatibilityProfile>(schema.code(), schema.error().message);
  profile.output_schema = schema.value();
  Outcome<std::uint32_t> family = in.u32();
  if (!family.ok()) return fail<CompatibilityProfile>(family.code(), family.error().message);
  profile.model_family = family.value();
  Outcome<std::uint32_t> features = in.u32();
  if (!features.ok()) return fail<CompatibilityProfile>(features.code(), features.error().message);
  profile.backend_features = features.value();
  return profile;
}

void put_compatibility_requirement(Encoder& out, const CompatibilityRequirement& requirement) {
  out.u16(requirement.min_protocol_version);
  out.u32(requirement.required_capabilities);
  out.u32(requirement.required_task_class);
  out.u32(requirement.required_output_schema);
  out.u32(requirement.excluded_model_family);
  out.u32(requirement.required_backend_features);
  out.boolean(requirement.require_model_family_diversity);
}

[[nodiscard]] Outcome<CompatibilityRequirement> get_compatibility_requirement(Decoder& in) {
  CompatibilityRequirement requirement;
  Outcome<std::uint16_t> version = in.u16();
  if (!version.ok()) return fail<CompatibilityRequirement>(version.code(), version.error().message);
  requirement.min_protocol_version = version.value();
  const char* names[] = {"required_capabilities", "required_task_class", "required_output_schema",
                         "excluded_model_family", "required_backend_features"};
  std::uint32_t* targets[] = {&requirement.required_capabilities, &requirement.required_task_class,
                              &requirement.required_output_schema,
                              &requirement.excluded_model_family,
                              &requirement.required_backend_features};
  for (std::size_t index = 0; index < 5u; ++index) {
    Outcome<std::uint32_t> value = in.u32();
    if (!value.ok()) {
      return fail<CompatibilityRequirement>(value.code(),
                                            std::string(names[index]) + ": " + value.error().message);
    }
    *targets[index] = value.value();
  }
  Outcome<bool> diversity = in.boolean();
  if (!diversity.ok()) {
    return fail<CompatibilityRequirement>(diversity.code(), diversity.error().message);
  }
  requirement.require_model_family_diversity = diversity.value();
  return requirement;
}

void put_participant_spec(Encoder& out, const ParticipantSpec& participant) {
  put_id(out, participant.id.value());
  out.string(participant.label);
  out.u32(participant.roles.mask());
  out.boolean(participant.required);
  out.string(participant.domain);
  out.u32(participant.priority);
  out.u32(participant.execution_budget_units);
  out.boolean(participant.selectable_override.has_value());
  if (participant.selectable_override.has_value()) {
    out.boolean(participant.selectable_override.value());
  }
  put_compatibility_requirement(out, participant.compatibility);
}

[[nodiscard]] Outcome<ParticipantSpec> get_participant_spec(Decoder& in, const Limits& limits) {
  ParticipantSpec participant;
  Outcome<std::uint64_t> id = in.u64();
  if (!id.ok()) return fail<ParticipantSpec>(id.code(), id.error().message);
  participant.id = ParticipantId(id.value());
  Outcome<std::string> label = get_string(in, limits.max_label_bytes, "participant label");
  if (!label.ok()) return fail<ParticipantSpec>(label.code(), label.error().message);
  participant.label = label.value();
  Outcome<std::uint32_t> roles = in.u32();
  if (!roles.ok()) return fail<ParticipantSpec>(roles.code(), roles.error().message);
  if ((roles.value() & ~0x1Fu) != 0u) {
    return fail<ParticipantSpec>(EnsembleError::invalid_enum_value,
                                 "participant declares an unknown role bit");
  }
  participant.roles = RoleSet(roles.value());
  Outcome<bool> required = in.boolean();
  if (!required.ok()) return fail<ParticipantSpec>(required.code(), required.error().message);
  participant.required = required.value();
  Outcome<std::string> domain = get_string(in, limits.max_metadata_bytes, "participant domain");
  if (!domain.ok()) return fail<ParticipantSpec>(domain.code(), domain.error().message);
  participant.domain = domain.value();
  Outcome<std::uint32_t> priority = in.u32();
  if (!priority.ok()) return fail<ParticipantSpec>(priority.code(), priority.error().message);
  participant.priority = priority.value();
  Outcome<std::uint32_t> budget = in.u32();
  if (!budget.ok()) return fail<ParticipantSpec>(budget.code(), budget.error().message);
  participant.execution_budget_units = budget.value();
  Outcome<bool> has_override = in.boolean();
  if (!has_override.ok()) return fail<ParticipantSpec>(has_override.code(), has_override.error().message);
  if (has_override.value()) {
    Outcome<bool> value = in.boolean();
    if (!value.ok()) return fail<ParticipantSpec>(value.code(), value.error().message);
    participant.selectable_override = value.value();
  }
  Outcome<CompatibilityRequirement> requirement = get_compatibility_requirement(in);
  if (!requirement.ok()) return fail<ParticipantSpec>(requirement.code(), requirement.error().message);
  participant.compatibility = requirement.value();
  return participant;
}

void put_stage_spec(Encoder& out, const StageSpec& stage) {
  put_id(out, stage.id.value());
  out.string(stage.label);
  put_enum(out, stage.mode);
  out.u32(static_cast<std::uint32_t>(stage.participants.size()));
  for (const ParticipantId id : stage.participants) {
    put_id(out, id.value());
  }
  out.u32(static_cast<std::uint32_t>(stage.required_domains.size()));
  for (const std::string& domain : stage.required_domains) {
    out.string(domain);
  }
  out.boolean(stage.fallback_stage);
}

[[nodiscard]] Outcome<StageSpec> get_stage_spec(Decoder& in, const Limits& limits) {
  StageSpec stage;
  Outcome<std::uint64_t> id = in.u64();
  if (!id.ok()) return fail<StageSpec>(id.code(), id.error().message);
  stage.id = StageId(id.value());
  Outcome<std::string> label = get_string(in, limits.max_label_bytes, "stage label");
  if (!label.ok()) return fail<StageSpec>(label.code(), label.error().message);
  stage.label = label.value();
  Outcome<TopologyMode> mode = get_enum<TopologyMode>(in, 1u, 2u, "topology mode");
  if (!mode.ok()) return fail<StageSpec>(mode.code(), mode.error().message);
  stage.mode = mode.value();
  Outcome<std::uint32_t> count = in.u32();
  if (!count.ok()) return fail<StageSpec>(count.code(), count.error().message);
  if (count.value() > limits.max_parallel_branches) {
    return fail<StageSpec>(EnsembleError::resource_limit_exceeded,
                           "declared stage participant count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    Outcome<std::uint64_t> participant = in.u64();
    if (!participant.ok()) return fail<StageSpec>(participant.code(), participant.error().message);
    stage.participants.push_back(ParticipantId(participant.value()));
  }
  Outcome<std::uint32_t> domains = in.u32();
  if (!domains.ok()) return fail<StageSpec>(domains.code(), domains.error().message);
  if (domains.value() > limits.max_participants_per_ensemble) {
    return fail<StageSpec>(EnsembleError::resource_limit_exceeded,
                           "declared domain count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < domains.value(); ++index) {
    Outcome<std::string> domain = get_string(in, limits.max_metadata_bytes, "required domain");
    if (!domain.ok()) return fail<StageSpec>(domain.code(), domain.error().message);
    stage.required_domains.push_back(domain.value());
  }
  Outcome<bool> fallback = in.boolean();
  if (!fallback.ok()) return fail<StageSpec>(fallback.code(), fallback.error().message);
  stage.fallback_stage = fallback.value();
  return stage;
}

void put_quorum_policy(Encoder& out, const QuorumPolicy& policy) {
  out.u32(policy.min_participants);
  out.u32(static_cast<std::uint32_t>(policy.role_quota.size()));
  for (const RoleQuota& quota : policy.role_quota) {
    put_enum(out, quota.role);
    out.u32(quota.min_count);
  }
  out.u32(policy.min_evaluations);
  out.u32(policy.min_valid_candidates);
  out.u32(policy.consensus_threshold_percent);
  put_enum(out, policy.vote_basis);
  out.boolean(policy.count_abstentions);
  out.boolean(policy.count_optional_participants);
  out.boolean(policy.count_failed_in_denominator);
  out.boolean(policy.count_unavailable_in_denominator);
  out.u32(policy.max_failures);
  out.u32(policy.max_abstentions);
}

[[nodiscard]] Outcome<QuorumPolicy> get_quorum_policy(Decoder& in, const Limits& limits) {
  QuorumPolicy policy;
  Outcome<std::uint32_t> minimum = in.u32();
  if (!minimum.ok()) return fail<QuorumPolicy>(minimum.code(), minimum.error().message);
  policy.min_participants = minimum.value();
  Outcome<std::uint32_t> quotas = in.u32();
  if (!quotas.ok()) return fail<QuorumPolicy>(quotas.code(), quotas.error().message);
  if (quotas.value() > limits.max_role_quota_entries) {
    return fail<QuorumPolicy>(EnsembleError::resource_limit_exceeded,
                              "declared role quota count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < quotas.value(); ++index) {
    Outcome<ParticipantRole> role = get_enum<ParticipantRole>(in, 1u, 5u, "participant role");
    if (!role.ok()) return fail<QuorumPolicy>(role.code(), role.error().message);
    Outcome<std::uint32_t> count = in.u32();
    if (!count.ok()) return fail<QuorumPolicy>(count.code(), count.error().message);
    RoleQuota quota;
    quota.role = role.value();
    quota.min_count = count.value();
    policy.role_quota.push_back(quota);
  }
  Outcome<std::uint32_t> evaluations = in.u32();
  if (!evaluations.ok()) return fail<QuorumPolicy>(evaluations.code(), evaluations.error().message);
  policy.min_evaluations = evaluations.value();
  Outcome<std::uint32_t> candidates = in.u32();
  if (!candidates.ok()) return fail<QuorumPolicy>(candidates.code(), candidates.error().message);
  policy.min_valid_candidates = candidates.value();
  Outcome<std::uint32_t> threshold = in.u32();
  if (!threshold.ok()) return fail<QuorumPolicy>(threshold.code(), threshold.error().message);
  policy.consensus_threshold_percent = threshold.value();
  Outcome<VoteBasis> basis = get_enum<VoteBasis>(in, 1u, 5u, "vote basis");
  if (!basis.ok()) return fail<QuorumPolicy>(basis.code(), basis.error().message);
  policy.vote_basis = basis.value();
  const char* flags[] = {"count_abstentions", "count_optional_participants",
                         "count_failed_in_denominator", "count_unavailable_in_denominator"};
  bool* targets[] = {&policy.count_abstentions, &policy.count_optional_participants,
                     &policy.count_failed_in_denominator, &policy.count_unavailable_in_denominator};
  for (std::size_t index = 0; index < 4u; ++index) {
    Outcome<bool> value = in.boolean();
    if (!value.ok()) {
      return fail<QuorumPolicy>(value.code(), std::string(flags[index]) + ": " + value.error().message);
    }
    *targets[index] = value.value();
  }
  Outcome<std::uint32_t> failures = in.u32();
  if (!failures.ok()) return fail<QuorumPolicy>(failures.code(), failures.error().message);
  policy.max_failures = failures.value();
  Outcome<std::uint32_t> abstentions = in.u32();
  if (!abstentions.ok()) return fail<QuorumPolicy>(abstentions.code(), abstentions.error().message);
  policy.max_abstentions = abstentions.value();
  return policy;
}

void put_evidence_requirement(Encoder& out, const EvidenceRequirement& evidence) {
  out.u32(evidence.min_candidate_payload_bytes);
  out.u32(evidence.min_evaluations_per_candidate);
  out.boolean(evidence.require_rationale);
  out.u32(static_cast<std::uint32_t>(evidence.mandatory_criteria.size()));
  for (const CriterionRef& criterion : evidence.mandatory_criteria) {
    put_criterion(out, criterion);
  }
  out.boolean(evidence.unknown_verification_is_failure);
  out.boolean(evidence.abstention_is_failure);
}

[[nodiscard]] Outcome<EvidenceRequirement> get_evidence_requirement(Decoder& in,
                                                                    const Limits& limits) {
  EvidenceRequirement evidence;
  Outcome<std::uint32_t> payload = in.u32();
  if (!payload.ok()) return fail<EvidenceRequirement>(payload.code(), payload.error().message);
  evidence.min_candidate_payload_bytes = payload.value();
  if (evidence.min_candidate_payload_bytes > limits.max_candidate_payload_bytes) {
    return fail<EvidenceRequirement>(EnsembleError::resource_limit_exceeded,
                                     "minimum candidate payload exceeds the configured bound");
  }
  Outcome<std::uint32_t> evaluations = in.u32();
  if (!evaluations.ok()) return fail<EvidenceRequirement>(evaluations.code(), evaluations.error().message);
  evidence.min_evaluations_per_candidate = evaluations.value();
  Outcome<bool> rationale = in.boolean();
  if (!rationale.ok()) return fail<EvidenceRequirement>(rationale.code(), rationale.error().message);
  evidence.require_rationale = rationale.value();
  Outcome<std::uint32_t> criteria = in.u32();
  if (!criteria.ok()) return fail<EvidenceRequirement>(criteria.code(), criteria.error().message);
  if (criteria.value() > limits.max_role_quota_entries * 4u) {
    return fail<EvidenceRequirement>(EnsembleError::resource_limit_exceeded,
                                     "declared criterion count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < criteria.value(); ++index) {
    Outcome<CriterionRef> criterion = get_criterion(in);
    if (!criterion.ok()) return fail<EvidenceRequirement>(criterion.code(), criterion.error().message);
    evidence.mandatory_criteria.push_back(criterion.value());
  }
  Outcome<bool> unknown = in.boolean();
  if (!unknown.ok()) return fail<EvidenceRequirement>(unknown.code(), unknown.error().message);
  evidence.unknown_verification_is_failure = unknown.value();
  Outcome<bool> abstention = in.boolean();
  if (!abstention.ok()) return fail<EvidenceRequirement>(abstention.code(), abstention.error().message);
  evidence.abstention_is_failure = abstention.value();
  return evidence;
}

}  // namespace

std::vector<std::byte> encode_spec(const EnsembleSpec& spec, const Limits& limits) {
  Encoder out(limits, 1024);
  out.u32(0u);  // reserved for forward-compatible extensions
  out.u64(spec.id.value());
  out.u64(spec.generation.value());
  out.string(spec.label);
  out.string(spec.task_class);
  out.u32(spec.task_class_id);
  out.u64(spec.fingerprint);
  out.u32(static_cast<std::uint32_t>(spec.participants.size()));
  for (const ParticipantSpec& participant : spec.participants) {
    put_participant_spec(out, participant);
  }
  out.u32(static_cast<std::uint32_t>(spec.stages.size()));
  for (const StageSpec& stage : spec.stages) {
    put_stage_spec(out, stage);
  }
  put_quorum_policy(out, spec.quorum);
  put_enum(out, spec.aggregation.kind);
  out.boolean(spec.aggregation.use_weights);
  out.boolean(spec.aggregation.require_verification_pass);
  out.boolean(spec.aggregation.require_judge_acceptance);
  out.f64(spec.aggregation.min_aggregate_score);
  out.f64(spec.aggregation.min_judge_acceptance_ratio);
  out.u32(static_cast<std::uint32_t>(spec.arbitration.factors.size()));
  for (const ArbitrationFactor factor : spec.arbitration.factors) {
    put_enum(out, factor);
  }
  put_enum(out, spec.arbitration.tie_break);
  out.boolean(spec.arbitration.require_unique_winner);
  out.u32(spec.arbitration.max_selected);
  out.boolean(spec.fallback.enabled);
  out.u32(static_cast<std::uint32_t>(spec.fallback.triggers.size()));
  for (const FallbackTrigger trigger : spec.fallback.triggers) {
    put_enum(out, trigger);
  }
  out.u32(static_cast<std::uint32_t>(spec.fallback.fallback_participants.size()));
  for (const ParticipantId id : spec.fallback.fallback_participants) {
    put_id(out, id.value());
  }
  out.boolean(spec.fallback.eager_speculative);
  out.u32(spec.fallback.max_depth);
  out.boolean(spec.completion.require_consensus);
  out.boolean(spec.completion.require_all_required_participants);
  out.boolean(spec.completion.allow_partial_results);
  out.u32(spec.retry.max_attempts);
  out.boolean(spec.retry.retry_on_unavailable);
  out.boolean(spec.retry.retry_on_transport_error);
  out.boolean(spec.retry.retry_on_malformed_candidate);
  out.boolean(spec.retry.retry_on_participant_failed);
  out.boolean(spec.retry.replace_participant);
  out.boolean(spec.retry.rerun_evaluations);
  out.boolean(spec.cancellation.cancel_on_required_failure);
  out.boolean(spec.cancellation.cancel_on_quorum_impossible);
  out.boolean(spec.cancellation.preserve_committed_results);
  put_evidence_requirement(out, spec.evidence);
  out.u32(static_cast<std::uint32_t>(spec.judging_criteria.size()));
  for (const CriterionRef& criterion : spec.judging_criteria) {
    put_criterion(out, criterion);
  }
  put_compatibility_requirement(out, spec.requirements);
  out.u32(spec.overall_budget_units);
  out.u64(spec.deterministic_seed);
  return out.take();
}

Outcome<EnsembleSpec> decode_spec(std::span<const std::byte> data, const Limits& limits) {
  Decoder in(data, limits);
  EnsembleSpec spec;
  Outcome<std::uint32_t> reserved = in.u32();
  if (!reserved.ok()) return fail<EnsembleSpec>(reserved.code(), reserved.error().message);
  if (reserved.value() != 0u) {
    return fail<EnsembleSpec>(EnsembleError::unsupported_version,
                              "specification encoding uses an unsupported extension word");
  }
  Outcome<std::uint64_t> id = in.u64();
  if (!id.ok()) return fail<EnsembleSpec>(id.code(), id.error().message);
  spec.id = EnsembleId(id.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<EnsembleSpec>(generation.code(), generation.error().message);
  spec.generation = EnsembleGeneration(generation.value());
  Outcome<std::string> label = get_string(in, limits.max_label_bytes, "ensemble label");
  if (!label.ok()) return fail<EnsembleSpec>(label.code(), label.error().message);
  spec.label = label.value();
  Outcome<std::string> task = get_string(in, limits.max_metadata_bytes, "task class");
  if (!task.ok()) return fail<EnsembleSpec>(task.code(), task.error().message);
  spec.task_class = task.value();
  Outcome<std::uint32_t> task_id = in.u32();
  if (!task_id.ok()) return fail<EnsembleSpec>(task_id.code(), task_id.error().message);
  spec.task_class_id = task_id.value();
  Outcome<std::uint64_t> fingerprint = in.u64();
  if (!fingerprint.ok()) return fail<EnsembleSpec>(fingerprint.code(), fingerprint.error().message);
  spec.fingerprint = fingerprint.value();
  Outcome<std::uint32_t> participants = in.u32();
  if (!participants.ok()) return fail<EnsembleSpec>(participants.code(), participants.error().message);
  if (participants.value() > limits.max_participants_per_ensemble) {
    return fail<EnsembleSpec>(EnsembleError::resource_limit_exceeded,
                              "declared participant count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < participants.value(); ++index) {
    Outcome<ParticipantSpec> participant = get_participant_spec(in, limits);
    if (!participant.ok()) return fail<EnsembleSpec>(participant.code(), participant.error().message);
    spec.participants.push_back(std::move(participant).value());
  }
  Outcome<std::uint32_t> stages = in.u32();
  if (!stages.ok()) return fail<EnsembleSpec>(stages.code(), stages.error().message);
  if (stages.value() > limits.max_stages_per_ensemble) {
    return fail<EnsembleSpec>(EnsembleError::resource_limit_exceeded,
                              "declared stage count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < stages.value(); ++index) {
    Outcome<StageSpec> stage = get_stage_spec(in, limits);
    if (!stage.ok()) return fail<EnsembleSpec>(stage.code(), stage.error().message);
    spec.stages.push_back(std::move(stage).value());
  }
  Outcome<QuorumPolicy> quorum = get_quorum_policy(in, limits);
  if (!quorum.ok()) return fail<EnsembleSpec>(quorum.code(), quorum.error().message);
  spec.quorum = quorum.value();
  Outcome<AggregationKind> aggregation = get_enum<AggregationKind>(in, 1u, 6u, "aggregation kind");
  if (!aggregation.ok()) return fail<EnsembleSpec>(aggregation.code(), aggregation.error().message);
  spec.aggregation.kind = aggregation.value();
  const char* aggregation_flags[] = {"use_weights", "require_verification_pass",
                                     "require_judge_acceptance"};
  bool* aggregation_targets[] = {&spec.aggregation.use_weights,
                                 &spec.aggregation.require_verification_pass,
                                 &spec.aggregation.require_judge_acceptance};
  for (std::size_t index = 0; index < 3u; ++index) {
    Outcome<bool> value = in.boolean();
    if (!value.ok()) return fail<EnsembleSpec>(value.code(), value.error().message);
    *aggregation_targets[index] = value.value();
  }
  Outcome<double> min_score = in.f64();
  if (!min_score.ok()) return fail<EnsembleSpec>(min_score.code(), min_score.error().message);
  spec.aggregation.min_aggregate_score = min_score.value();
  Outcome<double> min_ratio = in.f64();
  if (!min_ratio.ok()) return fail<EnsembleSpec>(min_ratio.code(), min_ratio.error().message);
  spec.aggregation.min_judge_acceptance_ratio = min_ratio.value();
  Outcome<std::uint32_t> factors = in.u32();
  if (!factors.ok()) return fail<EnsembleSpec>(factors.code(), factors.error().message);
  if (factors.value() > 16u) {
    return fail<EnsembleSpec>(EnsembleError::resource_limit_exceeded,
                              "declared arbitration factor count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < factors.value(); ++index) {
    Outcome<ArbitrationFactor> factor = get_enum<ArbitrationFactor>(in, 1u, 13u, "arbitration factor");
    if (!factor.ok()) return fail<EnsembleSpec>(factor.code(), factor.error().message);
    spec.arbitration.factors.push_back(factor.value());
  }
  Outcome<TieBreakPolicy> tie = get_enum<TieBreakPolicy>(in, 1u, 4u, "tie break policy");
  if (!tie.ok()) return fail<EnsembleSpec>(tie.code(), tie.error().message);
  spec.arbitration.tie_break = tie.value();
  Outcome<bool> unique = in.boolean();
  if (!unique.ok()) return fail<EnsembleSpec>(unique.code(), unique.error().message);
  spec.arbitration.require_unique_winner = unique.value();
  Outcome<std::uint32_t> selected = in.u32();
  if (!selected.ok()) return fail<EnsembleSpec>(selected.code(), selected.error().message);
  spec.arbitration.max_selected = selected.value();
  Outcome<bool> fallback_enabled = in.boolean();
  if (!fallback_enabled.ok()) return fail<EnsembleSpec>(fallback_enabled.code(), fallback_enabled.error().message);
  spec.fallback.enabled = fallback_enabled.value();
  Outcome<std::uint32_t> triggers = in.u32();
  if (!triggers.ok()) return fail<EnsembleSpec>(triggers.code(), triggers.error().message);
  if (triggers.value() > 8u) {
    return fail<EnsembleSpec>(EnsembleError::resource_limit_exceeded,
                              "declared fallback trigger count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < triggers.value(); ++index) {
    Outcome<FallbackTrigger> trigger = get_enum<FallbackTrigger>(in, 1u, 8u, "fallback trigger");
    if (!trigger.ok()) return fail<EnsembleSpec>(trigger.code(), trigger.error().message);
    spec.fallback.triggers.push_back(trigger.value());
  }
  Outcome<std::uint32_t> fallback_participants = in.u32();
  if (!fallback_participants.ok())
    return fail<EnsembleSpec>(fallback_participants.code(), fallback_participants.error().message);
  if (fallback_participants.value() > limits.max_participants_per_ensemble) {
    return fail<EnsembleSpec>(EnsembleError::resource_limit_exceeded,
                              "declared fallback participant count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < fallback_participants.value(); ++index) {
    Outcome<std::uint64_t> participant = in.u64();
    if (!participant.ok()) return fail<EnsembleSpec>(participant.code(), participant.error().message);
    spec.fallback.fallback_participants.push_back(ParticipantId(participant.value()));
  }
  Outcome<bool> eager = in.boolean();
  if (!eager.ok()) return fail<EnsembleSpec>(eager.code(), eager.error().message);
  spec.fallback.eager_speculative = eager.value();
  Outcome<std::uint32_t> depth = in.u32();
  if (!depth.ok()) return fail<EnsembleSpec>(depth.code(), depth.error().message);
  spec.fallback.max_depth = depth.value();
  const char* completion_flags[] = {"require_consensus", "require_all_required_participants",
                                    "allow_partial_results"};
  bool* completion_targets[] = {&spec.completion.require_consensus,
                                &spec.completion.require_all_required_participants,
                                &spec.completion.allow_partial_results};
  for (std::size_t index = 0; index < 3u; ++index) {
    Outcome<bool> value = in.boolean();
    if (!value.ok()) return fail<EnsembleSpec>(value.code(), value.error().message);
    *completion_targets[index] = value.value();
  }
  Outcome<std::uint32_t> attempts = in.u32();
  if (!attempts.ok()) return fail<EnsembleSpec>(attempts.code(), attempts.error().message);
  spec.retry.max_attempts = attempts.value();
  const char* retry_flags[] = {"retry_on_unavailable", "retry_on_transport_error",
                               "retry_on_malformed_candidate", "retry_on_participant_failed",
                               "replace_participant", "rerun_evaluations"};
  bool* retry_targets[] = {&spec.retry.retry_on_unavailable, &spec.retry.retry_on_transport_error,
                           &spec.retry.retry_on_malformed_candidate,
                           &spec.retry.retry_on_participant_failed,
                           &spec.retry.replace_participant, &spec.retry.rerun_evaluations};
  for (std::size_t index = 0; index < 6u; ++index) {
    Outcome<bool> value = in.boolean();
    if (!value.ok()) return fail<EnsembleSpec>(value.code(), value.error().message);
    *retry_targets[index] = value.value();
  }
  const char* cancellation_flags[] = {"cancel_on_required_failure", "cancel_on_quorum_impossible",
                                      "preserve_committed_results"};
  bool* cancellation_targets[] = {&spec.cancellation.cancel_on_required_failure,
                                  &spec.cancellation.cancel_on_quorum_impossible,
                                  &spec.cancellation.preserve_committed_results};
  for (std::size_t index = 0; index < 3u; ++index) {
    Outcome<bool> value = in.boolean();
    if (!value.ok()) return fail<EnsembleSpec>(value.code(), value.error().message);
    *cancellation_targets[index] = value.value();
  }
  Outcome<EvidenceRequirement> evidence = get_evidence_requirement(in, limits);
  if (!evidence.ok()) return fail<EnsembleSpec>(evidence.code(), evidence.error().message);
  spec.evidence = evidence.value();
  Outcome<std::uint32_t> judging = in.u32();
  if (!judging.ok()) return fail<EnsembleSpec>(judging.code(), judging.error().message);
  if (judging.value() > 16u) {
    return fail<EnsembleSpec>(EnsembleError::resource_limit_exceeded,
                              "declared judging criterion count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < judging.value(); ++index) {
    Outcome<CriterionRef> criterion = get_criterion(in);
    if (!criterion.ok()) return fail<EnsembleSpec>(criterion.code(), criterion.error().message);
    spec.judging_criteria.push_back(criterion.value());
  }
  Outcome<CompatibilityRequirement> requirements = get_compatibility_requirement(in);
  if (!requirements.ok()) return fail<EnsembleSpec>(requirements.code(), requirements.error().message);
  spec.requirements = requirements.value();
  Outcome<std::uint32_t> budget = in.u32();
  if (!budget.ok()) return fail<EnsembleSpec>(budget.code(), budget.error().message);
  spec.overall_budget_units = budget.value();
  Outcome<std::uint64_t> seed = in.u64();
  if (!seed.ok()) return fail<EnsembleSpec>(seed.code(), seed.error().message);
  spec.deterministic_seed = seed.value();

  const Status exhausted = in.require_exhausted("ensemble specification");
  if (!exhausted.ok()) {
    return fail<EnsembleSpec>(exhausted.code(), exhausted.error().message);
  }
  const Status structural = validate_spec(spec, limits);
  if (!structural.ok()) {
    return fail<EnsembleSpec>(structural.code(), structural.error().message);
  }
  return spec;
}

std::vector<std::byte> encode_report(const std::string& text, const Limits& limits) {
  Encoder out(limits, text.size() + 16u);
  out.string(text);
  return out.take();
}

Outcome<std::string> decode_report(std::span<const std::byte> data, const Limits& limits) {
  Decoder in(data, limits);
  Outcome<std::string> text = in.string(limits.max_explanation_entries * 512u);
  if (!text.ok()) {
    return fail<std::string>(text.code(), text.error().message);
  }
  const Status exhausted = in.require_exhausted("report");
  if (!exhausted.ok()) {
    return fail<std::string>(exhausted.code(), exhausted.error().message);
  }
  return text;
}

// ---------------------------------------------------------------------------
// Result and report serialization
// ---------------------------------------------------------------------------
namespace {

void put_id_list(Encoder& out, const std::vector<ParticipantId>& ids) {
  out.u32(static_cast<std::uint32_t>(ids.size()));
  for (const ParticipantId id : ids) {
    put_id(out, id.value());
  }
}

[[nodiscard]] Outcome<std::vector<ParticipantId>> get_id_list(Decoder& in, std::uint32_t max_count) {
  Outcome<std::uint32_t> count = in.u32();
  if (!count.ok()) return fail<std::vector<ParticipantId>>(count.code(), count.error().message);
  if (count.value() > max_count) {
    return fail<std::vector<ParticipantId>>(EnsembleError::resource_limit_exceeded,
                                            "declared identity list exceeds the configured bound");
  }
  std::vector<ParticipantId> ids;
  ids.reserve(count.value());
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    Outcome<std::uint64_t> value = in.u64();
    if (!value.ok()) return fail<std::vector<ParticipantId>>(value.code(), value.error().message);
    ids.push_back(ParticipantId(value.value()));
  }
  return ids;
}

void put_quorum_report(Encoder& out, const QuorumReport& report) {
  put_enum(out, report.state);
  put_enum(out, report.basis);
  out.u32(report.denominator);
  out.u32(report.contributors);
  out.u32(report.abstentions);
  out.u32(report.failures);
  out.u32(report.excluded_optional);
  out.u32(report.excluded_unavailable);
  out.u32(report.excluded_stale);
  out.u32(report.required_total);
  out.u32(report.required_present);
  out.u32(report.evaluations);
  out.u32(report.valid_candidates);
  out.u32(report.duplicate_votes_suppressed);
  out.u32(report.stale_votes_rejected);
  out.u32(report.threshold_percent);
  out.u32(report.threshold_count);
  out.u32(report.leading_votes);
  out.u32(report.role_quota_met);
  out.u32(report.role_quota_required);
  out.boolean(report.failure_budget_exceeded);
  out.boolean(report.abstention_budget_exceeded);
  out.u64(report.ensemble_generation.value());
  out.u64(report.coordinator_epoch.value());
  out.u64(report.computed_sequence.value());
  put_id_list(out, report.members);
  out.u32(static_cast<std::uint32_t>(report.member_generations.size()));
  for (const ParticipantGeneration generation : report.member_generations) {
    out.u64(generation.value());
  }
  put_id_list(out, report.contributors_list);
  put_id_list(out, report.abstaining_members);
  put_id_list(out, report.failed_members);
  out.u32(static_cast<std::uint32_t>(report.notes.size()));
  for (const std::string& note : report.notes) {
    out.string(note);
  }
}

[[nodiscard]] Outcome<QuorumReport> get_quorum_report(Decoder& in, const Limits& limits) {
  QuorumReport report;
  Outcome<QuorumState> state = get_enum<QuorumState>(in, 0u, 4u, "quorum state");
  if (!state.ok()) return fail<QuorumReport>(state.code(), state.error().message);
  report.state = state.value();
  Outcome<VoteBasis> basis = get_enum<VoteBasis>(in, 1u, 5u, "vote basis");
  if (!basis.ok()) return fail<QuorumReport>(basis.code(), basis.error().message);
  report.basis = basis.value();
  std::uint32_t* counters[] = {&report.denominator,      &report.contributors,
                               &report.abstentions,      &report.failures,
                               &report.excluded_optional, &report.excluded_unavailable,
                               &report.excluded_stale,    &report.required_total,
                               &report.required_present,  &report.evaluations,
                               &report.valid_candidates,  &report.duplicate_votes_suppressed,
                               &report.stale_votes_rejected, &report.threshold_percent,
                               &report.threshold_count,   &report.leading_votes,
                               &report.role_quota_met,    &report.role_quota_required};
  for (std::uint32_t* target : counters) {
    Outcome<std::uint32_t> value = in.u32();
    if (!value.ok()) return fail<QuorumReport>(value.code(), value.error().message);
    *target = value.value();
  }
  Outcome<bool> failure_budget = in.boolean();
  if (!failure_budget.ok()) return fail<QuorumReport>(failure_budget.code(), failure_budget.error().message);
  report.failure_budget_exceeded = failure_budget.value();
  Outcome<bool> abstention_budget = in.boolean();
  if (!abstention_budget.ok())
    return fail<QuorumReport>(abstention_budget.code(), abstention_budget.error().message);
  report.abstention_budget_exceeded = abstention_budget.value();
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<QuorumReport>(generation.code(), generation.error().message);
  report.ensemble_generation = EnsembleGeneration(generation.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<QuorumReport>(epoch.code(), epoch.error().message);
  report.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> sequence = in.u64();
  if (!sequence.ok()) return fail<QuorumReport>(sequence.code(), sequence.error().message);
  report.computed_sequence = Sequence(sequence.value());
  Outcome<std::vector<ParticipantId>> members = get_id_list(in, limits.max_participants_per_ensemble);
  if (!members.ok()) return fail<QuorumReport>(members.code(), members.error().message);
  report.members = std::move(members).value();
  Outcome<std::uint32_t> generations = in.u32();
  if (!generations.ok()) return fail<QuorumReport>(generations.code(), generations.error().message);
  if (generations.value() > limits.max_participants_per_ensemble) {
    return fail<QuorumReport>(EnsembleError::resource_limit_exceeded,
                              "declared generation list exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < generations.value(); ++index) {
    Outcome<std::uint64_t> value = in.u64();
    if (!value.ok()) return fail<QuorumReport>(value.code(), value.error().message);
    report.member_generations.push_back(ParticipantGeneration(value.value()));
  }
  Outcome<std::vector<ParticipantId>> contributors = get_id_list(in, limits.max_participants_per_ensemble);
  if (!contributors.ok()) return fail<QuorumReport>(contributors.code(), contributors.error().message);
  report.contributors_list = std::move(contributors).value();
  Outcome<std::vector<ParticipantId>> abstaining = get_id_list(in, limits.max_participants_per_ensemble);
  if (!abstaining.ok()) return fail<QuorumReport>(abstaining.code(), abstaining.error().message);
  report.abstaining_members = std::move(abstaining).value();
  Outcome<std::vector<ParticipantId>> failed = get_id_list(in, limits.max_participants_per_ensemble);
  if (!failed.ok()) return fail<QuorumReport>(failed.code(), failed.error().message);
  report.failed_members = std::move(failed).value();
  Outcome<std::uint32_t> notes = in.u32();
  if (!notes.ok()) return fail<QuorumReport>(notes.code(), notes.error().message);
  if (notes.value() > limits.max_explanation_entries) {
    return fail<QuorumReport>(EnsembleError::resource_limit_exceeded,
                              "declared note count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < notes.value(); ++index) {
    Outcome<std::string> note = get_string(in, limits.max_metadata_bytes, "quorum note");
    if (!note.ok()) return fail<QuorumReport>(note.code(), note.error().message);
    report.notes.push_back(std::move(note).value());
  }
  return report;
}

void put_consensus_report(Encoder& out, const ConsensusReport& report) {
  put_enum(out, report.state);
  out.u64(report.leading_candidate.value());
  out.u64(report.leading_generation.value());
  out.u64(report.leading_participant.value());
  out.u32(report.agreeing);
  out.u32(report.disagreeing);
  out.u32(report.abstaining);
  out.u32(report.missing);
  out.u32(report.invalid_candidates);
  out.u32(report.eligible_candidates);
  out.u32(report.candidates_declared);
  out.u32(report.threshold_percent);
  out.u32(report.threshold_count);
  out.u32(report.vote_basis);
  out.u32(report.runner_up_votes);
  out.boolean(report.unresolved_disagreement);
  out.u64(report.ensemble_generation.value());
  out.u64(report.coordinator_epoch.value());
  out.u64(report.computed_sequence.value());
  put_id_list(out, report.agreeing_members);
  put_id_list(out, report.disagreeing_members);
  put_id_list(out, report.abstaining_members);
  put_id_list(out, report.missing_members);
  out.u32(static_cast<std::uint32_t>(report.eligible_candidates_list.size()));
  for (const CandidateId id : report.eligible_candidates_list) {
    out.u64(id.value());
  }
}

[[nodiscard]] Outcome<ConsensusReport> get_consensus_report(Decoder& in, const Limits& limits) {
  ConsensusReport report;
  Outcome<ConsensusState> state = get_enum<ConsensusState>(in, 0u, 16u, "consensus state");
  if (!state.ok()) return fail<ConsensusReport>(state.code(), state.error().message);
  report.state = state.value();
  Outcome<std::uint64_t> leading = in.u64();
  if (!leading.ok()) return fail<ConsensusReport>(leading.code(), leading.error().message);
  report.leading_candidate = CandidateId(leading.value());
  Outcome<std::uint64_t> leading_generation = in.u64();
  if (!leading_generation.ok())
    return fail<ConsensusReport>(leading_generation.code(), leading_generation.error().message);
  report.leading_generation = CandidateGeneration(leading_generation.value());
  Outcome<std::uint64_t> leading_participant = in.u64();
  if (!leading_participant.ok())
    return fail<ConsensusReport>(leading_participant.code(), leading_participant.error().message);
  report.leading_participant = ParticipantId(leading_participant.value());
  std::uint32_t* counters[] = {&report.agreeing,       &report.disagreeing,
                               &report.abstaining,     &report.missing,
                               &report.invalid_candidates, &report.eligible_candidates,
                               &report.candidates_declared, &report.threshold_percent,
                               &report.threshold_count, &report.vote_basis,
                               &report.runner_up_votes};
  for (std::uint32_t* target : counters) {
    Outcome<std::uint32_t> value = in.u32();
    if (!value.ok()) return fail<ConsensusReport>(value.code(), value.error().message);
    *target = value.value();
  }
  Outcome<bool> unresolved = in.boolean();
  if (!unresolved.ok()) return fail<ConsensusReport>(unresolved.code(), unresolved.error().message);
  report.unresolved_disagreement = unresolved.value();
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<ConsensusReport>(generation.code(), generation.error().message);
  report.ensemble_generation = EnsembleGeneration(generation.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<ConsensusReport>(epoch.code(), epoch.error().message);
  report.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> sequence = in.u64();
  if (!sequence.ok()) return fail<ConsensusReport>(sequence.code(), sequence.error().message);
  report.computed_sequence = Sequence(sequence.value());
  Outcome<std::vector<ParticipantId>> agreeing = get_id_list(in, limits.max_participants_per_ensemble);
  if (!agreeing.ok()) return fail<ConsensusReport>(agreeing.code(), agreeing.error().message);
  report.agreeing_members = std::move(agreeing).value();
  Outcome<std::vector<ParticipantId>> disagreeing = get_id_list(in, limits.max_participants_per_ensemble);
  if (!disagreeing.ok()) return fail<ConsensusReport>(disagreeing.code(), disagreeing.error().message);
  report.disagreeing_members = std::move(disagreeing).value();
  Outcome<std::vector<ParticipantId>> abstaining = get_id_list(in, limits.max_participants_per_ensemble);
  if (!abstaining.ok()) return fail<ConsensusReport>(abstaining.code(), abstaining.error().message);
  report.abstaining_members = std::move(abstaining).value();
  Outcome<std::vector<ParticipantId>> missing = get_id_list(in, limits.max_participants_per_ensemble);
  if (!missing.ok()) return fail<ConsensusReport>(missing.code(), missing.error().message);
  report.missing_members = std::move(missing).value();
  Outcome<std::uint32_t> candidates = in.u32();
  if (!candidates.ok()) return fail<ConsensusReport>(candidates.code(), candidates.error().message);
  if (candidates.value() > limits.max_candidates_per_execution) {
    return fail<ConsensusReport>(EnsembleError::resource_limit_exceeded,
                                 "declared candidate list exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < candidates.value(); ++index) {
    Outcome<std::uint64_t> value = in.u64();
    if (!value.ok()) return fail<ConsensusReport>(value.code(), value.error().message);
    report.eligible_candidates_list.push_back(CandidateId(value.value()));
  }
  return report;
}

void put_arbitration_report(Encoder& out, const ArbitrationReport& report) {
  out.u64(report.ensemble_generation.value());
  out.u64(report.coordinator_epoch.value());
  out.u64(report.computed_sequence.value());
  put_enum(out, report.tie_break);
  out.u32(static_cast<std::uint32_t>(report.factor_order.size()));
  for (const ArbitrationFactor factor : report.factor_order) {
    put_enum(out, factor);
  }
  out.u32(static_cast<std::uint32_t>(report.ranking.size()));
  for (const CandidateRanking& ranking : report.ranking) {
    out.u64(ranking.candidate.value());
    out.u64(ranking.generation.value());
    out.u64(ranking.participant.value());
    out.u64(ranking.participant_generation.value());
    put_enum(out, ranking.role);
    out.boolean(ranking.eligible);
    put_enum(out, ranking.elimination);
    out.string(ranking.elimination_detail);
    out.u32(static_cast<std::uint32_t>(ranking.factors.size()));
    for (const RankingFactorValue& factor : ranking.factors) {
      put_enum(out, factor.factor);
      out.f64(factor.value);
      out.string(factor.detail);
    }
    out.i64(ranking.rank);
    out.boolean(ranking.selected);
  }
  out.u64(report.selected.value());
  out.u64(report.selected_generation.value());
  out.u64(report.selected_participant.value());
  put_enum(out, report.selected_role);
  out.boolean(report.tie_unresolved);
  out.boolean(report.no_eligible_candidate);
  out.u32(report.considered);
  out.u32(report.eliminated);
  out.u32(static_cast<std::uint32_t>(report.notes.size()));
  for (const std::string& note : report.notes) {
    out.string(note);
  }
}

[[nodiscard]] Outcome<ArbitrationReport> get_arbitration_report(Decoder& in, const Limits& limits) {
  ArbitrationReport report;
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<ArbitrationReport>(generation.code(), generation.error().message);
  report.ensemble_generation = EnsembleGeneration(generation.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<ArbitrationReport>(epoch.code(), epoch.error().message);
  report.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> sequence = in.u64();
  if (!sequence.ok()) return fail<ArbitrationReport>(sequence.code(), sequence.error().message);
  report.computed_sequence = Sequence(sequence.value());
  Outcome<TieBreakPolicy> tie = get_enum<TieBreakPolicy>(in, 1u, 4u, "tie break policy");
  if (!tie.ok()) return fail<ArbitrationReport>(tie.code(), tie.error().message);
  report.tie_break = tie.value();
  Outcome<std::uint32_t> factors = in.u32();
  if (!factors.ok()) return fail<ArbitrationReport>(factors.code(), factors.error().message);
  if (factors.value() > 16u) {
    return fail<ArbitrationReport>(EnsembleError::resource_limit_exceeded,
                                   "declared factor count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < factors.value(); ++index) {
    Outcome<ArbitrationFactor> factor = get_enum<ArbitrationFactor>(in, 1u, 13u, "arbitration factor");
    if (!factor.ok()) return fail<ArbitrationReport>(factor.code(), factor.error().message);
    report.factor_order.push_back(factor.value());
  }
  Outcome<std::uint32_t> rankings = in.u32();
  if (!rankings.ok()) return fail<ArbitrationReport>(rankings.code(), rankings.error().message);
  if (rankings.value() > limits.max_candidates_per_execution) {
    return fail<ArbitrationReport>(EnsembleError::resource_limit_exceeded,
                                   "declared ranking count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < rankings.value(); ++index) {
    CandidateRanking ranking;
    Outcome<std::uint64_t> candidate = in.u64();
    if (!candidate.ok()) return fail<ArbitrationReport>(candidate.code(), candidate.error().message);
    ranking.candidate = CandidateId(candidate.value());
    Outcome<std::uint64_t> candidate_generation = in.u64();
    if (!candidate_generation.ok())
      return fail<ArbitrationReport>(candidate_generation.code(), candidate_generation.error().message);
    ranking.generation = CandidateGeneration(candidate_generation.value());
    Outcome<std::uint64_t> participant = in.u64();
    if (!participant.ok()) return fail<ArbitrationReport>(participant.code(), participant.error().message);
    ranking.participant = ParticipantId(participant.value());
    Outcome<std::uint64_t> participant_generation = in.u64();
    if (!participant_generation.ok())
      return fail<ArbitrationReport>(participant_generation.code(), participant_generation.error().message);
    ranking.participant_generation = ParticipantGeneration(participant_generation.value());
    Outcome<ParticipantRole> role = get_enum<ParticipantRole>(in, 1u, 5u, "participant role");
    if (!role.ok()) return fail<ArbitrationReport>(role.code(), role.error().message);
    ranking.role = role.value();
    Outcome<bool> eligible = in.boolean();
    if (!eligible.ok()) return fail<ArbitrationReport>(eligible.code(), eligible.error().message);
    ranking.eligible = eligible.value();
    Outcome<EliminationReason> elimination = get_enum<EliminationReason>(in, 0u, 18u, "elimination reason");
    if (!elimination.ok()) return fail<ArbitrationReport>(elimination.code(), elimination.error().message);
    ranking.elimination = elimination.value();
    Outcome<std::string> detail = get_string(in, limits.max_metadata_bytes, "elimination detail");
    if (!detail.ok()) return fail<ArbitrationReport>(detail.code(), detail.error().message);
    ranking.elimination_detail = std::move(detail).value();
    Outcome<std::uint32_t> values = in.u32();
    if (!values.ok()) return fail<ArbitrationReport>(values.code(), values.error().message);
    if (values.value() > 16u) {
      return fail<ArbitrationReport>(EnsembleError::resource_limit_exceeded,
                                     "declared factor value count exceeds the configured bound");
    }
    for (std::uint32_t factor_index = 0; factor_index < values.value(); ++factor_index) {
      RankingFactorValue value;
      Outcome<ArbitrationFactor> factor = get_enum<ArbitrationFactor>(in, 1u, 13u, "arbitration factor");
      if (!factor.ok()) return fail<ArbitrationReport>(factor.code(), factor.error().message);
      value.factor = factor.value();
      Outcome<double> score = in.f64();
      if (!score.ok()) return fail<ArbitrationReport>(score.code(), score.error().message);
      value.value = score.value();
      Outcome<std::string> factor_detail = get_string(in, limits.max_metadata_bytes, "factor detail");
      if (!factor_detail.ok())
        return fail<ArbitrationReport>(factor_detail.code(), factor_detail.error().message);
      value.detail = std::move(factor_detail).value();
      ranking.factors.push_back(std::move(value));
    }
    Outcome<std::int64_t> rank = in.i64();
    if (!rank.ok()) return fail<ArbitrationReport>(rank.code(), rank.error().message);
    ranking.rank = rank.value();
    Outcome<bool> selected = in.boolean();
    if (!selected.ok()) return fail<ArbitrationReport>(selected.code(), selected.error().message);
    ranking.selected = selected.value();
    report.ranking.push_back(std::move(ranking));
  }
  Outcome<std::uint64_t> selected = in.u64();
  if (!selected.ok()) return fail<ArbitrationReport>(selected.code(), selected.error().message);
  report.selected = CandidateId(selected.value());
  Outcome<std::uint64_t> selected_generation = in.u64();
  if (!selected_generation.ok())
    return fail<ArbitrationReport>(selected_generation.code(), selected_generation.error().message);
  report.selected_generation = CandidateGeneration(selected_generation.value());
  Outcome<std::uint64_t> selected_participant = in.u64();
  if (!selected_participant.ok())
    return fail<ArbitrationReport>(selected_participant.code(), selected_participant.error().message);
  report.selected_participant = ParticipantId(selected_participant.value());
  Outcome<ParticipantRole> selected_role = get_enum<ParticipantRole>(in, 1u, 5u, "participant role");
  if (!selected_role.ok())
    return fail<ArbitrationReport>(selected_role.code(), selected_role.error().message);
  report.selected_role = selected_role.value();
  Outcome<bool> tie_unresolved = in.boolean();
  if (!tie_unresolved.ok())
    return fail<ArbitrationReport>(tie_unresolved.code(), tie_unresolved.error().message);
  report.tie_unresolved = tie_unresolved.value();
  Outcome<bool> no_eligible = in.boolean();
  if (!no_eligible.ok()) return fail<ArbitrationReport>(no_eligible.code(), no_eligible.error().message);
  report.no_eligible_candidate = no_eligible.value();
  Outcome<std::uint32_t> considered = in.u32();
  if (!considered.ok()) return fail<ArbitrationReport>(considered.code(), considered.error().message);
  report.considered = considered.value();
  Outcome<std::uint32_t> eliminated = in.u32();
  if (!eliminated.ok()) return fail<ArbitrationReport>(eliminated.code(), eliminated.error().message);
  report.eliminated = eliminated.value();
  Outcome<std::uint32_t> notes = in.u32();
  if (!notes.ok()) return fail<ArbitrationReport>(notes.code(), notes.error().message);
  if (notes.value() > limits.max_explanation_entries) {
    return fail<ArbitrationReport>(EnsembleError::resource_limit_exceeded,
                                   "declared note count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < notes.value(); ++index) {
    Outcome<std::string> note = get_string(in, limits.max_metadata_bytes, "arbitration note");
    if (!note.ok()) return fail<ArbitrationReport>(note.code(), note.error().message);
    report.notes.push_back(std::move(note).value());
  }
  return report;
}

void put_candidate_snapshot(Encoder& out, const CandidateSnapshot& snapshot) {
  out.u64(snapshot.id.value());
  out.u64(snapshot.generation.value());
  out.u64(snapshot.ensemble.value());
  out.u64(snapshot.ensemble_generation.value());
  out.u64(snapshot.execution.value());
  out.u64(snapshot.attempt_generation.value());
  out.u64(snapshot.participant.value());
  out.u64(snapshot.participant_generation.value());
  put_enum(out, snapshot.role);
  out.u64(snapshot.stage.value());
  put_enum(out, snapshot.state);
  out.u32(snapshot.attempt);
  out.u32(snapshot.attempts_used);
  out.string(snapshot.domain);
  out.bytes(snapshot.payload);
  out.u64(snapshot.payload_digest);
  out.u32(snapshot.payload_bytes);
  out.u64(snapshot.received_sequence.value());
  out.u64(snapshot.coordinator_epoch.value());
  out.u64(snapshot.worker.value());
  out.u64(snapshot.worker_boot.value());
  out.u16(static_cast<std::uint16_t>(snapshot.failure));
  out.string(snapshot.failure_detail);
  out.u32(snapshot.evaluation_count);
  out.u32(snapshot.hard_failures);
  out.boolean(snapshot.fallback);
  out.boolean(snapshot.current);
  out.string(snapshot.elimination_reason);
}

[[nodiscard]] Outcome<CandidateSnapshot> get_candidate_snapshot(Decoder& in, const Limits& limits) {
  CandidateSnapshot snapshot;
  Outcome<std::uint64_t> id = in.u64();
  if (!id.ok()) return fail<CandidateSnapshot>(id.code(), id.error().message);
  snapshot.id = CandidateId(id.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<CandidateSnapshot>(generation.code(), generation.error().message);
  snapshot.generation = CandidateGeneration(generation.value());
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<CandidateSnapshot>(ensemble.code(), ensemble.error().message);
  snapshot.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> ensemble_generation = in.u64();
  if (!ensemble_generation.ok())
    return fail<CandidateSnapshot>(ensemble_generation.code(), ensemble_generation.error().message);
  snapshot.ensemble_generation = EnsembleGeneration(ensemble_generation.value());
  Outcome<std::uint64_t> execution = in.u64();
  if (!execution.ok()) return fail<CandidateSnapshot>(execution.code(), execution.error().message);
  snapshot.execution = EnsembleExecutionId(execution.value());
  Outcome<std::uint64_t> attempt = in.u64();
  if (!attempt.ok()) return fail<CandidateSnapshot>(attempt.code(), attempt.error().message);
  snapshot.attempt_generation = EnsembleAttemptGeneration(attempt.value());
  Outcome<std::uint64_t> participant = in.u64();
  if (!participant.ok()) return fail<CandidateSnapshot>(participant.code(), participant.error().message);
  snapshot.participant = ParticipantId(participant.value());
  Outcome<std::uint64_t> participant_generation = in.u64();
  if (!participant_generation.ok())
    return fail<CandidateSnapshot>(participant_generation.code(), participant_generation.error().message);
  snapshot.participant_generation = ParticipantGeneration(participant_generation.value());
  Outcome<ParticipantRole> role = get_enum<ParticipantRole>(in, 1u, 5u, "participant role");
  if (!role.ok()) return fail<CandidateSnapshot>(role.code(), role.error().message);
  snapshot.role = role.value();
  Outcome<std::uint64_t> stage = in.u64();
  if (!stage.ok()) return fail<CandidateSnapshot>(stage.code(), stage.error().message);
  snapshot.stage = StageId(stage.value());
  Outcome<CandidateState> state = get_enum<CandidateState>(in, 1u, 13u, "candidate state");
  if (!state.ok()) return fail<CandidateSnapshot>(state.code(), state.error().message);
  snapshot.state = state.value();
  Outcome<std::uint32_t> attempt_number = in.u32();
  if (!attempt_number.ok())
    return fail<CandidateSnapshot>(attempt_number.code(), attempt_number.error().message);
  snapshot.attempt = attempt_number.value();
  Outcome<std::uint32_t> attempts_used = in.u32();
  if (!attempts_used.ok())
    return fail<CandidateSnapshot>(attempts_used.code(), attempts_used.error().message);
  snapshot.attempts_used = attempts_used.value();
  Outcome<std::string> domain = get_string(in, limits.max_metadata_bytes, "candidate domain");
  if (!domain.ok()) return fail<CandidateSnapshot>(domain.code(), domain.error().message);
  snapshot.domain = std::move(domain).value();
  Outcome<std::vector<std::byte>> payload = in.bytes(limits.max_candidate_payload_bytes);
  if (!payload.ok()) return fail<CandidateSnapshot>(payload.code(), payload.error().message);
  snapshot.payload = std::move(payload).value();
  Outcome<std::uint64_t> digest = in.u64();
  if (!digest.ok()) return fail<CandidateSnapshot>(digest.code(), digest.error().message);
  snapshot.payload_digest = digest.value();
  Outcome<std::uint32_t> bytes = in.u32();
  if (!bytes.ok()) return fail<CandidateSnapshot>(bytes.code(), bytes.error().message);
  snapshot.payload_bytes = bytes.value();
  if (snapshot.payload_bytes != snapshot.payload.size()) {
    return fail<CandidateSnapshot>(EnsembleError::malformed_persistence,
                                   "candidate payload length disagrees with its declared size");
  }
  Outcome<std::uint64_t> sequence = in.u64();
  if (!sequence.ok()) return fail<CandidateSnapshot>(sequence.code(), sequence.error().message);
  snapshot.received_sequence = Sequence(sequence.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<CandidateSnapshot>(epoch.code(), epoch.error().message);
  snapshot.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> worker = in.u64();
  if (!worker.ok()) return fail<CandidateSnapshot>(worker.code(), worker.error().message);
  snapshot.worker = WorkerId(worker.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<CandidateSnapshot>(boot.code(), boot.error().message);
  snapshot.worker_boot = WorkerBootId(boot.value());
  Outcome<std::uint16_t> failure = in.u16();
  if (!failure.ok()) return fail<CandidateSnapshot>(failure.code(), failure.error().message);
  if (failure.value() > static_cast<std::uint16_t>(EnsembleError::internal_error)) {
    return fail<CandidateSnapshot>(EnsembleError::invalid_enum_value, "invalid failure code");
  }
  snapshot.failure = static_cast<EnsembleError>(failure.value());
  Outcome<std::string> detail = get_string(in, limits.max_metadata_bytes, "candidate failure detail");
  if (!detail.ok()) return fail<CandidateSnapshot>(detail.code(), detail.error().message);
  snapshot.failure_detail = std::move(detail).value();
  Outcome<std::uint32_t> evaluations = in.u32();
  if (!evaluations.ok()) return fail<CandidateSnapshot>(evaluations.code(), evaluations.error().message);
  snapshot.evaluation_count = evaluations.value();
  Outcome<std::uint32_t> hard = in.u32();
  if (!hard.ok()) return fail<CandidateSnapshot>(hard.code(), hard.error().message);
  snapshot.hard_failures = hard.value();
  Outcome<bool> fallback = in.boolean();
  if (!fallback.ok()) return fail<CandidateSnapshot>(fallback.code(), fallback.error().message);
  snapshot.fallback = fallback.value();
  Outcome<bool> current = in.boolean();
  if (!current.ok()) return fail<CandidateSnapshot>(current.code(), current.error().message);
  snapshot.current = current.value();
  Outcome<std::string> elimination = get_string(in, limits.max_metadata_bytes, "elimination reason");
  if (!elimination.ok()) return fail<CandidateSnapshot>(elimination.code(), elimination.error().message);
  snapshot.elimination_reason = std::move(elimination).value();
  return snapshot;
}

void put_evaluation_snapshot(Encoder& out, const EvaluationSnapshot& snapshot) {
  out.u64(snapshot.id.value());
  out.u64(snapshot.generation.value());
  out.u64(snapshot.ensemble.value());
  out.u64(snapshot.ensemble_generation.value());
  out.u64(snapshot.execution.value());
  out.u64(snapshot.attempt_generation.value());
  out.u64(snapshot.evaluator.value());
  out.u64(snapshot.evaluator_generation.value());
  put_enum(out, snapshot.evaluator_role);
  out.u64(snapshot.candidate.value());
  out.u64(snapshot.candidate_generation.value());
  put_criterion(out, snapshot.criterion);
  put_enum(out, snapshot.kind);
  put_enum(out, snapshot.verification);
  put_enum(out, snapshot.judgment);
  out.boolean(snapshot.score_defined);
  out.f64(snapshot.score);
  out.f64(snapshot.weight);
  out.bytes(snapshot.evidence);
  out.string(snapshot.rationale);
  out.u64(snapshot.received_sequence.value());
  out.u64(snapshot.coordinator_epoch.value());
  out.u64(snapshot.worker.value());
  out.u64(snapshot.worker_boot.value());
  out.boolean(snapshot.current);
  out.boolean(snapshot.superseded);
}

[[nodiscard]] Outcome<EvaluationSnapshot> get_evaluation_snapshot(Decoder& in, const Limits& limits) {
  EvaluationSnapshot snapshot;
  Outcome<std::uint64_t> id = in.u64();
  if (!id.ok()) return fail<EvaluationSnapshot>(id.code(), id.error().message);
  snapshot.id = EvaluationId(id.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<EvaluationSnapshot>(generation.code(), generation.error().message);
  snapshot.generation = EvaluationGeneration(generation.value());
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<EvaluationSnapshot>(ensemble.code(), ensemble.error().message);
  snapshot.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> ensemble_generation = in.u64();
  if (!ensemble_generation.ok())
    return fail<EvaluationSnapshot>(ensemble_generation.code(), ensemble_generation.error().message);
  snapshot.ensemble_generation = EnsembleGeneration(ensemble_generation.value());
  Outcome<std::uint64_t> execution = in.u64();
  if (!execution.ok()) return fail<EvaluationSnapshot>(execution.code(), execution.error().message);
  snapshot.execution = EnsembleExecutionId(execution.value());
  Outcome<std::uint64_t> attempt = in.u64();
  if (!attempt.ok()) return fail<EvaluationSnapshot>(attempt.code(), attempt.error().message);
  snapshot.attempt_generation = EnsembleAttemptGeneration(attempt.value());
  Outcome<std::uint64_t> evaluator = in.u64();
  if (!evaluator.ok()) return fail<EvaluationSnapshot>(evaluator.code(), evaluator.error().message);
  snapshot.evaluator = ParticipantId(evaluator.value());
  Outcome<std::uint64_t> evaluator_generation = in.u64();
  if (!evaluator_generation.ok())
    return fail<EvaluationSnapshot>(evaluator_generation.code(), evaluator_generation.error().message);
  snapshot.evaluator_generation = ParticipantGeneration(evaluator_generation.value());
  Outcome<ParticipantRole> role = get_enum<ParticipantRole>(in, 1u, 5u, "participant role");
  if (!role.ok()) return fail<EvaluationSnapshot>(role.code(), role.error().message);
  snapshot.evaluator_role = role.value();
  Outcome<std::uint64_t> candidate = in.u64();
  if (!candidate.ok()) return fail<EvaluationSnapshot>(candidate.code(), candidate.error().message);
  snapshot.candidate = CandidateId(candidate.value());
  Outcome<std::uint64_t> candidate_generation = in.u64();
  if (!candidate_generation.ok())
    return fail<EvaluationSnapshot>(candidate_generation.code(), candidate_generation.error().message);
  snapshot.candidate_generation = CandidateGeneration(candidate_generation.value());
  Outcome<CriterionRef> criterion = get_criterion(in);
  if (!criterion.ok()) return fail<EvaluationSnapshot>(criterion.code(), criterion.error().message);
  snapshot.criterion = criterion.value();
  Outcome<CriterionKind> kind = get_enum<CriterionKind>(in, 1u, 2u, "criterion kind");
  if (!kind.ok()) return fail<EvaluationSnapshot>(kind.code(), kind.error().message);
  snapshot.kind = kind.value();
  Outcome<VerificationState> verification = get_enum<VerificationState>(in, 1u, 4u, "verification state");
  if (!verification.ok())
    return fail<EvaluationSnapshot>(verification.code(), verification.error().message);
  snapshot.verification = verification.value();
  Outcome<CategoricalJudgment> judgment =
      get_enum<CategoricalJudgment>(in, 1u, 4u, "categorical judgment");
  if (!judgment.ok()) return fail<EvaluationSnapshot>(judgment.code(), judgment.error().message);
  snapshot.judgment = judgment.value();
  Outcome<bool> score_defined = in.boolean();
  if (!score_defined.ok())
    return fail<EvaluationSnapshot>(score_defined.code(), score_defined.error().message);
  snapshot.score_defined = score_defined.value();
  Outcome<double> score = in.f64();
  if (!score.ok()) return fail<EvaluationSnapshot>(score.code(), score.error().message);
  snapshot.score = score.value();
  Outcome<double> weight = in.f64();
  if (!weight.ok()) return fail<EvaluationSnapshot>(weight.code(), weight.error().message);
  snapshot.weight = weight.value();
  Outcome<std::vector<std::byte>> evidence = in.bytes(limits.max_evaluation_payload_bytes);
  if (!evidence.ok()) return fail<EvaluationSnapshot>(evidence.code(), evidence.error().message);
  snapshot.evidence = std::move(evidence).value();
  Outcome<std::string> rationale = get_string(in, limits.max_metadata_bytes, "evaluation rationale");
  if (!rationale.ok()) return fail<EvaluationSnapshot>(rationale.code(), rationale.error().message);
  snapshot.rationale = std::move(rationale).value();
  Outcome<std::uint64_t> sequence = in.u64();
  if (!sequence.ok()) return fail<EvaluationSnapshot>(sequence.code(), sequence.error().message);
  snapshot.received_sequence = Sequence(sequence.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<EvaluationSnapshot>(epoch.code(), epoch.error().message);
  snapshot.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> worker = in.u64();
  if (!worker.ok()) return fail<EvaluationSnapshot>(worker.code(), worker.error().message);
  snapshot.worker = WorkerId(worker.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<EvaluationSnapshot>(boot.code(), boot.error().message);
  snapshot.worker_boot = WorkerBootId(boot.value());
  Outcome<bool> current = in.boolean();
  if (!current.ok()) return fail<EvaluationSnapshot>(current.code(), current.error().message);
  snapshot.current = current.value();
  Outcome<bool> superseded = in.boolean();
  if (!superseded.ok()) return fail<EvaluationSnapshot>(superseded.code(), superseded.error().message);
  snapshot.superseded = superseded.value();
  return snapshot;
}

void put_result(Encoder& out, const EnsembleResult& result) {
  out.u64(result.id.value());
  out.u64(result.generation.value());
  out.u64(result.ensemble.value());
  out.u64(result.ensemble_generation.value());
  out.u64(result.execution.value());
  out.u64(result.attempt_generation.value());
  out.u64(result.coordinator_epoch.value());
  put_enum(out, result.decision);
  out.boolean(result.has_selected);
  out.u64(result.selected_candidate.value());
  out.u64(result.selected_candidate_generation.value());
  out.u64(result.selected_participant.value());
  out.u64(result.selected_participant_generation.value());
  put_enum(out, result.selected_role);
  out.bytes(result.payload);
  out.u64(result.payload_digest);
  out.u32(result.payload_bytes);
  put_quorum_report(out, result.quorum);
  put_consensus_report(out, result.consensus);
  put_arbitration_report(out, result.arbitration);
  out.u32(static_cast<std::uint32_t>(result.participants.size()));
  for (const ParticipantOutcome& outcome : result.participants) {
    out.u64(outcome.id.value());
    out.u64(outcome.generation.value());
    out.u32(outcome.roles.mask());
    out.boolean(outcome.required);
    put_enum(out, outcome.final_status);
    out.u32(outcome.candidates);
    out.u32(outcome.candidates_valid);
    out.u32(outcome.candidates_failed);
    out.u32(outcome.candidates_abstained);
    out.u32(outcome.evaluations);
    out.u32(outcome.retries);
    out.boolean(outcome.fallback);
    out.boolean(outcome.authoritative);
  }
  out.u32(static_cast<std::uint32_t>(result.candidates.size()));
  for (const CandidateSnapshot& snapshot : result.candidates) {
    put_candidate_snapshot(out, snapshot);
  }
  out.u32(static_cast<std::uint32_t>(result.evaluations.size()));
  for (const EvaluationSnapshot& snapshot : result.evaluations) {
    put_evaluation_snapshot(out, snapshot);
  }
  out.u32(static_cast<std::uint32_t>(result.explanation.size()));
  for (const ExplanationEntry& entry : result.explanation) {
    out.string(entry.subject);
    out.string(entry.detail);
  }
  out.boolean(result.fallback_used);
  out.u32(result.fallback_depth);
  out.u64(result.commit_sequence.value());
  out.u64(result.commit_fingerprint);
}

[[nodiscard]] Outcome<EnsembleResult> get_result(Decoder& in, const Limits& limits) {
  EnsembleResult result;
  Outcome<std::uint64_t> id = in.u64();
  if (!id.ok()) return fail<EnsembleResult>(id.code(), id.error().message);
  result.id = ResultId(id.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<EnsembleResult>(generation.code(), generation.error().message);
  result.generation = ResultGeneration(generation.value());
  Outcome<std::uint64_t> ensemble = in.u64();
  if (!ensemble.ok()) return fail<EnsembleResult>(ensemble.code(), ensemble.error().message);
  result.ensemble = EnsembleId(ensemble.value());
  Outcome<std::uint64_t> ensemble_generation = in.u64();
  if (!ensemble_generation.ok())
    return fail<EnsembleResult>(ensemble_generation.code(), ensemble_generation.error().message);
  result.ensemble_generation = EnsembleGeneration(ensemble_generation.value());
  Outcome<std::uint64_t> execution = in.u64();
  if (!execution.ok()) return fail<EnsembleResult>(execution.code(), execution.error().message);
  result.execution = EnsembleExecutionId(execution.value());
  Outcome<std::uint64_t> attempt = in.u64();
  if (!attempt.ok()) return fail<EnsembleResult>(attempt.code(), attempt.error().message);
  result.attempt_generation = EnsembleAttemptGeneration(attempt.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<EnsembleResult>(epoch.code(), epoch.error().message);
  result.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<EnsembleDecision> decision = get_enum<EnsembleDecision>(in, 0u, 7u, "ensemble decision");
  if (!decision.ok()) return fail<EnsembleResult>(decision.code(), decision.error().message);
  result.decision = decision.value();
  Outcome<bool> has_selected = in.boolean();
  if (!has_selected.ok()) return fail<EnsembleResult>(has_selected.code(), has_selected.error().message);
  result.has_selected = has_selected.value();
  Outcome<std::uint64_t> selected = in.u64();
  if (!selected.ok()) return fail<EnsembleResult>(selected.code(), selected.error().message);
  result.selected_candidate = CandidateId(selected.value());
  Outcome<std::uint64_t> selected_generation = in.u64();
  if (!selected_generation.ok())
    return fail<EnsembleResult>(selected_generation.code(), selected_generation.error().message);
  result.selected_candidate_generation = CandidateGeneration(selected_generation.value());
  Outcome<std::uint64_t> selected_participant = in.u64();
  if (!selected_participant.ok())
    return fail<EnsembleResult>(selected_participant.code(), selected_participant.error().message);
  result.selected_participant = ParticipantId(selected_participant.value());
  Outcome<std::uint64_t> selected_participant_generation = in.u64();
  if (!selected_participant_generation.ok())
    return fail<EnsembleResult>(selected_participant_generation.code(),
                                selected_participant_generation.error().message);
  result.selected_participant_generation =
      ParticipantGeneration(selected_participant_generation.value());
  Outcome<ParticipantRole> selected_role = get_enum<ParticipantRole>(in, 1u, 5u, "participant role");
  if (!selected_role.ok()) return fail<EnsembleResult>(selected_role.code(), selected_role.error().message);
  result.selected_role = selected_role.value();
  Outcome<std::vector<std::byte>> payload = in.bytes(limits.max_candidate_payload_bytes);
  if (!payload.ok()) return fail<EnsembleResult>(payload.code(), payload.error().message);
  result.payload = std::move(payload).value();
  Outcome<std::uint64_t> digest = in.u64();
  if (!digest.ok()) return fail<EnsembleResult>(digest.code(), digest.error().message);
  result.payload_digest = digest.value();
  Outcome<std::uint32_t> payload_bytes = in.u32();
  if (!payload_bytes.ok()) return fail<EnsembleResult>(payload_bytes.code(), payload_bytes.error().message);
  result.payload_bytes = payload_bytes.value();
  if (result.payload_bytes != result.payload.size()) {
    return fail<EnsembleResult>(EnsembleError::malformed_persistence,
                                "result payload length disagrees with its declared size");
  }
  Outcome<QuorumReport> quorum = get_quorum_report(in, limits);
  if (!quorum.ok()) return fail<EnsembleResult>(quorum.code(), quorum.error().message);
  result.quorum = std::move(quorum).value();
  Outcome<ConsensusReport> consensus = get_consensus_report(in, limits);
  if (!consensus.ok()) return fail<EnsembleResult>(consensus.code(), consensus.error().message);
  result.consensus = std::move(consensus).value();
  Outcome<ArbitrationReport> arbitration = get_arbitration_report(in, limits);
  if (!arbitration.ok()) return fail<EnsembleResult>(arbitration.code(), arbitration.error().message);
  result.arbitration = std::move(arbitration).value();
  Outcome<std::uint32_t> participants = in.u32();
  if (!participants.ok()) return fail<EnsembleResult>(participants.code(), participants.error().message);
  if (participants.value() > limits.max_participants_per_ensemble) {
    return fail<EnsembleResult>(EnsembleError::resource_limit_exceeded,
                                "declared participant outcome count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < participants.value(); ++index) {
    ParticipantOutcome outcome;
    Outcome<std::uint64_t> participant = in.u64();
    if (!participant.ok()) return fail<EnsembleResult>(participant.code(), participant.error().message);
    outcome.id = ParticipantId(participant.value());
    Outcome<std::uint64_t> participant_generation = in.u64();
    if (!participant_generation.ok())
      return fail<EnsembleResult>(participant_generation.code(), participant_generation.error().message);
    outcome.generation = ParticipantGeneration(participant_generation.value());
    Outcome<std::uint32_t> roles = in.u32();
    if (!roles.ok()) return fail<EnsembleResult>(roles.code(), roles.error().message);
    if ((roles.value() & ~0x1Fu) != 0u) {
      return fail<EnsembleResult>(EnsembleError::invalid_enum_value, "unknown role bit in outcome");
    }
    outcome.roles = RoleSet(roles.value());
    Outcome<bool> required = in.boolean();
    if (!required.ok()) return fail<EnsembleResult>(required.code(), required.error().message);
    outcome.required = required.value();
    Outcome<ParticipantStatus> status = get_enum<ParticipantStatus>(in, 1u, 6u, "participant status");
    if (!status.ok()) return fail<EnsembleResult>(status.code(), status.error().message);
    outcome.final_status = status.value();
    std::uint32_t* counters[] = {&outcome.candidates,       &outcome.candidates_valid,
                                 &outcome.candidates_failed, &outcome.candidates_abstained,
                                 &outcome.evaluations,       &outcome.retries};
    for (std::uint32_t* target : counters) {
      Outcome<std::uint32_t> value = in.u32();
      if (!value.ok()) return fail<EnsembleResult>(value.code(), value.error().message);
      *target = value.value();
    }
    Outcome<bool> fallback = in.boolean();
    if (!fallback.ok()) return fail<EnsembleResult>(fallback.code(), fallback.error().message);
    outcome.fallback = fallback.value();
    Outcome<bool> authoritative = in.boolean();
    if (!authoritative.ok())
      return fail<EnsembleResult>(authoritative.code(), authoritative.error().message);
    outcome.authoritative = authoritative.value();
    result.participants.push_back(std::move(outcome));
  }
  Outcome<std::uint32_t> candidates = in.u32();
  if (!candidates.ok()) return fail<EnsembleResult>(candidates.code(), candidates.error().message);
  if (candidates.value() > limits.max_records_per_snapshot) {
    return fail<EnsembleResult>(EnsembleError::resource_limit_exceeded,
                                "declared candidate snapshot count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < candidates.value(); ++index) {
    Outcome<CandidateSnapshot> snapshot = get_candidate_snapshot(in, limits);
    if (!snapshot.ok()) return fail<EnsembleResult>(snapshot.code(), snapshot.error().message);
    result.candidates.push_back(std::move(snapshot).value());
  }
  Outcome<std::uint32_t> evaluations = in.u32();
  if (!evaluations.ok()) return fail<EnsembleResult>(evaluations.code(), evaluations.error().message);
  if (evaluations.value() > limits.max_records_per_snapshot) {
    return fail<EnsembleResult>(EnsembleError::resource_limit_exceeded,
                                "declared evaluation snapshot count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < evaluations.value(); ++index) {
    Outcome<EvaluationSnapshot> snapshot = get_evaluation_snapshot(in, limits);
    if (!snapshot.ok()) return fail<EnsembleResult>(snapshot.code(), snapshot.error().message);
    result.evaluations.push_back(std::move(snapshot).value());
  }
  Outcome<std::uint32_t> explanations = in.u32();
  if (!explanations.ok()) return fail<EnsembleResult>(explanations.code(), explanations.error().message);
  if (explanations.value() > limits.max_explanation_entries) {
    return fail<EnsembleResult>(EnsembleError::resource_limit_exceeded,
                                "declared explanation count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < explanations.value(); ++index) {
    Outcome<std::string> subject = get_string(in, limits.max_label_bytes, "explanation subject");
    if (!subject.ok()) return fail<EnsembleResult>(subject.code(), subject.error().message);
    Outcome<std::string> detail = get_string(in, limits.max_metadata_bytes, "explanation detail");
    if (!detail.ok()) return fail<EnsembleResult>(detail.code(), detail.error().message);
    ExplanationEntry entry;
    entry.subject = std::move(subject).value();
    entry.detail = std::move(detail).value();
    result.explanation.push_back(std::move(entry));
  }
  Outcome<bool> fallback_used = in.boolean();
  if (!fallback_used.ok()) return fail<EnsembleResult>(fallback_used.code(), fallback_used.error().message);
  result.fallback_used = fallback_used.value();
  Outcome<std::uint32_t> fallback_depth = in.u32();
  if (!fallback_depth.ok())
    return fail<EnsembleResult>(fallback_depth.code(), fallback_depth.error().message);
  result.fallback_depth = fallback_depth.value();
  Outcome<std::uint64_t> commit_sequence = in.u64();
  if (!commit_sequence.ok())
    return fail<EnsembleResult>(commit_sequence.code(), commit_sequence.error().message);
  result.commit_sequence = Sequence(commit_sequence.value());
  Outcome<std::uint64_t> fingerprint = in.u64();
  if (!fingerprint.ok()) return fail<EnsembleResult>(fingerprint.code(), fingerprint.error().message);
  result.commit_fingerprint = fingerprint.value();
  return result;
}

}  // namespace

std::vector<std::byte> encode_result_payload(const EnsembleResult& result, const Limits& limits) {
  Encoder out(limits, 4096);
  out.u32(0u);
  put_result(out, result);
  return out.take();
}

Outcome<EnsembleResult> decode_result_payload(std::span<const std::byte> data,
                                              const Limits& limits) {
  Decoder in(data, limits);
  Outcome<std::uint32_t> reserved = in.u32();
  if (!reserved.ok()) return fail<EnsembleResult>(reserved.code(), reserved.error().message);
  if (reserved.value() != 0u) {
    return fail<EnsembleResult>(EnsembleError::unsupported_version,
                                "result encoding uses an unsupported extension word");
  }
  Outcome<EnsembleResult> result = get_result(in, limits);
  if (!result.ok()) return result;
  const Status exhausted = in.require_exhausted("ensemble result");
  if (!exhausted.ok()) {
    return fail<EnsembleResult>(exhausted.code(), exhausted.error().message);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Durable record serialization
// ---------------------------------------------------------------------------
namespace {

void put_participant_record(Encoder& out, const ParticipantRecord& participant) {
  out.u64(participant.id.value());
  out.u64(participant.generation.value());
  put_participant_spec(out, participant.declaration);
  put_enum(out, participant.status);
  out.u64(participant.worker.value());
  out.u64(participant.worker_boot.value());
  out.u64(participant.coordinator_epoch.value());
  put_compatibility_profile(out, participant.profile);
  out.boolean(participant.registered);
  out.string(participant.failure_detail);
  out.u32(participant.replacement_count);
  out.u32(participant.candidates_completed);
  out.u32(participant.candidates_failed);
  out.u32(participant.candidates_abstained);
  out.u32(participant.evaluations_submitted);
  out.u32(participant.retries_consumed);
  out.boolean(participant.fallback_activated);
}

[[nodiscard]] Outcome<ParticipantRecord> get_participant_record(Decoder& in, const Limits& limits) {
  ParticipantRecord participant;
  Outcome<std::uint64_t> id = in.u64();
  if (!id.ok()) return fail<ParticipantRecord>(id.code(), id.error().message);
  participant.id = ParticipantId(id.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<ParticipantRecord>(generation.code(), generation.error().message);
  participant.generation = ParticipantGeneration(generation.value());
  Outcome<ParticipantSpec> declaration = get_participant_spec(in, limits);
  if (!declaration.ok()) return fail<ParticipantRecord>(declaration.code(), declaration.error().message);
  participant.declaration = std::move(declaration).value();
  Outcome<ParticipantStatus> status = get_enum<ParticipantStatus>(in, 1u, 6u, "participant status");
  if (!status.ok()) return fail<ParticipantRecord>(status.code(), status.error().message);
  participant.status = status.value();
  Outcome<std::uint64_t> worker = in.u64();
  if (!worker.ok()) return fail<ParticipantRecord>(worker.code(), worker.error().message);
  participant.worker = WorkerId(worker.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<ParticipantRecord>(boot.code(), boot.error().message);
  participant.worker_boot = WorkerBootId(boot.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<ParticipantRecord>(epoch.code(), epoch.error().message);
  participant.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<CompatibilityProfile> profile = get_compatibility_profile(in);
  if (!profile.ok()) return fail<ParticipantRecord>(profile.code(), profile.error().message);
  participant.profile = profile.value();
  Outcome<bool> registered = in.boolean();
  if (!registered.ok()) return fail<ParticipantRecord>(registered.code(), registered.error().message);
  participant.registered = registered.value();
  Outcome<std::string> detail = get_string(in, limits.max_metadata_bytes, "participant failure detail");
  if (!detail.ok()) return fail<ParticipantRecord>(detail.code(), detail.error().message);
  participant.failure_detail = std::move(detail).value();
  std::uint32_t* counters[] = {&participant.replacement_count, &participant.candidates_completed,
                               &participant.candidates_failed, &participant.candidates_abstained,
                               &participant.evaluations_submitted, &participant.retries_consumed};
  for (std::uint32_t* target : counters) {
    Outcome<std::uint32_t> value = in.u32();
    if (!value.ok()) return fail<ParticipantRecord>(value.code(), value.error().message);
    *target = value.value();
  }
  Outcome<bool> fallback = in.boolean();
  if (!fallback.ok()) return fail<ParticipantRecord>(fallback.code(), fallback.error().message);
  participant.fallback_activated = fallback.value();
  return participant;
}

void put_candidate_record(Encoder& out, const CandidateRecord& candidate) {
  out.u64(candidate.id.value());
  out.u64(candidate.generation.value());
  out.u64(candidate.participant.value());
  out.u64(candidate.participant_generation.value());
  put_enum(out, candidate.role);
  out.u64(candidate.stage.value());
  put_enum(out, candidate.state);
  out.u32(candidate.attempt);
  out.u32(candidate.attempts_used);
  out.string(candidate.domain);
  out.bytes(candidate.payload);
  out.u64(candidate.payload_digest);
  out.u64(candidate.received_sequence.value());
  out.u64(candidate.dispatch_sequence.value());
  out.u64(candidate.worker.value());
  out.u64(candidate.worker_boot.value());
  out.u64(candidate.coordinator_epoch.value());
  out.u16(static_cast<std::uint16_t>(candidate.failure));
  out.string(candidate.failure_detail);
  out.u32(candidate.evaluation_count);
  out.u32(candidate.hard_failures);
  out.u32(candidate.hard_unknowns);
  out.u32(candidate.hard_predicates);
  out.u32(candidate.budget_units_used);
  out.u32(candidate.latency);
  out.boolean(candidate.fallback);
  out.boolean(candidate.current);
  out.string(candidate.elimination_reason);
}

[[nodiscard]] Outcome<CandidateRecord> get_candidate_record(Decoder& in, const Limits& limits) {
  CandidateRecord candidate;
  Outcome<std::uint64_t> id = in.u64();
  if (!id.ok()) return fail<CandidateRecord>(id.code(), id.error().message);
  candidate.id = CandidateId(id.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<CandidateRecord>(generation.code(), generation.error().message);
  candidate.generation = CandidateGeneration(generation.value());
  Outcome<std::uint64_t> participant = in.u64();
  if (!participant.ok()) return fail<CandidateRecord>(participant.code(), participant.error().message);
  candidate.participant = ParticipantId(participant.value());
  Outcome<std::uint64_t> participant_generation = in.u64();
  if (!participant_generation.ok())
    return fail<CandidateRecord>(participant_generation.code(), participant_generation.error().message);
  candidate.participant_generation = ParticipantGeneration(participant_generation.value());
  Outcome<ParticipantRole> role = get_enum<ParticipantRole>(in, 1u, 5u, "participant role");
  if (!role.ok()) return fail<CandidateRecord>(role.code(), role.error().message);
  candidate.role = role.value();
  Outcome<std::uint64_t> stage = in.u64();
  if (!stage.ok()) return fail<CandidateRecord>(stage.code(), stage.error().message);
  candidate.stage = StageId(stage.value());
  Outcome<CandidateState> state = get_enum<CandidateState>(in, 1u, 13u, "candidate state");
  if (!state.ok()) return fail<CandidateRecord>(state.code(), state.error().message);
  candidate.state = state.value();
  Outcome<std::uint32_t> attempt = in.u32();
  if (!attempt.ok()) return fail<CandidateRecord>(attempt.code(), attempt.error().message);
  candidate.attempt = attempt.value();
  Outcome<std::uint32_t> attempts_used = in.u32();
  if (!attempts_used.ok()) return fail<CandidateRecord>(attempts_used.code(), attempts_used.error().message);
  candidate.attempts_used = attempts_used.value();
  Outcome<std::string> domain = get_string(in, limits.max_metadata_bytes, "candidate domain");
  if (!domain.ok()) return fail<CandidateRecord>(domain.code(), domain.error().message);
  candidate.domain = std::move(domain).value();
  Outcome<std::vector<std::byte>> payload = in.bytes(limits.max_candidate_payload_bytes);
  if (!payload.ok()) return fail<CandidateRecord>(payload.code(), payload.error().message);
  candidate.payload = std::move(payload).value();
  Outcome<std::uint64_t> digest = in.u64();
  if (!digest.ok()) return fail<CandidateRecord>(digest.code(), digest.error().message);
  candidate.payload_digest = digest.value();
  Outcome<std::uint64_t> sequence = in.u64();
  if (!sequence.ok()) return fail<CandidateRecord>(sequence.code(), sequence.error().message);
  candidate.received_sequence = Sequence(sequence.value());
  Outcome<std::uint64_t> dispatch_sequence = in.u64();
  if (!dispatch_sequence.ok())
    return fail<CandidateRecord>(dispatch_sequence.code(), dispatch_sequence.error().message);
  candidate.dispatch_sequence = Sequence(dispatch_sequence.value());
  Outcome<std::uint64_t> worker = in.u64();
  if (!worker.ok()) return fail<CandidateRecord>(worker.code(), worker.error().message);
  candidate.worker = WorkerId(worker.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<CandidateRecord>(boot.code(), boot.error().message);
  candidate.worker_boot = WorkerBootId(boot.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<CandidateRecord>(epoch.code(), epoch.error().message);
  candidate.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint16_t> failure = in.u16();
  if (!failure.ok()) return fail<CandidateRecord>(failure.code(), failure.error().message);
  if (failure.value() > static_cast<std::uint16_t>(EnsembleError::internal_error)) {
    return fail<CandidateRecord>(EnsembleError::invalid_enum_value, "invalid candidate failure code");
  }
  candidate.failure = static_cast<EnsembleError>(failure.value());
  Outcome<std::string> detail = get_string(in, limits.max_metadata_bytes, "candidate failure detail");
  if (!detail.ok()) return fail<CandidateRecord>(detail.code(), detail.error().message);
  candidate.failure_detail = std::move(detail).value();
  std::uint32_t* counters[] = {&candidate.evaluation_count, &candidate.hard_failures,
                               &candidate.hard_unknowns, &candidate.hard_predicates,
                               &candidate.budget_units_used, &candidate.latency};
  for (std::uint32_t* target : counters) {
    Outcome<std::uint32_t> value = in.u32();
    if (!value.ok()) return fail<CandidateRecord>(value.code(), value.error().message);
    *target = value.value();
  }
  Outcome<bool> fallback = in.boolean();
  if (!fallback.ok()) return fail<CandidateRecord>(fallback.code(), fallback.error().message);
  candidate.fallback = fallback.value();
  Outcome<bool> current = in.boolean();
  if (!current.ok()) return fail<CandidateRecord>(current.code(), current.error().message);
  candidate.current = current.value();
  Outcome<std::string> elimination = get_string(in, limits.max_metadata_bytes, "elimination reason");
  if (!elimination.ok()) return fail<CandidateRecord>(elimination.code(), elimination.error().message);
  candidate.elimination_reason = std::move(elimination).value();
  return candidate;
}

void put_evaluation_record(Encoder& out, const EvaluationRecord& evaluation) {
  out.u64(evaluation.id.value());
  out.u64(evaluation.generation.value());
  out.u64(evaluation.evaluator.value());
  out.u64(evaluation.evaluator_generation.value());
  put_enum(out, evaluation.role);
  out.u64(evaluation.candidate.value());
  out.u64(evaluation.candidate_generation.value());
  put_criterion(out, evaluation.criterion);
  put_enum(out, evaluation.kind);
  put_enum(out, evaluation.verification);
  put_enum(out, evaluation.judgment);
  out.boolean(evaluation.score_defined);
  out.f64(evaluation.score);
  out.f64(evaluation.weight);
  out.bytes(evaluation.evidence);
  out.string(evaluation.rationale);
  out.u64(evaluation.received_sequence.value());
  out.u64(evaluation.coordinator_epoch.value());
  out.u64(evaluation.worker.value());
  out.u64(evaluation.worker_boot.value());
  out.u64(evaluation.fingerprint);
  out.boolean(evaluation.current);
}

[[nodiscard]] Outcome<EvaluationRecord> get_evaluation_record(Decoder& in, const Limits& limits) {
  EvaluationRecord evaluation;
  Outcome<std::uint64_t> id = in.u64();
  if (!id.ok()) return fail<EvaluationRecord>(id.code(), id.error().message);
  evaluation.id = EvaluationId(id.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<EvaluationRecord>(generation.code(), generation.error().message);
  evaluation.generation = EvaluationGeneration(generation.value());
  Outcome<std::uint64_t> evaluator = in.u64();
  if (!evaluator.ok()) return fail<EvaluationRecord>(evaluator.code(), evaluator.error().message);
  evaluation.evaluator = ParticipantId(evaluator.value());
  Outcome<std::uint64_t> evaluator_generation = in.u64();
  if (!evaluator_generation.ok())
    return fail<EvaluationRecord>(evaluator_generation.code(), evaluator_generation.error().message);
  evaluation.evaluator_generation = ParticipantGeneration(evaluator_generation.value());
  Outcome<ParticipantRole> role = get_enum<ParticipantRole>(in, 1u, 5u, "participant role");
  if (!role.ok()) return fail<EvaluationRecord>(role.code(), role.error().message);
  evaluation.role = role.value();
  Outcome<std::uint64_t> candidate = in.u64();
  if (!candidate.ok()) return fail<EvaluationRecord>(candidate.code(), candidate.error().message);
  evaluation.candidate = CandidateId(candidate.value());
  Outcome<std::uint64_t> candidate_generation = in.u64();
  if (!candidate_generation.ok())
    return fail<EvaluationRecord>(candidate_generation.code(), candidate_generation.error().message);
  evaluation.candidate_generation = CandidateGeneration(candidate_generation.value());
  Outcome<CriterionRef> criterion = get_criterion(in);
  if (!criterion.ok()) return fail<EvaluationRecord>(criterion.code(), criterion.error().message);
  evaluation.criterion = criterion.value();
  Outcome<CriterionKind> kind = get_enum<CriterionKind>(in, 1u, 2u, "criterion kind");
  if (!kind.ok()) return fail<EvaluationRecord>(kind.code(), kind.error().message);
  evaluation.kind = kind.value();
  Outcome<VerificationState> verification = get_enum<VerificationState>(in, 1u, 4u, "verification state");
  if (!verification.ok()) return fail<EvaluationRecord>(verification.code(), verification.error().message);
  evaluation.verification = verification.value();
  Outcome<CategoricalJudgment> judgment =
      get_enum<CategoricalJudgment>(in, 1u, 4u, "categorical judgment");
  if (!judgment.ok()) return fail<EvaluationRecord>(judgment.code(), judgment.error().message);
  evaluation.judgment = judgment.value();
  Outcome<bool> score_defined = in.boolean();
  if (!score_defined.ok())
    return fail<EvaluationRecord>(score_defined.code(), score_defined.error().message);
  evaluation.score_defined = score_defined.value();
  Outcome<double> score = in.f64();
  if (!score.ok()) return fail<EvaluationRecord>(score.code(), score.error().message);
  evaluation.score = score.value();
  Outcome<double> weight = in.f64();
  if (!weight.ok()) return fail<EvaluationRecord>(weight.code(), weight.error().message);
  evaluation.weight = weight.value();
  Outcome<std::vector<std::byte>> evidence = in.bytes(limits.max_evaluation_payload_bytes);
  if (!evidence.ok()) return fail<EvaluationRecord>(evidence.code(), evidence.error().message);
  evaluation.evidence = std::move(evidence).value();
  Outcome<std::string> rationale = get_string(in, limits.max_metadata_bytes, "evaluation rationale");
  if (!rationale.ok()) return fail<EvaluationRecord>(rationale.code(), rationale.error().message);
  evaluation.rationale = std::move(rationale).value();
  Outcome<std::uint64_t> sequence = in.u64();
  if (!sequence.ok()) return fail<EvaluationRecord>(sequence.code(), sequence.error().message);
  evaluation.received_sequence = Sequence(sequence.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<EvaluationRecord>(epoch.code(), epoch.error().message);
  evaluation.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> worker = in.u64();
  if (!worker.ok()) return fail<EvaluationRecord>(worker.code(), worker.error().message);
  evaluation.worker = WorkerId(worker.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<EvaluationRecord>(boot.code(), boot.error().message);
  evaluation.worker_boot = WorkerBootId(boot.value());
  Outcome<std::uint64_t> fingerprint = in.u64();
  if (!fingerprint.ok()) return fail<EvaluationRecord>(fingerprint.code(), fingerprint.error().message);
  evaluation.fingerprint = fingerprint.value();
  Outcome<bool> current = in.boolean();
  if (!current.ok()) return fail<EvaluationRecord>(current.code(), current.error().message);
  evaluation.current = current.value();
  return evaluation;
}

}  // namespace

// Durable runtime state
// ---------------------------------------------------------------------------
namespace {

void put_commit_record(Encoder& out, const CommitRecord& record) {
  out.u64(record.result.value());
  out.u64(record.generation.value());
  out.u64(record.sequence.value());
  out.u64(record.coordinator_epoch.value());
  out.u64(record.execution.value());
  out.u64(record.attempt_generation.value());
  put_enum(out, record.decision);
  out.u64(record.commit_fingerprint);
}

[[nodiscard]] Outcome<CommitRecord> get_commit_record(Decoder& in) {
  CommitRecord record;
  Outcome<std::uint64_t> result = in.u64();
  if (!result.ok()) return fail<CommitRecord>(result.code(), result.error().message);
  record.result = ResultId(result.value());
  Outcome<std::uint64_t> generation = in.u64();
  if (!generation.ok()) return fail<CommitRecord>(generation.code(), generation.error().message);
  record.generation = ResultGeneration(generation.value());
  Outcome<std::uint64_t> sequence = in.u64();
  if (!sequence.ok()) return fail<CommitRecord>(sequence.code(), sequence.error().message);
  record.sequence = Sequence(sequence.value());
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<CommitRecord>(epoch.code(), epoch.error().message);
  record.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> execution = in.u64();
  if (!execution.ok()) return fail<CommitRecord>(execution.code(), execution.error().message);
  record.execution = EnsembleExecutionId(execution.value());
  Outcome<std::uint64_t> attempt = in.u64();
  if (!attempt.ok()) return fail<CommitRecord>(attempt.code(), attempt.error().message);
  record.attempt_generation = EnsembleAttemptGeneration(attempt.value());
  Outcome<EnsembleDecision> decision = get_enum<EnsembleDecision>(in, 0u, 7u, "ensemble decision");
  if (!decision.ok()) return fail<CommitRecord>(decision.code(), decision.error().message);
  record.decision = decision.value();
  Outcome<std::uint64_t> fingerprint = in.u64();
  if (!fingerprint.ok()) return fail<CommitRecord>(fingerprint.code(), fingerprint.error().message);
  record.commit_fingerprint = fingerprint.value();
  return record;
}

void put_execution_record(Encoder& out, const ExecutionRecord& execution, const Limits& limits) {
  out.u64(execution.id.value());
  out.u64(execution.attempt_generation.value());
  put_enum(out, execution.status);
  out.u64(execution.coordinator_epoch.value());
  out.u64(execution.report_generation);
  out.u64(execution.stage_index);
  out.boolean(execution.declared_current_stage);
  out.boolean(execution.planning_complete);
  out.boolean(execution.fallback_activated);
  out.u32(execution.fallback_depth);
  out.boolean(execution.cancellation_requested);
  out.string(execution.cancellation_reason);
  out.boolean(execution.cancellation_emitted);
  out.u64(execution.cancelled_sequence.value());
  out.u64(execution.committed_sequence.value());
  out.boolean(execution.has_result);
  if (execution.has_result) {
    put_result(out, execution.result);
  }
  out.u32(static_cast<std::uint32_t>(execution.candidates.size()));
  for (const CandidateRecord& candidate : execution.candidates) {
    put_candidate_record(out, candidate);
  }
  out.u32(static_cast<std::uint32_t>(execution.evaluations.size()));
  for (const EvaluationRecord& evaluation : execution.evaluations) {
    put_evaluation_record(out, evaluation);
  }
  out.u32(static_cast<std::uint32_t>(execution.extra_stages.size()));
  for (const StageSpec& stage : execution.extra_stages) {
    put_stage_spec(out, stage);
  }
  out.u32(static_cast<std::uint32_t>(execution.commits.size()));
  for (const CommitRecord& record : execution.commits) {
    put_commit_record(out, record);
  }
  (void)limits;
}

[[nodiscard]] Outcome<ExecutionRecord> get_execution_record(Decoder& in, const Limits& limits) {
  ExecutionRecord execution;
  Outcome<std::uint64_t> id = in.u64();
  if (!id.ok()) return fail<ExecutionRecord>(id.code(), id.error().message);
  execution.id = EnsembleExecutionId(id.value());
  Outcome<std::uint64_t> attempt = in.u64();
  if (!attempt.ok()) return fail<ExecutionRecord>(attempt.code(), attempt.error().message);
  execution.attempt_generation = EnsembleAttemptGeneration(attempt.value());
  Outcome<ExecutionStatus> status = get_enum<ExecutionStatus>(in, 0u, 7u, "execution status");
  if (!status.ok()) return fail<ExecutionRecord>(status.code(), status.error().message);
  execution.status = status.value();
  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<ExecutionRecord>(epoch.code(), epoch.error().message);
  execution.coordinator_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> reports = in.u64();
  if (!reports.ok()) return fail<ExecutionRecord>(reports.code(), reports.error().message);
  execution.report_generation = reports.value();
  Outcome<std::uint64_t> stage_index = in.u64();
  if (!stage_index.ok()) return fail<ExecutionRecord>(stage_index.code(), stage_index.error().message);
  if (stage_index.value() > limits.max_stages_per_ensemble) {
    return fail<ExecutionRecord>(EnsembleError::resource_limit_exceeded,
                                 "declared stage index exceeds the configured bound");
  }
  execution.stage_index = static_cast<std::size_t>(stage_index.value());
  Outcome<bool> declared = in.boolean();
  if (!declared.ok()) return fail<ExecutionRecord>(declared.code(), declared.error().message);
  execution.declared_current_stage = declared.value();
  Outcome<bool> planning = in.boolean();
  if (!planning.ok()) return fail<ExecutionRecord>(planning.code(), planning.error().message);
  execution.planning_complete = planning.value();
  Outcome<bool> fallback = in.boolean();
  if (!fallback.ok()) return fail<ExecutionRecord>(fallback.code(), fallback.error().message);
  execution.fallback_activated = fallback.value();
  Outcome<std::uint32_t> depth = in.u32();
  if (!depth.ok()) return fail<ExecutionRecord>(depth.code(), depth.error().message);
  execution.fallback_depth = depth.value();
  Outcome<bool> cancellation = in.boolean();
  if (!cancellation.ok()) return fail<ExecutionRecord>(cancellation.code(), cancellation.error().message);
  execution.cancellation_requested = cancellation.value();
  Outcome<std::string> reason = get_string(in, limits.max_metadata_bytes, "cancellation reason");
  if (!reason.ok()) return fail<ExecutionRecord>(reason.code(), reason.error().message);
  execution.cancellation_reason = std::move(reason).value();
  Outcome<bool> emitted = in.boolean();
  if (!emitted.ok()) return fail<ExecutionRecord>(emitted.code(), emitted.error().message);
  execution.cancellation_emitted = emitted.value();
  Outcome<std::uint64_t> cancelled = in.u64();
  if (!cancelled.ok()) return fail<ExecutionRecord>(cancelled.code(), cancelled.error().message);
  execution.cancelled_sequence = Sequence(cancelled.value());
  Outcome<std::uint64_t> committed = in.u64();
  if (!committed.ok()) return fail<ExecutionRecord>(committed.code(), committed.error().message);
  execution.committed_sequence = Sequence(committed.value());
  Outcome<bool> has_result = in.boolean();
  if (!has_result.ok()) return fail<ExecutionRecord>(has_result.code(), has_result.error().message);
  execution.has_result = has_result.value();
  if (execution.has_result) {
    Outcome<EnsembleResult> result = get_result(in, limits);
    if (!result.ok()) return fail<ExecutionRecord>(result.code(), result.error().message);
    execution.result = std::move(result).value();
  }
  Outcome<std::uint32_t> candidates = in.u32();
  if (!candidates.ok()) return fail<ExecutionRecord>(candidates.code(), candidates.error().message);
  if (candidates.value() > limits.max_candidates_per_execution) {
    return fail<ExecutionRecord>(EnsembleError::resource_limit_exceeded,
                                 "declared candidate count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < candidates.value(); ++index) {
    Outcome<CandidateRecord> candidate = get_candidate_record(in, limits);
    if (!candidate.ok()) return fail<ExecutionRecord>(candidate.code(), candidate.error().message);
    execution.candidates.push_back(std::move(candidate).value());
  }
  Outcome<std::uint32_t> evaluations = in.u32();
  if (!evaluations.ok()) return fail<ExecutionRecord>(evaluations.code(), evaluations.error().message);
  if (evaluations.value() > limits.max_evaluations_per_execution) {
    return fail<ExecutionRecord>(EnsembleError::resource_limit_exceeded,
                                 "declared evaluation count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < evaluations.value(); ++index) {
    Outcome<EvaluationRecord> evaluation = get_evaluation_record(in, limits);
    if (!evaluation.ok()) return fail<ExecutionRecord>(evaluation.code(), evaluation.error().message);
    execution.evaluations.push_back(std::move(evaluation).value());
  }
  Outcome<std::uint32_t> stages = in.u32();
  if (!stages.ok()) return fail<ExecutionRecord>(stages.code(), stages.error().message);
  if (stages.value() > limits.max_stages_per_ensemble) {
    return fail<ExecutionRecord>(EnsembleError::resource_limit_exceeded,
                                 "declared fallback stage count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < stages.value(); ++index) {
    Outcome<StageSpec> stage = get_stage_spec(in, limits);
    if (!stage.ok()) return fail<ExecutionRecord>(stage.code(), stage.error().message);
    execution.extra_stages.push_back(std::move(stage).value());
  }
  Outcome<std::uint32_t> commits = in.u32();
  if (!commits.ok()) return fail<ExecutionRecord>(commits.code(), commits.error().message);
  if (commits.value() > limits.max_retained_commits) {
    return fail<ExecutionRecord>(EnsembleError::resource_limit_exceeded,
                                 "declared commit count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < commits.value(); ++index) {
    Outcome<CommitRecord> record = get_commit_record(in);
    if (!record.ok()) return fail<ExecutionRecord>(record.code(), record.error().message);
    execution.commits.push_back(std::move(record).value());
  }
  return execution;
}

}  // namespace

Outcome<std::vector<std::byte>> EnsembleFabric::serialize_state() const {
  std::shared_lock lock(impl_->mutex);
  const Limits& limits = impl_->config.limits;
  Encoder out(limits, 8192);
  out.u32(0u);
  out.u64(impl_->epoch.value());
  out.u64(impl_->coordinator_boot.value());
  out.u64(impl_->ensemble_ids.next_value());
  out.u64(impl_->candidate_ids.next_value());
  out.u64(impl_->evaluation_ids.next_value());
  out.u64(impl_->result_ids.next_value());
  out.u64(impl_->sequences.next_value());
  const Counters& counters = impl_->counters;
  out.u64(counters.commits_accepted);
  out.u64(counters.commits_rejected_conflicting);
  out.u64(counters.commits_rejected_stale);
  out.u64(counters.stale_submissions_rejected);
  out.u64(counters.duplicate_submissions_suppressed);
  out.u64(counters.duplicate_votes_suppressed);
  out.u64(counters.fallback_activations);
  out.u64(counters.retries_issued);
  out.u64(counters.cancellations);
  out.u64(counters.supersessions);
  out.u64(counters.revalidations_required);
  out.u32(static_cast<std::uint32_t>(impl_->ensembles.size()));
  for (const auto& entry : impl_->ensembles) {
    const EnsembleRecord& ensemble = entry.second;
    const std::vector<std::byte> spec_bytes = encode_spec(ensemble.spec, limits);
    out.bytes(spec_bytes);
    out.boolean(ensemble.superseded);
    out.u32(static_cast<std::uint32_t>(ensemble.participants.size()));
    for (const ParticipantRecord& participant : ensemble.participants) {
      put_participant_record(out, participant);
    }
    out.boolean(ensemble.has_execution);
    if (ensemble.has_execution) {
      put_execution_record(out, ensemble.execution, limits);
    }
    out.u32(static_cast<std::uint32_t>(ensemble.history.size()));
    for (const EnsembleResult& result : ensemble.history) {
      put_result(out, result);
    }
  }
  return out.take();
}

Status EnsembleFabric::save_state(const std::string& path) const {
  Outcome<std::vector<std::byte>> payload = serialize_state();
  if (!payload.ok()) {
    return fail(payload.code(), payload.error().message);
  }
  return write_state_file(std::filesystem::path(path), payload.value(), 1u, impl_->config.limits);
}

Outcome<CoordinatorEpoch> EnsembleFabric::recover_state(const std::string& path) {
  PersistenceHeader header;
  Outcome<std::vector<std::byte>> payload =
      read_state_file(std::filesystem::path(path), header, impl_->config.limits);
  if (!payload.ok()) {
    return fail<CoordinatorEpoch>(payload.code(), payload.error().message);
  }
  const Limits& limits = impl_->config.limits;
  Decoder in(std::span<const std::byte>(payload.value().data(), payload.value().size()), limits);
  Outcome<std::uint32_t> reserved = in.u32();
  if (!reserved.ok()) return fail<CoordinatorEpoch>(reserved.code(), reserved.error().message);
  if (reserved.value() != 0u) {
    return fail<CoordinatorEpoch>(EnsembleError::unsupported_version,
                                  "state uses an unsupported extension word");
  }

  CoordinatorEpoch stored_epoch;
  WorkerBootId stored_boot;
  std::uint64_t waterfalls[5] = {};
  Counters counters;
  std::map<std::uint64_t, EnsembleRecord> loaded;

  Outcome<std::uint64_t> epoch = in.u64();
  if (!epoch.ok()) return fail<CoordinatorEpoch>(epoch.code(), epoch.error().message);
  stored_epoch = CoordinatorEpoch(epoch.value());
  Outcome<std::uint64_t> boot = in.u64();
  if (!boot.ok()) return fail<CoordinatorEpoch>(boot.code(), boot.error().message);
  stored_boot = WorkerBootId(boot.value());
  for (std::uint64_t& value : waterfalls) {
    Outcome<std::uint64_t> raw = in.u64();
    if (!raw.ok()) return fail<CoordinatorEpoch>(raw.code(), raw.error().message);
    value = raw.value();
  }
  std::uint64_t* counter_targets[] = {&counters.commits_accepted,
                                      &counters.commits_rejected_conflicting,
                                      &counters.commits_rejected_stale,
                                      &counters.stale_submissions_rejected,
                                      &counters.duplicate_submissions_suppressed,
                                      &counters.duplicate_votes_suppressed,
                                      &counters.fallback_activations,
                                      &counters.retries_issued,
                                      &counters.cancellations,
                                      &counters.supersessions,
                                      &counters.revalidations_required};
  for (std::uint64_t* target : counter_targets) {
    Outcome<std::uint64_t> raw = in.u64();
    if (!raw.ok()) return fail<CoordinatorEpoch>(raw.code(), raw.error().message);
    *target = raw.value();
  }
  Outcome<std::uint32_t> ensembles = in.u32();
  if (!ensembles.ok()) return fail<CoordinatorEpoch>(ensembles.code(), ensembles.error().message);
  if (ensembles.value() > limits.max_records_per_snapshot) {
    return fail<CoordinatorEpoch>(EnsembleError::resource_limit_exceeded,
                                  "declared ensemble count exceeds the configured bound");
  }
  for (std::uint32_t index = 0; index < ensembles.value(); ++index) {
    Outcome<std::vector<std::byte>> spec_bytes = in.bytes(limits.max_persistence_bytes);
    if (!spec_bytes.ok()) return fail<CoordinatorEpoch>(spec_bytes.code(), spec_bytes.error().message);
    Outcome<EnsembleSpec> spec = decode_spec(std::span<const std::byte>(spec_bytes.value().data(),
                                                                        spec_bytes.value().size()),
                                             limits);
    if (!spec.ok()) return fail<CoordinatorEpoch>(spec.code(), spec.error().message);
    EnsembleRecord record;
    record.spec = std::move(spec).value();
    Outcome<bool> superseded = in.boolean();
    if (!superseded.ok()) return fail<CoordinatorEpoch>(superseded.code(), superseded.error().message);
    record.superseded = superseded.value();
    Outcome<std::uint32_t> participants = in.u32();
    if (!participants.ok()) return fail<CoordinatorEpoch>(participants.code(), participants.error().message);
    if (participants.value() > limits.max_participants_per_ensemble) {
      return fail<CoordinatorEpoch>(EnsembleError::resource_limit_exceeded,
                                    "declared participant count exceeds the configured bound");
    }
    for (std::uint32_t participant_index = 0; participant_index < participants.value();
         ++participant_index) {
      Outcome<ParticipantRecord> participant = get_participant_record(in, limits);
      if (!participant.ok()) return fail<CoordinatorEpoch>(participant.code(), participant.error().message);
      record.participants.push_back(std::move(participant).value());
    }
    Outcome<bool> has_execution = in.boolean();
    if (!has_execution.ok())
      return fail<CoordinatorEpoch>(has_execution.code(), has_execution.error().message);
    record.has_execution = has_execution.value();
    if (record.has_execution) {
      Outcome<ExecutionRecord> execution = get_execution_record(in, limits);
      if (!execution.ok()) return fail<CoordinatorEpoch>(execution.code(), execution.error().message);
      record.execution = std::move(execution).value();
    }
    Outcome<std::uint32_t> history = in.u32();
    if (!history.ok()) return fail<CoordinatorEpoch>(history.code(), history.error().message);
    if (history.value() > limits.max_retained_results) {
      return fail<CoordinatorEpoch>(EnsembleError::resource_limit_exceeded,
                                    "declared history count exceeds the configured bound");
    }
    for (std::uint32_t history_index = 0; history_index < history.value(); ++history_index) {
      Outcome<EnsembleResult> result = get_result(in, limits);
      if (!result.ok()) return fail<CoordinatorEpoch>(result.code(), result.error().message);
      record.history.push_back(std::move(result).value());
    }
    loaded.emplace(record.spec.id.value(), std::move(record));
  }
  const Status exhausted = in.require_exhausted("runtime state");
  if (!exhausted.ok()) {
    return fail<CoordinatorEpoch>(exhausted.code(), exhausted.error().message);
  }

  std::unique_lock lock(impl_->mutex);
  if (stored_epoch.value() == static_cast<std::uint64_t>(-1)) {
    return fail<CoordinatorEpoch>(EnsembleError::resource_limit_exceeded,
                                  "coordinator epoch space is exhausted");
  }
  impl_->ensembles = std::move(loaded);
  impl_->counters = counters;
  impl_->epoch = CoordinatorEpoch(stored_epoch.value() + 1u);
  impl_->coordinator_boot = impl_->config.coordinator_boot.valid()
                                ? impl_->config.coordinator_boot
                                : generate_worker_boot_id();
  if (waterfalls[0] != 0u) {
    impl_->ensemble_ids.observe(EnsembleId(waterfalls[0] - 1u));
  }
  if (waterfalls[1] != 0u) {
    impl_->candidate_ids.observe(CandidateId(waterfalls[1] - 1u));
  }
  if (waterfalls[2] != 0u) {
    impl_->evaluation_ids.observe(EvaluationId(waterfalls[2] - 1u));
  }
  if (waterfalls[3] != 0u) {
    impl_->result_ids.observe(ResultId(waterfalls[3] - 1u));
  }
  if (waterfalls[4] != 0u) {
    impl_->sequences.observe(Sequence(waterfalls[4] - 1u));
  }
  for (auto& entry : impl_->ensembles) {
    EnsembleRecord& ensemble = entry.second;
    impl_->ensemble_ids.observe(ensemble.spec.id);
    for (const ParticipantRecord& participant : ensemble.participants) {
      (void)participant;
    }
    if (!ensemble.has_execution) {
      continue;
    }
    for (const CandidateRecord& candidate : ensemble.execution.candidates) {
      impl_->candidate_ids.observe(candidate.id);
      impl_->sequences.observe(candidate.received_sequence);
      impl_->sequences.observe(candidate.dispatch_sequence);
    }
    for (const EvaluationRecord& evaluation : ensemble.execution.evaluations) {
      impl_->evaluation_ids.observe(evaluation.id);
      impl_->sequences.observe(evaluation.received_sequence);
    }
    if (ensemble.execution.has_result) {
      impl_->result_ids.observe(ensemble.execution.result.id);
      impl_->sequences.observe(ensemble.execution.result.commit_sequence);
    }
  }
  // Reports are derived state and are recomputed from recovered evidence so
  // that inspection after recovery shows the same picture as before it.  The
  // committed result itself is restored verbatim, never recomputed.
  for (auto& entry : impl_->ensembles) {
    EnsembleRecord& ensemble = entry.second;
    if (ensemble.has_execution) {
      compute_reports(ensemble, ensemble.execution);
    }
  }
  (void)stored_boot;
  const CoordinatorEpoch recovered_epoch = impl_->epoch;
  lock.unlock();
  // A recovered coordinator never inherits live process authority: every
  // recovered participant must publish fresh evidence, and every in-flight
  // execution is conservatively marked as requiring revalidation.
  const Status invalidated =
      invalidate_dynamic_authority("coordinator recovery: live authority is not durable");
  if (!invalidated.ok()) {
    return fail<CoordinatorEpoch>(invalidated.code(), invalidated.error().message);
  }
  return recovered_epoch;
}

Outcome<CoordinatorEpoch> EnsembleFabric::restart(std::string reason) {
  if (epoch().value() == static_cast<std::uint64_t>(-1)) {
    return fail<CoordinatorEpoch>(EnsembleError::resource_limit_exceeded,
                                  "coordinator epoch space is exhausted");
  }
  const Status invalidated = invalidate_dynamic_authority(reason);
  if (!invalidated.ok()) {
    return fail<CoordinatorEpoch>(invalidated.code(), invalidated.error().message);
  }
  std::unique_lock lock(impl_->mutex);
  impl_->epoch = CoordinatorEpoch(impl_->epoch.value() + 1u);
  impl_->coordinator_boot = impl_->config.coordinator_boot.valid()
                                ? impl_->config.coordinator_boot
                                : generate_worker_boot_id();
  return impl_->epoch;
}

}  // namespace ensemble_fabric