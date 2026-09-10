#include "example_common.hpp"

/// Retry with participant replacement: a retry advances the logical
/// participant generation, so evidence produced by the previous incarnation can
/// never be attributed to the replacement.
int main() {
  using namespace examples;
  EnsembleFabric fabric;
  LocalEnsembleDriver driver(fabric);

  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "retry-replacement";
  spec.task_class = "unreliable";
  spec.participants.push_back(candidate(ParticipantId(1)));
  spec.stages.push_back(stage(StageId(1), {ParticipantId(1)}));
  spec.retry.max_attempts = 3u;
  spec.retry.retry_on_malformed_candidate = true;
  spec.retry.retry_on_unavailable = true;
  spec.retry.replace_participant = true;

  const Outcome<EnsembleGeneration> defined = driver.define(spec);
  const Outcome<EnsembleExecutionId> opened = driver.open(EnsembleId(1), defined.value());
  if (!defined.ok() || !opened.ok()) {
    std::cerr << "setup failed\n";
    return 1;
  }
  (void)driver.attach(EnsembleId(1), defined.value(), spec.participants[0],
                      scripted("malformed:bytes=2;ok:value=recovered-answer"), WorkerId(1),
                      WorkerBootId(901));
  if (!driver.register_all().ok() || !driver.run().ok()) {
    std::cerr << "execution failed\n";
    return 1;
  }
  const Outcome<EnsembleSnapshot> snapshot = driver.fabric().inspect(EnsembleId(1));
  if (snapshot.ok() && !snapshot.value().candidates.empty()) {
    std::cout << "retry: attempts_used=" << snapshot.value().candidates.front().attempts_used
              << " candidate_generation=" << snapshot.value().candidates.front().generation.to_string()
              << " state=" << to_string(snapshot.value().candidates.front().state) << std::endl;
  }
  const Outcome<EnsembleResult> result = driver.fabric().finalize(EnsembleId(1), defined.value());
  if (!result.ok()) {
    std::cerr << "finalize failed: " << result.error().message << "\n";
    return 1;
  }
  report(result.value(), "retry-replacement");
  return 0;
}
