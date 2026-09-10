#include "example_common.hpp"

/// Cancellation: a cancelled execution never publishes success, and a late
/// participant result is fenced with a typed rejection.
int main() {
  using namespace examples;
  EnsembleFabric fabric;
  LocalEnsembleDriver driver(fabric);

  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "cancellation";
  spec.task_class = "long-running";
  spec.participants.push_back(candidate(ParticipantId(1)));
  spec.participants.push_back(candidate(ParticipantId(2)));
  spec.stages.push_back(stage(StageId(1), {ParticipantId(1), ParticipantId(2)}));

  const Outcome<EnsembleGeneration> defined = driver.define(spec);
  const Outcome<EnsembleExecutionId> opened = driver.open(EnsembleId(1), defined.value());
  if (!defined.ok() || !opened.ok()) {
    std::cerr << "setup failed\n";
    return 1;
  }
  driver.attach(EnsembleId(1), defined.value(), spec.participants[0],
                scripted("ok:value=first"), WorkerId(1), WorkerBootId(801));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[1],
                scripted("ok:value=second"), WorkerId(2), WorkerBootId(802));
  if (!driver.register_all().ok()) {
    std::cerr << "registration failed\n";
    return 1;
  }
  // Dispatch one branch, then cancel while work is nominally in flight.
  if (!driver.step().ok()) {
    std::cerr << "step failed\n";
    return 1;
  }
  const Status cancelled =
      driver.fabric().cancel_ensemble(EnsembleId(1), defined.value(), "operator requested stop");
  if (!cancelled.ok()) {
    std::cerr << "cancel failed: " << cancelled.error().message << "\n";
    return 1;
  }
  if (!driver.run().ok()) {
    std::cerr << "drain failed\n";
    return 1;
  }
  const Outcome<EnsembleResult> result = driver.fabric().finalize(EnsembleId(1), defined.value());
  std::cout << "cancellation: finalize returned " << to_string(result.code()) << std::endl;
  const Outcome<EnsembleSnapshot> snapshot = driver.fabric().inspect(EnsembleId(1));
  if (snapshot.ok()) {
    std::cout << "  execution status: " << to_string(snapshot.value().execution_status)
              << std::endl;
    std::cout << "  has_result=" << (snapshot.value().has_result ? "true" : "false") << std::endl;
  }
  return result.ok() ? 1 : 0;
}
