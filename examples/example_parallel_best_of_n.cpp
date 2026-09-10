#include "example_common.hpp"

/// Parallel best-of-N: several independent candidate participants produce
/// content, hard eligibility runs first, and exactly one result is committed.
int main() {
  using namespace examples;
  EnsembleFabric fabric;
  LocalEnsembleDriver driver(fabric);

  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "best-of-n";
  spec.task_class = "summarisation";
  spec.participants.push_back(candidate(ParticipantId(1)));
  spec.participants.push_back(candidate(ParticipantId(2)));
  spec.participants.push_back(candidate(ParticipantId(3)));
  spec.stages.push_back(stage(StageId(1), {ParticipantId(1), ParticipantId(2), ParticipantId(3)}));
  spec.quorum.min_valid_candidates = 1u;
  spec.arbitration.factors = {ArbitrationFactor::aggregate_score};
  spec.arbitration.tie_break = TieBreakPolicy::lowest_candidate_id;

  const Outcome<EnsembleGeneration> defined = driver.define(spec);
  if (!defined.ok()) {
    std::cerr << "define failed: " << defined.error().message << "\n";
    return 1;
  }
  const Outcome<EnsembleExecutionId> opened = driver.open(EnsembleId(1), defined.value());
  if (!opened.ok()) {
    std::cerr << "open failed: " << opened.error().message << "\n";
    return 1;
  }
  driver.attach(EnsembleId(1), defined.value(), spec.participants[0],
                scripted("ok:value=candidate-one"), WorkerId(1), WorkerBootId(101));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[1],
                scripted("ok:value=candidate-two"), WorkerId(2), WorkerBootId(102));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[2],
                scripted("fail:kind=internal_error"), WorkerId(3), WorkerBootId(103));
  if (!driver.register_all().ok() || !driver.run().ok()) {
    std::cerr << "execution failed\n";
    return 1;
  }
  const Outcome<EnsembleResult> result = driver.fabric().finalize(EnsembleId(1), defined.value());
  if (!result.ok()) {
    std::cerr << "finalize failed: " << result.error().message << "\n";
    return 1;
  }
  report(result.value(), "best-of-n");
  return 0;
}
