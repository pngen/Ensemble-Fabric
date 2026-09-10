#pragma once

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ensemble_fabric/fabric.hpp"
#include "ensemble_fabric/reference_backend.hpp"
#include "ensemble_fabric/version.hpp"

namespace ef_test {

using namespace ensemble_fabric;

/// A synthetic participant bound to an in-process reference backend.
struct HarnessParticipant {
  ParticipantId id;
  RoleSet roles;
  WorkerId worker;
  WorkerBootId boot;
  ParticipantGeneration generation;
  CompatibilityProfile profile;
  std::string domain;
  std::shared_ptr<ReferenceBackend> backend;
  bool registered{false};
  bool killed{false};
};

/// Drives an EnsembleFabric in-process by executing the actions the runtime
/// produces.  This is exactly the contract the distributed coordinator
/// implements over the wire; using it here keeps the unit tests honest about
/// which side owns authority.
class EnsembleHarness {
 public:
  explicit EnsembleHarness(EnsembleFabric& fabric) : fabric_(fabric) {}

  [[nodiscard]] Status define(EnsembleSpec spec) {
    const std::uint64_t requested = spec.id.valid() ? spec.id.value() : 0u;
    Outcome<EnsembleGeneration> generation = fabric_.define_ensemble(std::move(spec));
    if (!generation.ok()) {
      return fail(generation.code(), generation.error().message);
    }
    (void)requested;
    return ok_status();
  }

  [[nodiscard]] Outcome<EnsembleExecutionId> open(EnsembleId ensemble,
                                                  EnsembleGeneration generation) {
    return fabric_.open_execution(ensemble, generation);
  }

  HarnessParticipant& attach(EnsembleId ensemble, EnsembleGeneration ensemble_generation,
                             ParticipantSpec declaration, ReferenceProgram program,
                             std::uint64_t worker_serial, CompatibilityProfile profile = {}) {
    HarnessParticipant participant;
    participant.id = declaration.id;
    participant.roles = declaration.roles;
    participant.worker = WorkerId(worker_serial);
    participant.boot = WorkerBootId(1000u + worker_serial);
    participant.profile = profile;
    participant.domain = declaration.domain;
    participant.backend = std::make_shared<ReferenceBackend>(std::move(program));
    if (profile.protocol_version == 0u) {
      participant.profile.protocol_version = protocol_version;
    }
    participant.profile.task_class = declaration.compatibility.required_task_class;
    participants_.push_back(std::move(participant));
    ensemble_ = ensemble;
    ensemble_generation_ = ensemble_generation;
    return participants_.back();
  }

  [[nodiscard]] Status register_participant(HarnessParticipant& participant) {
    ParticipantRegistration registration;
    registration.ensemble = ensemble_;
    registration.ensemble_generation = ensemble_generation_;
    registration.participant = participant.id;
    registration.worker = participant.worker;
    registration.worker_boot = participant.boot;
    registration.coordinator_epoch = fabric_.epoch();
    registration.profile = participant.profile;
    registration.claimed_roles = participant.roles;
    Outcome<ParticipantGeneration> generation = fabric_.register_participant(registration);
    if (!generation.ok()) {
      return fail(generation.code(), generation.error().message);
    }
    participant.generation = generation.value();
    participant.registered = true;
    participant.killed = false;
    return ok_status();
  }

  [[nodiscard]] Status register_all() {
    for (HarnessParticipant& participant : participants_) {
      const Status status = register_participant(participant);
      if (!status.ok()) {
        return status;
      }
    }
    return ok_status();
  }

  [[nodiscard]] ParticipantAuthority authority_of(const HarnessParticipant& participant) const {
    ParticipantAuthority authority;
    authority.ensemble = ensemble_;
    authority.ensemble_generation = ensemble_generation_;
    authority.participant = participant.id;
    authority.participant_generation = participant.generation;
    authority.worker = participant.worker;
    authority.worker_boot = participant.boot;
    authority.coordinator_epoch = fabric_.epoch();
    return authority;
  }

  /// Executes one round of actions.  Returns the number of actions performed.
  [[nodiscard]] Outcome<std::size_t> step() {
    Outcome<std::vector<FabricAction>> actions = fabric_.advance(ensemble_, ensemble_generation_);
    if (!actions.ok()) {
      return fail<std::size_t>(actions.code(), actions.error().message);
    }
    std::size_t performed = 0;
    for (const FabricAction& action : actions.value()) {
      if (const auto* dispatch = std::get_if<DispatchAction>(&action)) {
        if (perform_dispatch(*dispatch)) {
          ++performed;
        }
      } else if (const auto* evaluation = std::get_if<EvaluationAction>(&action)) {
        if (perform_evaluation(*evaluation)) {
          ++performed;
        }
      } else if (std::get_if<CancelAction>(&action) != nullptr) {
        ++performed;
      } else {
        ++performed;
      }
    }
    return performed;
  }

