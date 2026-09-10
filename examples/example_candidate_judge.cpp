#include "example_common.hpp"

/// Candidate plus judge: judges consume candidate output but never become the
/// final answer themselves.
int main() {
  using namespace examples;
  EnsembleFabric fabric;
  LocalEnsembleDriver driver(fabric);

  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "candidate-judge";
  spec.task_class = "classification";
  spec.participants.push_back(candidate(ParticipantId(1)));
  spec.participants.push_back(candidate(ParticipantId(2)));
  spec.participants.push_back(judge(ParticipantId(11)));
  spec.participants.push_back(judge(ParticipantId(12)));
  spec.stages.push_back(stage(StageId(1), {ParticipantId(1), ParticipantId(2)}));
  spec.aggregation.kind = AggregationKind::judge_arbitration;
  spec.aggregation.require_judge_acceptance = true;
  spec.aggregation.min_judge_acceptance_ratio = 0.5;
  spec.quorum.min_evaluations = 2u;
  spec.evidence.min_evaluations_per_candidate = 2u;
  spec.arbitration.factors = {ArbitrationFactor::judge_acceptance_ratio,
                              ArbitrationFactor::aggregate_score};
  spec.judging_criteria = {CriterionRef{1u, 1u}};

  const Outcome<EnsembleGeneration> defined = driver.define(spec);
  const Outcome<EnsembleExecutionId> opened = driver.open(EnsembleId(1), defined.value());
  if (!defined.ok() || !opened.ok()) {
    std::cerr << "setup failed\n";
    return 1;
  }
  driver.attach(EnsembleId(1), defined.value(), spec.participants[0],
                scripted("ok:value=answer-a"), WorkerId(1), WorkerBootId(201));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[1],
                scripted("ok:value=answer-b"), WorkerId(2), WorkerBootId(202));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[2],
                scripted("evaluate:judgment=accept,score=0.8,rationale=clear"),
                WorkerId(11), WorkerBootId(211));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[3],
                scripted("evaluate:judgment=accept,score=0.6,rationale=plausible"),
                WorkerId(12), WorkerBootId(212));
  if (!driver.register_all().ok() || !driver.run().ok()) {
    std::cerr << "execution failed\n";
    return 1;
  }
  const Outcome<EnsembleResult> result = driver.fabric().finalize(EnsembleId(1), defined.value());
  if (!result.ok()) {
    std::cerr << "finalize failed: " << result.error().message << "\n";
    return 1;
  }
  report(result.value(), "candidate-judge");
  return 0;
}
