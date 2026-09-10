#include "ensemble_fabric/fabric.hpp"

#include <algorithm>
#include <utility>

#include "detail/text.hpp"
#include "fabric_internal.hpp"

namespace ensemble_fabric {

std::string_view to_string(ExecutionStatus status) noexcept {
  switch (status) {
    case ExecutionStatus::none: return "NONE";
    case ExecutionStatus::open: return "OPEN";
    case ExecutionStatus::committed: return "COMMITTED";
    case ExecutionStatus::cancelled: return "CANCELLED";
    case ExecutionStatus::superseded: return "SUPERSEDED";
    case ExecutionStatus::revalidation_required: return "REVALIDATION_REQUIRED";
    case ExecutionStatus::failed: return "FAILED";
    case ExecutionStatus::no_eligible_candidate: return "NO_ELIGIBLE_CANDIDATE";
  }
  return "UNKNOWN_STATUS";
}

std::vector<StageSpec> effective_stages(const EnsembleSpec& spec) {
  if (!spec.stages.empty()) {
    return spec.stages;
  }
  StageSpec implicit;
  implicit.id = StageId(1);
  implicit.label = "implicit";
  implicit.mode = TopologyMode::parallel;
  for (const ParticipantSpec& participant : spec.participants) {
    const bool produces = participant.roles.contains(ParticipantRole::candidate) ||
                          participant.roles.contains(ParticipantRole::specialist);
    const bool speculative_fallback =
        spec.fallback.eager_speculative && participant.roles.contains(ParticipantRole::fallback);
    if (produces || speculative_fallback) {
      implicit.participants.push_back(participant.id);
    }
  }
  std::vector<StageSpec> stages;
  if (!implicit.participants.empty()) {
    stages.push_back(std::move(implicit));
  }
  return stages;
}

std::vector<const ParticipantSpec*> evaluating_participants(const EnsembleSpec& spec) {
  std::vector<const ParticipantSpec*> out;
  for (const ParticipantSpec& participant : spec.participants) {
    if (participant.roles.contains(ParticipantRole::judge) ||
        participant.roles.contains(ParticipantRole::verifier)) {
      out.push_back(&participant);
    }
  }
  return out;
}

std::vector<CriterionRef> judging_criteria(const EnsembleSpec& spec) {
  if (!spec.judging_criteria.empty()) {
    return spec.judging_criteria;
  }
  return {CriterionRef{1u, 1u}};
}

EnsembleFabric::EnsembleFabric(FabricConfig config) : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
  impl_->epoch = impl_->config.initial_epoch.valid() ? impl_->config.initial_epoch
                                                     : CoordinatorEpoch(1);
  impl_->coordinator_boot = impl_->config.coordinator_boot;
  if (!impl_->coordinator_boot.valid()) {
    impl_->coordinator_boot = WorkerBootId(1);
  }
}

EnsembleFabric::~EnsembleFabric() = default;

CoordinatorEpoch EnsembleFabric::epoch() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->epoch;
}

WorkerBootId EnsembleFabric::coordinator_boot() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->coordinator_boot;
}

Limits EnsembleFabric::limits() const { return impl_->config.limits; }

const FabricConfig& EnsembleFabric::config() const noexcept { return impl_->config; }

EnsembleFabric::Counters EnsembleFabric::counters() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->counters;
}