  /// Executes the backend for an already dispatched candidate of p participant
  /// and submits whatever it produced.  Used by tests that need a candidate to
  /// be in flight while something else happens.
  [[nodiscard]] Status complete_dispatched(ParticipantId participant_id) {
    Outcome<EnsembleSnapshot> snapshot = fabric_.inspect(ensemble_);
    if (!snapshot.ok()) {
      return fail(snapshot.code(), snapshot.error().message);
    }
    for (const CandidateSnapshot& view : snapshot.value().candidates) {
      if (view.participant != participant_id || view.state != CandidateState::dispatched) {
        continue;
      }
      HarnessParticipant* participant = find(participant_id);
      if (participant == nullptr || participant->killed) {
        return fail(EnsembleError::participant_unavailable, "participant is not available");
      }
      DispatchCandidateMessage request;
      request.ensemble = ensemble_;
      request.ensemble_generation = ensemble_generation_;
      request.execution = view.execution;
      request.attempt_generation = view.attempt_generation;
      request.participant = participant_id;
      request.participant_generation = view.participant_generation;
      request.coordinator_epoch = fabric_.epoch();
      request.candidate = view.id;
      request.candidate_generation = view.generation;
      request.role = view.role;
      request.stage = view.stage;
      request.attempt = view.attempt;
      request.domain = view.domain;

      CandidateAuthority authority;
      authority.participant = authority_of(*participant);
      authority.execution = view.execution;
      authority.attempt_generation = view.attempt_generation;
      authority.candidate = view.id;
      authority.candidate_generation = view.generation;

      Outcome<ParticipantProduction> production = participant->backend->produce(request);
      if (!production.ok()) {
        return fail(production.code(), production.error().message);
      }
      switch (production.value().kind) {
        case ParticipantProduction::Kind::content: {
          CandidateSubmission submission;
          submission.authority = authority;
          submission.payload = production.value().payload;
          return fabric_.submit_candidate(submission);
        }
        case ParticipantProduction::Kind::abstention: {
          CandidateAbstentionSubmission submission;
          submission.authority = authority;
          submission.rationale = production.value().detail;
          return fabric_.submit_candidate_abstention(submission);
        }
        case ParticipantProduction::Kind::failure: {
          CandidateFailureSubmission submission;
          submission.authority = authority;
          submission.participant_failure = static_cast<std::uint32_t>(production.value().failure);
          return fabric_.submit_candidate_failure(submission);
        }
      }
      return fail(EnsembleError::internal_error, "unreachable production kind");
    }
    return fail(EnsembleError::unknown_candidate, "no dispatched candidate for this participant");
  }

  /// Dispatches the next round of actions without executing them, so that a
  /// candidate can be observed while it is genuinely in flight.
  [[nodiscard]] Outcome<std::size_t> declare_only() {
    Outcome<std::vector<FabricAction>> actions = fabric_.advance(ensemble_, ensemble_generation_);
    if (!actions.ok()) {
      return fail<std::size_t>(actions.code(), actions.error().message);
    }
    return actions.value().size();
  }

  /// Runs until the runtime has no further actions to offer.
  [[nodiscard]] Status run(std::size_t max_rounds = 4096u) {
    for (std::size_t round = 0; round < max_rounds; ++round) {
      Outcome<std::size_t> performed = step();
      if (!performed.ok()) {
        return fail(performed.code(), performed.error().message);
      }
      if (performed.value() == 0u) {
        return ok_status();
      }
    }
    return fail(EnsembleError::resource_limit_exceeded,
                "the in-process harness exceeded its round budget");
  }

  [[nodiscard]] HarnessParticipant* find(ParticipantId id) {
    for (HarnessParticipant& participant : participants_) {
      if (participant.id == id) {
        return &participant;
      }
    }
    return nullptr;
  }

  [[nodiscard]] EnsembleFabric& fabric() noexcept { return fabric_; }
  [[nodiscard]] EnsembleId ensemble() const noexcept { return ensemble_; }
  [[nodiscard]] EnsembleGeneration generation() const noexcept { return ensemble_generation_; }
  [[nodiscard]] std::deque<HarnessParticipant>& participants() noexcept { return participants_; }

 private:
  [[nodiscard]] bool perform_dispatch(const DispatchAction& dispatch) {
    HarnessParticipant* participant = find(dispatch.participant);
    if (participant == nullptr || participant->killed) {
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
        submission.participant_failure =
            static_cast<std::uint32_t>(production.value().failure);
        return fabric_.submit_candidate_failure(submission).ok();
      }
    }
    return false;
  }

