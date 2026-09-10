#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <iostream>
#include <string>
#include <vector>

#include "ensemble_fabric/ensemble_fabric.hpp"
#include "ensemble_fabric/local_driver.hpp"

namespace {

using namespace ensemble_fabric;

struct Measurement {
  std::string name;
  std::uint64_t operations{0};
  double milliseconds{0.0};

  void report() const {
    const double per_operation =
        operations == 0u ? 0.0 : (milliseconds * 1000.0) / static_cast<double>(operations);
    std::cout << name << ": " << operations << " completed operation(s) in " << milliseconds
              << " ms (" << per_operation << " us/op)" << std::endl;
  }
};

template <typename Body>
Measurement measure(const std::string& name, std::uint64_t operations, Body body) {
  const auto start = std::chrono::steady_clock::now();
  body();
  const auto finish = std::chrono::steady_clock::now();
  Measurement measurement;
  measurement.name = name;
  measurement.operations = operations;
  measurement.milliseconds =
      std::chrono::duration<double, std::milli>(finish - start).count();
  return measurement;
}

std::shared_ptr<ParticipantBackend> scripted(const std::string& script) {
  return std::make_shared<ReferenceBackend>(std::move(ReferenceProgram::parse(script).value()));
}

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
  return participant;
}

/// Builds a complete specification over p candidates participants.
EnsembleSpec build_spec(std::uint64_t id, std::uint32_t candidates, std::uint32_t judges) {
  EnsembleSpec spec;
  spec.id = EnsembleId(id);
  spec.label = "benchmark";
  spec.task_class = "benchmark";
  std::vector<ParticipantId> stage_participants;
  for (std::uint32_t index = 0; index < candidates; ++index) {
    const ParticipantId participant_id(index + 1u);
    spec.participants.push_back(candidate(participant_id));
    stage_participants.push_back(participant_id);
  }
  for (std::uint32_t index = 0; index < judges; ++index) {
    spec.participants.push_back(judge(ParticipantId(100000u + index)));
  }
  StageSpec stage;
  stage.id = StageId(1);
  stage.label = "primary";
  stage.mode = TopologyMode::parallel;
  stage.participants = stage_participants;
  spec.stages.push_back(stage);
  spec.quorum.min_valid_candidates = 1u;
  spec.arbitration.factors = {ArbitrationFactor::aggregate_score};
  return spec;
}

/// Runs one full ensemble to a committed result.  Returns the number of payload
/// bytes committed, or throws when the operation did not complete: a benchmark
/// must never report a time for work that did not happen.
std::size_t run_one_ensemble(std::uint64_t id, std::uint32_t candidates, std::uint32_t judges) {
  EnsembleFabric fabric;
  LocalEnsembleDriver driver(fabric);
  EnsembleSpec spec = build_spec(id, candidates, judges);
  const Outcome<EnsembleGeneration> generation = driver.define(spec);
  if (!generation.ok()) {
    throw std::runtime_error(std::string("ensemble definition failed: ") +
                             generation.error().message);
  }
  if (!driver.open(EnsembleId(id), generation.value()).ok()) {
    throw std::runtime_error("execution could not be opened");
  }
  std::uint64_t worker = 1u;
  for (const ParticipantSpec& declaration : spec.participants) {
    const bool produces = declaration.roles.contains(ParticipantRole::candidate);
    driver.attach(EnsembleId(id), generation.value(), declaration,
                  scripted(produces ? "ok:value=payload-payload-payload"
                                    : "evaluate:judgment=accept,score=0.5"),
                  WorkerId(worker), WorkerBootId(worker * 31u + 7u));
    ++worker;
  }
  const Status registered = driver.register_all();
  if (!registered.ok()) {
    throw std::runtime_error("participant registration failed: " + registered.error().message);
  }
  const Status ran = driver.run();
  if (!ran.ok()) {
    throw std::runtime_error("ensemble execution failed: " + ran.error().message);
  }
  const Outcome<EnsembleResult> result = driver.fabric().finalize(EnsembleId(id), generation.value());
  if (!result.ok()) {
    throw std::runtime_error("commit failed: " + result.error().message);
  }
  if (!result.value().authoritative()) {
    throw std::runtime_error("the committed result was not authoritative");
  }
  return result.value().payload_bytes;
}

}  // namespace

