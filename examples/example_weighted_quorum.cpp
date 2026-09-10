#include "example_common.hpp"

/// Weighted quorum: the denominator and the consensus threshold are explicit,
/// and abstentions are reported rather than discarded.
int main() {
  using namespace examples;
  EnsembleFabric fabric;
  LocalEnsembleDriver driver(fabric);

  EnsembleSpec spec;
  spec.id = EnsembleId(1);
  spec.label = "weighted-quorum";
  spec.task_class = "voting";
  spec.participants.push_back(candidate(ParticipantId(1)));
  spec.participants.push_back(candidate(ParticipantId(2)));
  spec.participants.push_back(judge(ParticipantId(11)));
  spec.participants.push_back(judge(ParticipantId(12)));
  spec.participants.push_back(judge(ParticipantId(13)));
  spec.stages.push_back(stage(StageId(1), {ParticipantId(1), ParticipantId(2)}));
  spec.aggregation.kind = AggregationKind::weighted_vote;
  spec.aggregation.use_weights = true;
  spec.quorum.vote_basis = VoteBasis::required_participants;
  spec.quorum.min_participants = 3u;
  spec.quorum.consensus_threshold_percent = 60u;
  spec.quorum.count_abstentions = true;
  spec.arbitration.factors = {ArbitrationFactor::weighted_vote};

  const Outcome<EnsembleGeneration> defined = driver.define(spec);
  const Outcome<EnsembleExecutionId> opened = driver.open(EnsembleId(1), defined.value());
  if (!defined.ok() || !opened.ok()) {
    std::cerr << "setup failed\n";
    return 1;
  }
  driver.attach(EnsembleId(1), defined.value(), spec.participants[0],
                scripted("ok:value=option-x"), WorkerId(1), WorkerBootId(301));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[1],
                scripted("ok:value=option-y"), WorkerId(2), WorkerBootId(302));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[2],
                scripted("evaluate:judgment=accept,score=9,weight=5"), WorkerId(11),
                WorkerBootId(311));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[3],
                scripted("evaluate:judgment=accept,score=4,weight=1"), WorkerId(12),
                WorkerBootId(312));
  driver.attach(EnsembleId(1), defined.value(), spec.participants[4],
                scripted("evaluate:judgment=abstain,score=0"), WorkerId(13), WorkerBootId(313));
  if (!driver.register_all().ok() || !driver.run().ok()) {
    std::cerr << "execution failed\n";
    return 1;
  }
  const Outcome<EnsembleResult> result = driver.fabric().finalize(EnsembleId(1), defined.value());
  if (!result.ok()) {
    std::cerr << "finalize failed: " << result.error().message << "\n";
    return 1;
  }
  report(result.value(), "weighted-quorum");
  return 0;
}
