#include "example_common.hpp"

/// Fallback: a fallback participant runs only after an explicit, typed trigger
/// condition becomes true, and its result cannot be displaced by a late primary.
int main() {
  using namespace examples;
  EnsembleFabric fabric;
  LocalEnsembleDriver driver(fabric);

  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "fallback";
  spec.task_class = "availability";
  spec.participants.push_back(candidate(ParticipantId(1)));
  spec.participants.push_back(fallback(ParticipantId(5)));
  spec.stages.push_back(stage(StageId(1), {ParticipantId(1)}));
  spec.participants[0].required = true;
  spec.fallback.enabled = true;
  spec.fallback.triggers = {FallbackTrigger::participant_unavailable,
                            FallbackTrigger::retry_exhausted};
  spec.fallback.fallback_participants = {ParticipantId(5)};
  spec.fallback.max_depth = 1u;
  spec.retry.max_attempts = 1u;

  const Outcome<EnsembleGeneration> defined = driver.define(spec);
  const Outcome<EnsembleExecutionId> opened = driver.open(EnsembleId(1), defined.value());
  if (!defined.ok() || !opened.ok()) {
    std::cerr << "setup failed\n";
    return 1;
  }
  LocalParticipant& primary =
      driver.attach(EnsembleId(1), defined.value(), spec.participants[0],
                    scripted("ok:value=primary-answer"), WorkerId(1), WorkerBootId(601));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[1],
                scripted("ok:value=fallback-answer"), WorkerId(5), WorkerBootId(605));
  if (!driver.register_all().ok() || !driver.step().ok()) {
    std::cerr << "execution failed\n";
    return 1;
  }
  std::cout << "primary participant is available; no fallback has been activated"
            << std::endl;
  if (!driver.kill(primary, "example simulated failure").ok()) {
    std::cerr << "kill failed\n";
    return 1;
  }
  if (!driver.run().ok()) {
    std::cerr << "execution failed\n";
    return 1;
  }
  const Outcome<EnsembleResult> result = driver.fabric().finalize(EnsembleId(1), defined.value());
  if (!result.ok()) {
    std::cerr << "finalize failed: " << result.error().message << "\n";
    return 1;
  }
  report(result.value(), "fallback");
  std::cout << "  fallback_used=" << (result.value().fallback_used ? "true" : "false")
            << " depth=" << result.value().fallback_depth << std::endl;
  return 0;
}
