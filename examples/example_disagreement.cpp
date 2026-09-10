#include "example_common.hpp"

/// Judge disagreement: the runtime reports the disagreement instead of
/// inventing a majority.
int main() {
  using namespace examples;
  EnsembleFabric fabric;
  LocalEnsembleDriver driver(fabric);

  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "disagreement";
  spec.task_class = "review";
  spec.participants.push_back(candidate(ParticipantId(1)));
  spec.participants.push_back(candidate(ParticipantId(2)));
  spec.participants.push_back(judge(ParticipantId(11)));
  spec.participants.push_back(judge(ParticipantId(12)));
  spec.stages.push_back(stage(StageId(1), {ParticipantId(1), ParticipantId(2)}));
  spec.quorum.consensus_threshold_percent = 100u;
  spec.quorum.min_evaluations = 2u;
  spec.arbitration.tie_break = TieBreakPolicy::report_tie;

  const Outcome<EnsembleGeneration> defined = driver.define(spec);
  const Outcome<EnsembleExecutionId> opened = driver.open(EnsembleId(1), defined.value());
  if (!defined.ok() || !opened.ok()) {
    std::cerr << "setup failed\n";
    return 1;
  }
  driver.attach(EnsembleId(1), defined.value(), spec.participants[0],
                scripted("ok:value=answer-a"), WorkerId(1), WorkerBootId(701));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[1],
                scripted("ok:value=answer-b"), WorkerId(2), WorkerBootId(702));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[2],
                scripted("evaluate:judgment=accept,score=0.9;evaluate:judgment=reject,score=0.1"),
                WorkerId(11), WorkerBootId(711));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[3],
                scripted("evaluate:judgment=reject,score=0.1;evaluate:judgment=accept,score=0.9"),
                WorkerId(12), WorkerBootId(712));
  if (!driver.register_all().ok() || !driver.run().ok()) {
    std::cerr << "execution failed\n";
    return 1;
  }
  const Outcome<EnsembleResult> result = driver.fabric().finalize(EnsembleId(1), defined.value());
  const Outcome<EnsembleResult> stored = driver.fabric().current_result(EnsembleId(1));
  if (stored.ok()) {
    report(stored.value(), "disagreement");
    std::cout << "  authoritative=" << (stored.value().authoritative() ? "true" : "false")
              << std::endl;
    std::cout << "  finalize returned: "
              << (result.ok() ? "success" : to_string(result.code())) << std::endl;
  } else {
    std::cout << "disagreement: no terminal record (" << stored.error().message << ")"
              << std::endl;
  }
  return 0;
}
