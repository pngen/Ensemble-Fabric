#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "ensemble_fabric/ensemble_fabric.hpp"
#include "ensemble_fabric/local_driver.hpp"

/// Real device entry points, implemented in cuda_proof.cu.  They perform device
/// work only; every authority decision is made here, by the runtime.
extern "C" int ef_cuda_accelerator_proof(char* detail, std::size_t detail_bytes);
extern "C" int ef_cuda_device_memory_baseline(char* detail, std::size_t detail_bytes);

namespace {

using namespace ensemble_fabric;

[[nodiscard]] bool accelerator_proof(std::string& detail) {
  std::array<char, 512> buffer{};
  const int result = ef_cuda_accelerator_proof(buffer.data(), buffer.size());
  detail = std::string(buffer.data());
  return result == 0;
}

[[nodiscard]] bool device_memory_baseline(std::string& detail) {
  std::array<char, 512> buffer{};
  const int result = ef_cuda_device_memory_baseline(buffer.data(), buffer.size());
  detail = std::string(buffer.data());
  return result == 0;
}

/// A participant backend whose produce() performs real CUDA work.  It never
/// decides whether it may run: the runtime only calls it for a candidate that
/// the current generation still authorizes.
class CudaParticipantBackend final : public ParticipantBackend {
 public:
  [[nodiscard]] Outcome<ParticipantProduction> produce(
      const DispatchCandidateMessage& request) override {
    ParticipantProduction production;
    std::string detail;
    if (!accelerator_proof(detail)) {
      production.kind = ParticipantProduction::Kind::failure;
      production.failure = ParticipantFailureKind::internal_error;
      production.detail = "accelerator proof failed: " + detail;
      return production;
    }
    production.kind = ParticipantProduction::Kind::content;
    const std::string value = "cuda-candidate-" + request.candidate.to_string();
    production.payload.assign(reinterpret_cast<const std::byte*>(value.data()),
                              reinterpret_cast<const std::byte*>(value.data()) + value.size());
    production.budget_units_used = request.budget_units;
    return production;
  }

  [[nodiscard]] Outcome<ParticipantEvaluation> evaluate(
      const EvaluationRequestMessage& request) override {
    ParticipantEvaluation evaluation;
    evaluation.judgment = CategoricalJudgment::accept;
    evaluation.verification = VerificationState::pass;
    evaluation.score_defined = true;
    evaluation.score = 0.9;
    evaluation.weight = 1.0;
    evaluation.rationale = "accelerator-backed candidate verified on candidate " +
                           request.candidate.to_string();
    return evaluation;
  }
};

ParticipantSpec candidate(ParticipantId id) {
  ParticipantSpec participant;
  participant.id = id;
  participant.label = "cuda-candidate";
  participant.roles.insert(ParticipantRole::candidate);
  return participant;
}

ParticipantSpec judge(ParticipantId id) {
  ParticipantSpec participant;
  participant.id = id;
  participant.label = "judge";
  participant.roles.insert(ParticipantRole::judge);
  participant.required = true;
  return participant;
}

/// A candidate whose generation is superseded while it is in flight must not be
/// able to publish accelerator-backed work.
[[nodiscard]] bool fenced_proof(std::string& detail) {
  EnsembleFabric fabric;
  LocalEnsembleDriver driver(fabric);

  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "cuda-fencing";
  spec.task_class = "accelerator";
  spec.participants.push_back(candidate(ParticipantId(1)));
  StageSpec stage;
  stage.id = StageId(1);
  stage.label = "primary";
  stage.mode = TopologyMode::parallel;
  stage.participants = {ParticipantId(1)};
  spec.stages.push_back(stage);
  spec.quorum.min_valid_candidates = 1u;

  const Outcome<EnsembleGeneration> generation = driver.define(spec);
  if (!generation.ok()) {
    detail = "define failed: " + generation.error().message;
    return false;
  }
  const Outcome<EnsembleExecutionId> opened = driver.open(EnsembleId(1), generation.value());
  if (!opened.ok()) {
    detail = "open failed: " + opened.error().message;
    return false;
  }
  LocalParticipant& participant =
      driver.attach(EnsembleId(1), generation.value(), spec.participants[0],
                    std::make_shared<CudaParticipantBackend>(), WorkerId(1), WorkerBootId(1));
  const Status registered = driver.register_participant(participant);
  if (!registered.ok()) {
    detail = "registration failed: " + registered.error().message;
    return false;
  }

  const Outcome<std::vector<FabricAction>> actions =
      fabric.advance(EnsembleId(1), generation.value());
  if (!actions.ok() || actions.value().empty()) {
    detail = "the runtime produced no dispatch to fence";
    return false;
  }
  const auto* dispatch = std::get_if<DispatchAction>(&actions.value().front());
  if (dispatch == nullptr) {
    detail = "the first action was not a dispatch";
    return false;
  }

  // The ensemble generation is superseded while the candidate is in flight.
  const Status superseded = fabric.supersede_ensemble(EnsembleId(1), generation.value());
  if (!superseded.ok()) {
    detail = "supersede failed: " + superseded.error().message;
    return false;
  }

  CandidateAuthority authority;
  authority.participant = driver.authority_of(participant);
  authority.execution = dispatch->execution.execution;
  authority.attempt_generation = dispatch->execution.attempt_generation;
  authority.candidate = dispatch->candidate;
  authority.candidate_generation = dispatch->candidate_generation;
  CandidateSubmission submission;
  submission.authority = authority;
  const std::string value = "cuda-output-after-fencing";
  submission.payload.assign(reinterpret_cast<const std::byte*>(value.data()),
                            reinterpret_cast<const std::byte*>(value.data()) + value.size());
  const Status published = fabric.submit_candidate(submission);
  if (published.ok()) {
    detail = "a superseded generation accepted accelerator output";
    return false;
  }
  const Outcome<EnsembleResult> finalized = fabric.finalize(EnsembleId(1), generation.value());
  if (finalized.ok()) {
    detail = "a superseded generation committed a result";
    return false;
  }
  const Outcome<EnsembleResult> current = fabric.current_result(EnsembleId(1));
  if (current.ok()) {
    detail = "a superseded generation published an authoritative result";
    return false;
  }
  detail = "a superseded candidate could not publish accelerator work; rejection=" +
           std::string(to_string(published.code()));
  return true;
}

}  // namespace

