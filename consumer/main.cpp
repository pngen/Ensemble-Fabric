#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <ensemble_fabric/ensemble_fabric.hpp>

namespace {

using namespace ensemble_fabric;

ParticipantSpec candidate(ParticipantId id) {
  ParticipantSpec participant;
  participant.id = id;
  participant.label = "candidate";
  participant.roles.insert(ParticipantRole::candidate);
  return participant;
}

ParticipantSpec judge(ParticipantId id) {
  ParticipantSpec participant;
  participant.id = id;
  participant.label = "judge";
  participant.roles.insert(ParticipantRole::judge);
  participant.required = true;
  participant.domain = "judging";
  return participant;
}

std::shared_ptr<ParticipantBackend> scripted(const std::string& script) {
  Outcome<ReferenceProgram> program = ReferenceProgram::parse(script);
  if (!program.ok()) {
    std::cerr << "consumer: invalid script: " << program.error().message << "\n";
    std::exit(2);
  }
  return std::make_shared<ReferenceBackend>(std::move(program).value());
}

}  // namespace

/// A small but complete downstream ensemble: three candidate participants and
/// one judge.  It constructs the ensemble, registers participants, drives the
/// runtime, arbitrates, and verifies the selected output.
int main() {
  EnsembleFabric fabric;
  LocalEnsembleDriver driver(fabric);

  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "downstream";
  spec.task_class = "downstream-task";
  spec.participants.push_back(candidate(ParticipantId(1)));
  spec.participants.push_back(candidate(ParticipantId(2)));
  spec.participants.push_back(candidate(ParticipantId(3)));
  spec.participants.push_back(judge(ParticipantId(11)));
  StageSpec stage;
  stage.id = StageId(1);
  stage.label = "primary";
  stage.mode = TopologyMode::parallel;
  stage.participants = {ParticipantId(1), ParticipantId(2), ParticipantId(3)};
  spec.stages.push_back(stage);
  spec.quorum.min_valid_candidates = 1u;
  spec.quorum.min_evaluations = 1u;
  spec.quorum.count_optional_participants = false;
  spec.evidence.min_evaluations_per_candidate = 1u;
  spec.aggregation.kind = AggregationKind::judge_arbitration;
  spec.aggregation.require_judge_acceptance = true;
  spec.arbitration.factors = {ArbitrationFactor::judge_acceptance_ratio,
                              ArbitrationFactor::aggregate_score};

  const Outcome<EnsembleGeneration> generation = driver.define(spec);
  if (!generation.ok()) {
    std::cerr << "consumer: define failed: " << generation.error().message << "\n";
    return 1;
  }
  const Outcome<EnsembleExecutionId> opened = driver.open(EnsembleId(1), generation.value());
  if (!opened.ok()) {
    std::cerr << "consumer: open failed: " << opened.error().message << "\n";
    return 1;
  }
  driver.attach(EnsembleId(1), generation.value(), spec.participants[0],
                scripted("ok:value=downstream-alpha"), WorkerId(1), WorkerBootId(11));
  driver.attach(EnsembleId(1), generation.value(), spec.participants[1],
                scripted("ok:value=downstream-beta"), WorkerId(2), WorkerBootId(12));
  driver.attach(EnsembleId(1), generation.value(), spec.participants[2],
                scripted("malformed:bytes=1"), WorkerId(3), WorkerBootId(13));
  driver.attach(EnsembleId(1), generation.value(), spec.participants[3],
                scripted("evaluate:judgment=accept,score=0.75"), WorkerId(11), WorkerBootId(21));

  const Status registered = driver.register_all();
  if (!registered.ok()) {
    std::cerr << "consumer: registration failed: " << registered.error().message << "\n";
    return 1;
  }
  const Status ran = driver.run();
  if (!ran.ok()) {
    std::cerr << "consumer: run failed: " << ran.error().message << "\n";
    return 1;
  }
  const Outcome<EnsembleResult> result =
      driver.fabric().finalize(EnsembleId(1), generation.value());
  if (!result.ok()) {
    std::cerr << "consumer: finalize failed: " << result.error().message << "\n";
    return 1;
  }
  if (!result.value().authoritative()) {
    std::cerr << "consumer: the committed result is not authoritative\n";
    return 1;
  }
  const std::string payload(reinterpret_cast<const char*>(result.value().payload.data()),
                            result.value().payload.size());
  if (payload != "downstream-alpha") {
    std::cerr << "consumer: unexpected selected payload '" << payload << "'\n";
    return 1;
  }
  if (result.value().quorum.state != QuorumState::reached) {
    std::cerr << "consumer: quorum was not reached\n";
    return 1;
  }
  std::cout << "consumer: " << result.value().render() << std::endl;

  const Outcome<EnsembleResult> repeated =
      driver.fabric().finalize(EnsembleId(1), generation.value());
  if (!repeated.ok() ||
      repeated.value().commit_fingerprint != result.value().commit_fingerprint) {
    std::cerr << "consumer: repeated finalize was not idempotent\n";
    return 1;
  }
  return 0;
}
