#include "example_common.hpp"

/// Specialist then judge: a sequential stage runs one specialist at a time, and
/// the specialist role is what makes the result selectable.
int main() {
  using namespace examples;
  EnsembleFabric fabric;
  LocalEnsembleDriver driver(fabric);

  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "specialist-judge";
  spec.task_class = "physics";
  spec.participants.push_back(specialist(ParticipantId(1), "physics"));
  spec.participants.push_back(specialist(ParticipantId(2), "chemistry"));
  spec.participants.push_back(judge(ParticipantId(11)));
  spec.stages.push_back(stage(StageId(1), {ParticipantId(1), ParticipantId(2)},
                              TopologyMode::sequential));
  spec.quorum.min_evaluations = 1u;
  spec.evidence.min_evaluations_per_candidate = 1u;
  spec.arbitration.factors = {ArbitrationFactor::specialist_relevance,
                              ArbitrationFactor::aggregate_score};

  const Outcome<EnsembleGeneration> defined = driver.define(spec);
  const Outcome<EnsembleExecutionId> opened = driver.open(EnsembleId(1), defined.value());
  if (!defined.ok() || !opened.ok()) {
    std::cerr << "setup failed\n";
    return 1;
  }
  driver.attach(EnsembleId(1), defined.value(), spec.participants[0],
                scripted("ok:value=physics-answer"), WorkerId(1), WorkerBootId(401));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[1],
                scripted("ok:value=chemistry-answer"), WorkerId(2), WorkerBootId(402));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[2],
                scripted("evaluate:judgment=accept,score=0.5"), WorkerId(11), WorkerBootId(411));
  if (!driver.register_all().ok() || !driver.run().ok()) {
    std::cerr << "execution failed\n";
    return 1;
  }
  const Outcome<EnsembleResult> result = driver.fabric().finalize(EnsembleId(1), defined.value());
  if (!result.ok()) {
    std::cerr << "finalize failed: " << result.error().message << "\n";
    return 1;
  }
  report(result.value(), "specialist-judge");
  return 0;
}