int main() {
  std::string detail;
  std::cout << "Ensemble Fabric CUDA authority-gating proof" << std::endl;

  // Phase 1: the raw accelerator contract (allocation, H2D, kernel, D2H, CPU
  // reference parity, device cleanup).
  if (!accelerator_proof(detail)) {
    std::cerr << "accelerator proof failed: " << detail << "\n";
    return 1;
  }
  std::cout << "  accelerator: " << detail << std::endl;

  // Phase 2: a real ensemble whose candidate participant performs CUDA work.
  EnsembleFabric fabric;
  LocalEnsembleDriver driver(fabric);
  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "cuda-proof";
  spec.task_class = "accelerator";
  spec.participants.push_back(candidate(ParticipantId(1)));
  spec.participants.push_back(judge(ParticipantId(11)));
  StageSpec stage;
  stage.id = StageId(1);
  stage.label = "primary";
  stage.mode = TopologyMode::parallel;
  stage.participants = {ParticipantId(1)};
  spec.stages.push_back(stage);
  spec.quorum.min_valid_candidates = 1u;
  spec.quorum.min_evaluations = 1u;
  spec.quorum.count_optional_participants = false;
  spec.evidence.min_evaluations_per_candidate = 1u;
  spec.aggregation.kind = AggregationKind::judge_arbitration;
  spec.arbitration.factors = {ArbitrationFactor::aggregate_score};
  spec.arbitration.tie_break = TieBreakPolicy::lowest_candidate_id;

  const Outcome<EnsembleGeneration> generation = driver.define(spec);
  if (!generation.ok()) {
    std::cerr << "define failed: " << generation.error().message << "\n";
    return 1;
  }
  const Outcome<EnsembleExecutionId> opened = driver.open(EnsembleId(1), generation.value());
  if (!opened.ok()) {
    std::cerr << "open failed: " << opened.error().message << "\n";
    return 1;
  }
  driver.attach(EnsembleId(1), generation.value(), spec.participants[0],
                std::make_shared<CudaParticipantBackend>(), WorkerId(1), WorkerBootId(1));
  driver.attach(EnsembleId(1), generation.value(), spec.participants[1],
                std::make_shared<ReferenceBackend>(
                    std::move(*ReferenceProgram::parse("evaluate:judgment=accept,score=0.9"))),
                WorkerId(2), WorkerBootId(2));
  const Status registered = driver.register_all();
  if (!registered.ok()) {
    std::cerr << "registration failed: " << registered.error().message << "\n";
    return 1;
  }
  const Status ran = driver.run();
  if (!ran.ok()) {
    std::cerr << "ensemble execution failed: " << ran.error().message << "\n";
    return 1;
  }
  const Outcome<EnsembleResult> result = driver.fabric().finalize(EnsembleId(1), generation.value());
  if (!result.ok()) {
    std::cerr << "finalize failed: " << result.error().message << "\n";
    return 1;
  }
  if (!result.value().authoritative()) {
    std::cerr << "the accelerator-backed result was not authoritative\n";
    return 1;
  }
  std::cout << "  ensemble: " << result.value().render() << std::endl;

  // Phase 3: a fenced candidate must not be able to publish accelerator work
  // after its generation is superseded.
  if (!fenced_proof(detail)) {
    std::cerr << "fenced accelerator proof failed: " << detail << "\n";
    return 1;
  }
  std::cout << "  fencing: " << detail << std::endl;

  // Phase 4: device memory returns to its baseline.
  if (!device_memory_baseline(detail)) {
    std::cerr << "device memory was not returned to baseline: " << detail << "\n";
    return 1;
  }
  std::cout << "  cleanup: " << detail << std::endl;
  return 0;
}