Outcome<EnsembleGeneration> EnsembleFabric::define_ensemble(EnsembleSpec spec) {
  std::unique_lock lock(impl_->mutex);
  const Limits& limits = impl_->config.limits;

  if (!spec.id.valid()) {
    const auto allocated = impl_->ensemble_ids.allocate();
    if (!allocated.has_value()) {
      return fail<EnsembleGeneration>(EnsembleError::resource_limit_exceeded,
                                      "ensemble identity space is exhausted");
    }
    spec.id = *allocated;
  } else {
    impl_->ensemble_ids.observe(spec.id);
  }

  EnsembleRecord* existing = impl_->find(spec.id);
  if (existing == nullptr) {
    if (spec.generation.valid() && spec.generation != EnsembleGeneration(1)) {
      return fail<EnsembleGeneration>(EnsembleError::invalid_argument,
                                      "a new ensemble must start at generation 1");
    }
    spec.generation = EnsembleGeneration(1);
  } else {
    if (existing->spec.generation.value() == static_cast<std::uint64_t>(-1)) {
      return fail<EnsembleGeneration>(EnsembleError::resource_limit_exceeded,
                                      "ensemble generation space is exhausted");
    }
    // The next generation is computed without mutating current state, so a
    // rejected redefinition leaves the previous generation fully intact.
    const EnsembleGeneration next(existing->spec.generation.value() + 1u);
    if (spec.generation.valid() && spec.generation != next) {
      return fail<EnsembleGeneration>(
          EnsembleError::stale_ensemble_generation,
          "redefinition must advance to generation " + next.to_string() + ", got " +
              spec.generation.to_string());
    }
    spec.generation = next;
  }

  const Status structural = validate_spec(spec, limits);
  if (!structural.ok()) {
    return fail<EnsembleGeneration>(structural.code(), structural.error().message);
  }
  spec.fingerprint = compute_spec_fingerprint(spec);

  const bool redefining = existing != nullptr;
  if (redefining) {
    // Reconfiguration fences the previous generation: an in-flight execution of
    // the old generation can never commit afterwards.
    if (existing->has_execution && existing->execution.status == ExecutionStatus::open) {
      existing->execution.status = ExecutionStatus::superseded;
      impl_->counters.supersessions += 1u;
    }
    if (existing->has_execution && existing->execution.has_result) {
      // Committed results stay inspectable as history; they never become
      // current again because their generation is now historical.
      existing->history.push_back(existing->execution.result);
      const std::size_t retained = impl_->config.limits.max_retained_results;
      if (existing->history.size() > retained) {
        existing->history.erase(existing->history.begin(),
                                existing->history.begin() +
                                    static_cast<std::ptrdiff_t>(existing->history.size() - retained));
      }
    }
    existing->superseded = false;
    existing->participants.clear();
    existing->has_execution = false;
    existing->execution = ExecutionRecord{};
  } else {
    EnsembleRecord record;
    record.spec = spec;
    auto inserted = impl_->ensembles.emplace(spec.id.value(), std::move(record));
    existing = &inserted.first->second;
  }

  existing->spec = spec;
  existing->participants.clear();
  for (const ParticipantSpec& declaration : spec.participants) {
    ParticipantRecord participant;
    participant.id = declaration.id;
    participant.generation = ParticipantGeneration(1);
    participant.declaration = declaration;
    // Recovered or reconfigured participants are never silently current: they
    // must publish fresh evidence under the new generation.
    participant.status = ParticipantStatus::revalidation_required;
    existing->participants.push_back(std::move(participant));
  }
  return spec.generation;
}

Status EnsembleFabric::supersede_ensemble(EnsembleId id, EnsembleGeneration generation) {
  std::unique_lock lock(impl_->mutex);
  EnsembleRecord* ensemble = impl_->find(id);
  if (ensemble == nullptr) {
    return fail(EnsembleError::unknown_ensemble, "unknown ensemble " + id.to_string());
  }
  if (ensemble->spec.generation != generation) {
    return fail(EnsembleError::stale_ensemble_generation,
                "supersede requested for generation " + generation.to_string() + " but the current "
                "generation is " + ensemble->spec.generation.to_string());
  }
  ensemble->superseded = true;
  if (ensemble->has_execution && ensemble->execution.status == ExecutionStatus::open) {
    ensemble->execution.status = ExecutionStatus::superseded;
    for (CandidateRecord& candidate : ensemble->execution.candidates) {
      if (candidate.in_flight()) {
        candidate.state = CandidateState::superseded;
      }
    }
  }
  impl_->counters.supersessions += 1u;
  return ok_status();
}

Outcome<EnsembleSpec> EnsembleFabric::spec(EnsembleId id) const {
  std::shared_lock lock(impl_->mutex);
  const EnsembleRecord* ensemble = impl_->find(id);
  if (ensemble == nullptr) {
    return fail<EnsembleSpec>(EnsembleError::unknown_ensemble, "unknown ensemble " + id.to_string());
  }
  return ensemble->spec;
}

Status validate_result(const EnsembleResult& result, const EnsembleSpec& spec,
                       CoordinatorEpoch current_epoch) {
  if (result.ensemble != spec.id) {
    return fail(EnsembleError::invalid_argument, "result names a different ensemble");
  }
  if (result.ensemble_generation != spec.generation) {
    return fail(EnsembleError::stale_ensemble_generation,
                "result was produced for ensemble generation " +
                    result.ensemble_generation.to_string() + " but the current generation is " +
                    spec.generation.to_string());
  }
  if (result.coordinator_epoch != current_epoch) {
    return fail(EnsembleError::stale_coordinator_epoch,
                "result was produced under epoch " + result.coordinator_epoch.to_string() +
                    " but the current epoch is " + current_epoch.to_string());
  }
  if (!result.execution.valid() || !result.attempt_generation.valid()) {
    return fail(EnsembleError::invalid_identity_combination,
                "result must name its execution and attempt generation");
  }
  if (result.decision == EnsembleDecision::committed && !result.has_selected) {
    return fail(EnsembleError::invalid_argument,
                "a committed result must name the candidate it selected");
  }
  return ok_status();
}

}  // namespace ensemble_fabric