  [[nodiscard]] bool perform_evaluation(const EvaluationAction& action) {
    HarnessParticipant* participant = find(action.judge);
    if (participant == nullptr || participant->killed) {
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

  EnsembleFabric& fabric_;
  EnsembleId ensemble_;
  EnsembleGeneration ensemble_generation_;
  std::deque<HarnessParticipant> participants_;
};

/// Parses a reference script, recording a test failure (never aborting) when the
/// script itself is malformed.  Keeps test bodies free of Outcome plumbing.
[[nodiscard]] inline Outcome<ReferenceProgram> reference_program(std::string_view script) {
  Outcome<ReferenceProgram> program = ReferenceProgram::parse(script);
  if (!program.ok()) {
    // A malformed script in a test is a defect in the test, so it is recorded
    // as a failure; the returned program is still a usable placeholder so the
    // case can finish instead of terminating the process.
    ef_test::fail(__FILE__, __LINE__,
                  std::string("invalid reference script '") + std::string(script) +
                      "': " + program.error().message);
    return ReferenceProgram::parse("ok:value=invalid-script");
  }
  return program;
}

// ---------------------------------------------------------------------------
// Small specification builders.  Tests stay readable and every specification
// still passes through the real validation path.
// ---------------------------------------------------------------------------

[[nodiscard]] inline ParticipantSpec candidate_participant(ParticipantId id,
                                                           std::string label = std::string(),
                                                           std::string domain = std::string(),
                                                           bool required = false) {
  ParticipantSpec participant;
  participant.id = id;
  participant.label = label.empty() ? ("candidate-" + id.to_string()) : std::move(label);
  participant.roles.insert(ParticipantRole::candidate);
  participant.required = required;
  participant.domain = std::move(domain);
  return participant;
}

[[nodiscard]] inline ParticipantSpec specialist_participant(ParticipantId id, std::string domain) {
  ParticipantSpec participant;
  participant.id = id;
  participant.label = "specialist-" + id.to_string();
  participant.roles.insert(ParticipantRole::specialist);
  participant.required = false;
  participant.domain = std::move(domain);
  return participant;
}

[[nodiscard]] inline ParticipantSpec judge_participant(ParticipantId id, bool required = true) {
  ParticipantSpec participant;
  participant.id = id;
  participant.label = "judge-" + id.to_string();
  participant.roles.insert(ParticipantRole::judge);
  participant.required = required;
  participant.domain = "judging";
  return participant;
}

[[nodiscard]] inline ParticipantSpec verifier_participant(ParticipantId id, bool required = true) {
  ParticipantSpec participant;
  participant.id = id;
  participant.label = "verifier-" + id.to_string();
  participant.roles.insert(ParticipantRole::verifier);
  participant.required = required;
  participant.domain = "verification";
  return participant;
}

[[nodiscard]] inline ParticipantSpec fallback_participant(ParticipantId id) {
  ParticipantSpec participant;
  participant.id = id;
  participant.label = "fallback-" + id.to_string();
  participant.roles.insert(ParticipantRole::fallback);
  participant.required = false;
  participant.domain = "fallback";
  return participant;
}

[[nodiscard]] inline StageSpec parallel_stage(StageId id, std::vector<ParticipantId> participants) {
  StageSpec stage;
  stage.id = id;
  stage.label = "stage-" + id.to_string();
  stage.mode = TopologyMode::parallel;
  stage.participants = std::move(participants);
  return stage;
}

[[nodiscard]] inline StageSpec sequential_stage(StageId id, std::vector<ParticipantId> participants) {
  StageSpec stage = parallel_stage(id, std::move(participants));
  stage.mode = TopologyMode::sequential;
  return stage;
}

/// A complete, valid parallel ensemble over the given candidate participants.
[[nodiscard]] inline EnsembleSpec parallel_spec(std::uint64_t id,
                                                std::vector<ParticipantId> candidates,
                                                std::vector<ParticipantId> judges = {}) {
  EnsembleSpec spec;
  spec.id = EnsembleId(id);
  spec.label = "parallel";
  spec.task_class = "task";
  for (const ParticipantId candidate : candidates) {
    spec.participants.push_back(candidate_participant(candidate));
  }
  for (const ParticipantId judge : judges) {
    spec.participants.push_back(judge_participant(judge));
  }
  spec.stages.push_back(parallel_stage(StageId(1), candidates));
  spec.quorum.min_valid_candidates = 1u;
  spec.arbitration.tie_break = TieBreakPolicy::lowest_candidate_id;
  spec.deterministic_seed = 1u;
  return spec;
}

}  // namespace ef_test

// Test translation units use the harness helpers unqualified; the support header
// makes them visible deliberately so that each test file stays readable.
using namespace ef_test;  // NOLINT(google-build-using-namespace)
