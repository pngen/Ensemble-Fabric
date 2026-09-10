#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ensemble_fabric/candidate.hpp"
#include "ensemble_fabric/ids.hpp"
#include "ensemble_fabric/outcome.hpp"
#include "ensemble_fabric/participant.hpp"
#include "ensemble_fabric/protocol.hpp"
#include "ensemble_fabric/role.hpp"

namespace ensemble_fabric {

/// Result a participant backend produces for one dispatched candidate slot.
///
/// A backend can produce content, abstain, or report a typed failure.  All
/// three are legitimate participant outcomes; only content can become a
/// candidate, and only an abstraction is ever a replacement for content.
struct ParticipantProduction {
  enum class Kind : std::uint8_t {
    content = 1,
    abstention = 2,
    failure = 3,
  };

  Kind kind{Kind::content};
  std::vector<std::byte> payload;
  ParticipantFailureKind failure{ParticipantFailureKind::internal_error};
  std::string detail;
  std::uint32_t budget_units_used{0};
};

/// Result a participant backend produces for one evaluation request.
struct ParticipantEvaluation {
  VerificationState verification{VerificationState::unknown};
  CategoricalJudgment judgment{CategoricalJudgment::unknown};
  bool score_defined{false};
  double score{0.0};
  double weight{1.0};
  std::vector<std::byte> evidence;
  std::string rationale;
};

/// The interface a participant process implements.
///
/// Ensemble Fabric governs *which* execution may happen, *which* evidence counts,
/// and *which* result becomes authoritative.  It does not own inference: the
/// backend behind this interface may be a local model runtime, a remote
/// endpoint, or the deterministic reference backend.
class ParticipantBackend {
 public:
  virtual ~ParticipantBackend();
  ParticipantBackend(const ParticipantBackend&) = delete;
  ParticipantBackend& operator=(const ParticipantBackend&) = delete;
  ParticipantBackend() = default;

  [[nodiscard]] virtual Outcome<ParticipantProduction> produce(
      const DispatchCandidateMessage& request) = 0;
  [[nodiscard]] virtual Outcome<ParticipantEvaluation> evaluate(
      const EvaluationRequestMessage& request) = 0;
};

/// Step kinds of the deterministic reference/synthetic model backend.
///
/// This backend is explicitly synthetic: it emulates model participants with
/// scripted, replayable behaviour so that ensemble semantics can be tested
/// without a proprietary model API.  It is never evidence of real model quality.
enum class ReferenceStepKind : std::uint8_t {
  produce_ok = 1,
  produce_failure = 2,
  produce_abstain = 3,
  produce_malformed = 4,
  evaluate = 5,
  wait_gate = 6,
  die = 7,
};

[[nodiscard]] std::string_view to_string(ReferenceStepKind kind) noexcept;

struct ReferenceStep {
  ReferenceStepKind kind{ReferenceStepKind::produce_ok};
  std::string value;
  std::string gate;
  ParticipantFailureKind failure{ParticipantFailureKind::internal_error};
  VerificationState verification{VerificationState::unknown};
  CategoricalJudgment judgment{CategoricalJudgment::unknown};
  bool score_defined{false};
  double score{0.0};
  double weight{1.0};
  std::string rationale;
  std::uint32_t malformed_bytes{0};

  [[nodiscard]] std::string render() const;
};

/// A deterministic, replayable script.  Parsing is strict: unknown step kinds,
/// unknown keys, and out-of-range values are rejected with a typed error.
struct ReferenceProgram {
  std::vector<ReferenceStep> steps;
  std::uint64_t seed{0};

  [[nodiscard]] static Outcome<ReferenceProgram> parse(std::string_view script);

  /// Repeat-the-last-step behaviour: when the program runs out of steps the
  /// final step is reused, which keeps long-running scripts short without
  /// making behaviour nondeterministic.
  [[nodiscard]] ReferenceStep step_at(std::size_t index) const;
  [[nodiscard]] std::string render() const;
};

/// Deterministic reference backend.  Every decision is a pure function of the
/// program, the request identity, and the program step index, so replaying the
/// same script reproduces the same evidence exactly.
class ReferenceBackend final : public ParticipantBackend {
 public:
  explicit ReferenceBackend(ReferenceProgram program);
  ~ReferenceBackend() override;

  ReferenceBackend(const ReferenceBackend&) = delete;
  ReferenceBackend& operator=(const ReferenceBackend&) = delete;

  [[nodiscard]] Outcome<ParticipantProduction> produce(
      const DispatchCandidateMessage& request) override;
  [[nodiscard]] Outcome<ParticipantEvaluation> evaluate(
      const EvaluationRequestMessage& request) override;

  [[nodiscard]] std::size_t steps_consumed() const noexcept { return step_index_; }
  [[nodiscard]] const ReferenceProgram& program() const noexcept { return program_; }

 private:
  [[nodiscard]] ReferenceStep consume();

  ReferenceProgram program_;
  std::size_t step_index_{0};
};

/// Blocks until the gate file exists, yielding the processor.  Used by
/// cross-process tests to synchronize deterministically without sleeping.
void await_gate(const std::string& path);

}  // namespace ensemble_fabric