namespace {

int run_benchmarks(std::uint32_t scale) {
  std::size_t sink = 0;
  std::cout << "Ensemble Fabric benchmarks (scale=" << scale << ")" << std::endl;

  {
    const std::uint64_t iterations = 2000u * scale;
    const Measurement measurement = measure("ensemble definition", iterations, [&]() {
      for (std::uint64_t index = 0; index < iterations; ++index) {
        EnsembleFabric fabric;
        EnsembleSpec spec = build_spec(1u + index, 4u, 1u);
        Outcome<EnsembleGeneration> generation = fabric.define_ensemble(spec);
        sink += generation.ok() ? 1u : 0u;
      }
    });
    measurement.report();
  }

  {
    const std::uint64_t iterations = 300u * scale;
    const Measurement measurement = measure("candidate ingest and arbitration (4 candidates)", iterations,
                                            [&]() {
      for (std::uint64_t index = 0; index < iterations; ++index) {
        sink += run_one_ensemble(1u + index, 4u, 1u);
      }
    });
    measurement.report();
  }

  {
    const std::uint64_t iterations = 60u * scale;
    const Measurement measurement =
        measure("ensemble completion (32 candidates, 4 judges)", iterations, [&]() {
          for (std::uint64_t index = 0; index < iterations; ++index) {
            sink += run_one_ensemble(1u + index, 32u, 4u);
          }
        });
    measurement.report();
  }

  {
    const std::uint64_t iterations = 40u * scale;
    // 48 candidates plus 8 judges stays inside the default participant bound of
    // 64, so every iteration performs the work it reports.
    const Measurement measurement =
        measure("ensemble completion (48 candidates, 8 judges)", iterations, [&]() {
          for (std::uint64_t index = 0; index < iterations; ++index) {
            sink += run_one_ensemble(1u + index, 48u, 8u);
          }
        });
    measurement.report();
  }

  {
    const std::uint64_t iterations = 4000u * scale;
    EnsembleFabric fabric;
    EnsembleSpec spec = build_spec(1u, 16u, 2u);
    (void)fabric.define_ensemble(spec);
    const Measurement measurement = measure("state query (inspection snapshot)", iterations, [&]() {
      for (std::uint64_t index = 0; index < iterations; ++index) {
        Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
        sink += snapshot.ok() ? 1u : 0u;
      }
    });
    measurement.report();
  }

  {
    const std::uint64_t iterations = 500u * scale;
    EnsembleSpec spec = build_spec(1u, 64u, 4u);
    const Measurement measurement = measure("specification encode/decode", iterations, [&]() {
      for (std::uint64_t index = 0; index < iterations; ++index) {
        const std::vector<std::byte> encoded = encode_spec(spec, Limits::defaults());
        Outcome<EnsembleSpec> decoded =
            decode_spec(std::span<const std::byte>(encoded.data(), encoded.size()),
                        Limits::defaults());
        sink += decoded.ok() ? 1u : 0u;
      }
    });
    measurement.report();
  }

  {
    const std::uint64_t iterations = 200u * scale;
    EnsembleFabric fabric;
    EnsembleSpec spec = build_spec(1u, 32u, 2u);
    (void)fabric.define_ensemble(spec);
    const Measurement measurement = measure("state persistence (atomic write)", iterations, [&]() {
      for (std::uint64_t index = 0; index < iterations; ++index) {
        Outcome<std::vector<std::byte>> state = fabric.serialize_state();
        sink += state.ok() ? state.value().size() : 0u;
      }
    });
    measurement.report();
  }

  {
    const std::uint64_t iterations = 20000u * scale;
    Frame frame;
    frame.type = MessageType::heartbeat;
    frame.correlation = RequestId(1);
    const Measurement measurement = measure("protocol frame encode/decode", iterations, [&]() {
      for (std::uint64_t index = 0; index < iterations; ++index) {
        Outcome<std::vector<std::byte>> encoded = encode_frame(frame, Limits::defaults());
        if (encoded.ok()) {
          const DecodeResult decoded =
              decode_frame(std::span<const std::byte>(encoded.value().data(), encoded.value().size()),
                           Limits::defaults());
          sink += decoded.status == DecodeStatus::complete ? 1u : 0u;
        }
      }
    });
    measurement.report();
  }

  std::cout << "(sink=" << sink << ")" << std::endl;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint32_t scale = 1u;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--scale" && index + 1 < argc) {
      scale = static_cast<std::uint32_t>(std::strtoul(argv[++index], nullptr, 10));
    }
  }
  try {
    return run_benchmarks(scale);
  } catch (const std::exception& error) {
    std::cerr << "benchmark did not complete: " << error.what() << std::endl;
    return 1;
  }
}
