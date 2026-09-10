#include "example_common.hpp"

/// Verifier gating: a candidate with the best score is still ineligible when a
/// mandatory predicate fails.  Score is evidence, not authority.
int main() {
  using namespace examples;
  EnsembleFabric fabric;
  LocalEnsembleDriver driver(fabric);

  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "verifier-gated";
  spec.task_class = "safety";
  spec.participants.push_back(candidate(ParticipantId(1)));
  spec.participants.push_back(candidate(ParticipantId(2)));
  spec.participants.push_back(judge(ParticipantId(11)));
  spec.participants.push_back(verifier(ParticipantId(21)));
  spec.stages.push_back(stage(StageId(1), {ParticipantId(1), ParticipantId(2)}));
  spec.aggregation.kind = AggregationKind::verifier_gated;
  spec.aggregation.require_verification_pass = true;
  spec.evidence.mandatory_criteria = {CriterionRef{1u, 1u}};
  spec.evidence.min_evaluations_per_candidate = 1u;
  spec.arbitration.factors = {ArbitrationFactor::aggregate_score};

  const Outcome<EnsembleGeneration> defined = driver.define(spec);
  const Outcome<EnsembleExecutionId> opened = driver.open(EnsembleId(1), defined.value());
  if (!defined.ok() || !opened.ok()) {
    std::cerr << "setup failed\n";
    return 1;
  }
  driver.attach(EnsembleId(1), defined.value(), spec.participants[0],
                scripted("ok:value=unverified-high-score"), WorkerId(1), WorkerBootId(501));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[1],
                scripted("ok:value=verified-lower-score"), WorkerId(2), WorkerBootId(502));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[2],
                scripted("evaluate:judgment=accept,score=0.99;evaluate:judgment=accept,score=0.2"),
                WorkerId(11), WorkerBootId(511));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[3],
                scripted("evaluate:verification=fail;evaluate:verification=pass"), WorkerId(21),
                WorkerBootId(521));
  if (!driver.register_all().ok() || !driver.run().ok()) {
    std::cerr << "execution failed\n";
    return 1;
  }
  const Outcome<EnsembleResult> result = driver.fabric().finalize(EnsembleId(1), defined.value());
  if (!result.ok()) {
    std::cerr << "finalize failed: " << result.error().message << "\n";
    return 1;
  }
  report(result.value(), "verifier-gated");
  return 0;
}
