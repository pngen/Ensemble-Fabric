#include "ensemble_fabric/reference_backend.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <utility>

#include "detail/text.hpp"

namespace ensemble_fabric {

std::string_view to_string(ReferenceStepKind kind) noexcept {
  switch (kind) {
    case ReferenceStepKind::produce_ok: return "ok";
    case ReferenceStepKind::produce_failure: return "fail";
    case ReferenceStepKind::produce_abstain: return "abstain";
    case ReferenceStepKind::produce_malformed: return "malformed";
    case ReferenceStepKind::evaluate: return "evaluate";
    case ReferenceStepKind::wait_gate: return "gate";
    case ReferenceStepKind::die: return "die";
  }
  return "unknown";
}

std::string ReferenceStep::render() const {
  std::string out(ensemble_fabric::to_string(kind));
  out.push_back(':');
  switch (kind) {
    case ReferenceStepKind::produce_ok:
      out.append("value=").append(value);
      break;
    case ReferenceStepKind::produce_failure:
      out.append("kind=").append(ensemble_fabric::to_string(failure));
      break;
    case ReferenceStepKind::produce_abstain:
      out.append("rationale=").append(rationale);
      break;
    case ReferenceStepKind::produce_malformed:
      out.append("bytes=").append(std::to_string(malformed_bytes));
      break;
    case ReferenceStepKind::evaluate:
      out.append("judgment=").append(ensemble_fabric::to_string(judgment));
      out.append(",verification=").append(ensemble_fabric::to_string(verification));
      if (score_defined) {
        out.append(",score=").append(detail::format_double(score));
      }
      out.append(",weight=").append(detail::format_double(weight));
      out.append(",rationale=").append(rationale);
      break;
    case ReferenceStepKind::wait_gate:
      out.append("gate=").append(gate);
      break;
    case ReferenceStepKind::die:
      out.append("value=immediate");
      break;
  }
  return out;
}

namespace {

[[nodiscard]] Outcome<ReferenceStepKind> parse_step_kind(std::string_view text) {
  if (text == "ok" || text == "produce_ok") return ReferenceStepKind::produce_ok;
  if (text == "fail" || text == "produce_failure") return ReferenceStepKind::produce_failure;
  if (text == "abstain" || text == "produce_abstain") return ReferenceStepKind::produce_abstain;
  if (text == "malformed" || text == "produce_malformed") return ReferenceStepKind::produce_malformed;
  if (text == "evaluate") return ReferenceStepKind::evaluate;
  if (text == "gate" || text == "wait_gate") return ReferenceStepKind::wait_gate;
  if (text == "die") return ReferenceStepKind::die;
  return fail<ReferenceStepKind>(EnsembleError::invalid_argument,
                                 "reference script: unknown step kind '" + std::string(text) + "'");
}

[[nodiscard]] std::vector<std::string> split(std::string_view text, char separator) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (std::size_t index = 0; index <= text.size(); ++index) {
    if (index == text.size() || text[index] == separator) {
      parts.emplace_back(text.substr(start, index - start));
      start = index + 1u;
    }
  }
  return parts;
}

[[nodiscard]] std::string trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
    ++begin;
  }
  while (end > begin && (text[end - 1u] == ' ' || text[end - 1u] == '\t')) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

}  // namespace

Outcome<ReferenceProgram> ReferenceProgram::parse(std::string_view script) {
  ReferenceProgram program;
  if (script.empty()) {
    ReferenceStep step;
    step.kind = ReferenceStepKind::produce_ok;
    step.value = "reference";
    program.steps.push_back(std::move(step));
    return program;
  }
  if (script.size() > 64u * 1024u) {
    return fail<ReferenceProgram>(EnsembleError::resource_limit_exceeded,
                                  "reference script exceeds the configured bound");
  }
  const std::vector<std::string> raw_steps = split(script, ';');
  for (const std::string& raw : raw_steps) {
    const std::string step_text = trim(raw);
    if (step_text.empty()) {
      continue;
    }
    if (step_text.rfind("seed=", 0) == 0u) {
      std::uint64_t seed = 0;
      if (!detail::parse_u64(step_text.substr(5u), seed)) {
        return fail<ReferenceProgram>(EnsembleError::invalid_argument,
                                      "reference script: seed must be an unsigned integer");
      }
      program.seed = seed;
      continue;
    }
    const std::size_t colon = step_text.find(':');
    const std::string head = colon == std::string::npos ? step_text : step_text.substr(0, colon);
    Outcome<ReferenceStepKind> kind = parse_step_kind(head);
    if (!kind.ok()) {
      return fail<ReferenceProgram>(kind.code(), kind.error().message);
    }
    ReferenceStep step;
    step.kind = kind.value();
    if (colon != std::string::npos) {
      const std::vector<std::string> assignments = split(step_text.substr(colon + 1u), ',');
      for (const std::string& assignment : assignments) {
        if (assignment.empty()) {
          continue;
        }
        const std::size_t equals = assignment.find('=');
        if (equals == std::string::npos) {
          return fail<ReferenceProgram>(EnsembleError::invalid_argument,
                                        "reference script: expected key=value in '" + assignment + "'");
        }
        const std::string key = trim(assignment.substr(0, equals));
        const std::string raw_value = trim(assignment.substr(equals + 1u));
        if (key == "value") {
          step.value = raw_value;
        } else if (key == "gate") {
          step.gate = raw_value;
        } else if (key == "kind") {
          const std::optional<ParticipantFailureKind> parsed = parse_failure_kind(raw_value);
          if (!parsed.has_value()) {
            return fail<ReferenceProgram>(EnsembleError::invalid_enum_value,
                                          "reference script: unknown failure kind '" + raw_value + "'");
          }
          step.failure = parsed.value();
        } else if (key == "verification") {
          const std::optional<VerificationState> parsed = parse_verification_state(raw_value);
          if (!parsed.has_value()) {
            return fail<ReferenceProgram>(EnsembleError::invalid_enum_value,
                                          "reference script: unknown verification state '" + raw_value + "'");
          }
          step.verification = parsed.value();
        } else if (key == "judgment") {
          const std::optional<CategoricalJudgment> parsed = parse_categorical_judgment(raw_value);
          if (!parsed.has_value()) {
            return fail<ReferenceProgram>(EnsembleError::invalid_enum_value,
                                          "reference script: unknown judgment '" + raw_value + "'");
          }
          step.judgment = parsed.value();
        } else if (key == "score") {
          double score = 0.0;
          if (!detail::parse_finite_double(raw_value, score)) {
            return fail<ReferenceProgram>(EnsembleError::invalid_argument,
                                          "reference script: score must be a finite number");
          }
          step.score = score;
          step.score_defined = true;
        } else if (key == "weight") {
          double weight = 0.0;
          if (!detail::parse_finite_double(raw_value, weight) || weight <= 0.0) {
            return fail<ReferenceProgram>(EnsembleError::invalid_argument,
                                          "reference script: weight must be a positive number");
          }
          step.weight = weight;
        } else if (key == "rationale") {
          step.rationale = raw_value;
        } else if (key == "bytes") {
          std::uint32_t bytes = 0;
          if (!detail::parse_u32(raw_value, bytes) || bytes > (1u << 20)) {
            return fail<ReferenceProgram>(EnsembleError::invalid_argument,
                                          "reference script: bytes must be a bounded unsigned integer");
          }
          step.malformed_bytes = bytes;
        } else {
          return fail<ReferenceProgram>(EnsembleError::invalid_argument,
                                        "reference script: unknown key '" + key + "'");
        }
      }
    }
    program.steps.push_back(std::move(step));
  }
  if (program.steps.empty()) {
    return fail<ReferenceProgram>(EnsembleError::invalid_argument,
                                  "reference script declares no steps");
  }
  return program;
}

ReferenceStep ReferenceProgram::step_at(std::size_t index) const {
  if (steps.empty()) {
    return ReferenceStep{};
  }
  // The last step repeats.  That keeps long-running scripts short without
  // introducing nondeterminism: the same index always yields the same step.
  const std::size_t effective = std::min(index, steps.size() - 1u);
  return steps[effective];
}

std::string ReferenceProgram::render() const {
  std::string out = "seed=" + std::to_string(seed);
  for (const ReferenceStep& step : steps) {
    out.push_back(';');
    out.append(step.render());
  }
  return out;
}

void await_gate(const std::string& path) {
  if (path.empty()) {
    return;
  }
  const std::filesystem::path gate(path);
  std::error_code error;
  for (;;) {
    if (std::filesystem::exists(gate, error)) {
      return;
    }
    std::this_thread::yield();
  }
}

ParticipantBackend::~ParticipantBackend() = default;

ReferenceBackend::ReferenceBackend(ReferenceProgram program) : program_(std::move(program)) {}

ReferenceBackend::~ReferenceBackend() = default;

ReferenceStep ReferenceBackend::consume() {
  for (;;) {
    const ReferenceStep step = program_.step_at(step_index_);
    ++step_index_;
    if (step.kind == ReferenceStepKind::wait_gate) {
      await_gate(step.gate);
      continue;
    }
    return step;
  }
}

Outcome<ParticipantProduction> ReferenceBackend::produce(const DispatchCandidateMessage& request) {
  const ReferenceStep step = consume();
  ParticipantProduction production;
  switch (step.kind) {
    case ReferenceStepKind::die:
      // Immediate process termination without unwinding: this is how the
      // reference deployment exercises real worker death.  It never raises an
      // abort dialog.
      std::_Exit(70);
      break;
    case ReferenceStepKind::produce_ok: {
      std::string value = step.value;
      if (value.empty()) {
        value = "candidate-" + request.candidate.to_string() + "-attempt-" +
                std::to_string(request.attempt);
      }
      production.kind = ParticipantProduction::Kind::content;
      production.payload.assign(reinterpret_cast<const std::byte*>(value.data()),
                                reinterpret_cast<const std::byte*>(value.data()) + value.size());
      production.budget_units_used = request.budget_units;
      return production;
    }
    case ReferenceStepKind::produce_abstain:
      production.kind = ParticipantProduction::Kind::abstention;
      production.detail = step.rationale;
      return production;
    case ReferenceStepKind::produce_malformed:
      production.kind = ParticipantProduction::Kind::failure;
      production.failure = ParticipantFailureKind::malformed_output;
      production.detail = "reference backend produced malformed output (" +
                          std::to_string(step.malformed_bytes) + " bytes requested)";
      return production;
    case ReferenceStepKind::produce_failure:
      production.kind = ParticipantProduction::Kind::failure;
      production.failure = step.failure;
      production.detail = "reference backend reported " +
                          std::string(ensemble_fabric::to_string(step.failure));
      return production;
    case ReferenceStepKind::evaluate:
      // An evaluation step in a production slot is a script error, reported as
      // a typed failure rather than silently producing content.
      production.kind = ParticipantProduction::Kind::failure;
      production.failure = ParticipantFailureKind::internal_error;
      production.detail = "reference script supplied an evaluation step for a production slot";
      return production;
    case ReferenceStepKind::wait_gate:
      break;
  }
  production.kind = ParticipantProduction::Kind::failure;
  production.failure = ParticipantFailureKind::internal_error;
  production.detail = "reference backend reached an unreachable state";
  return production;
}

Outcome<ParticipantEvaluation> ReferenceBackend::evaluate(const EvaluationRequestMessage& request) {
  const ReferenceStep step = consume();
  ParticipantEvaluation evaluation;
  switch (step.kind) {
    case ReferenceStepKind::die:
      std::_Exit(70);
      break;
    case ReferenceStepKind::evaluate:
      evaluation.verification = step.verification;
      evaluation.judgment = step.judgment;
      evaluation.score_defined = step.score_defined;
      evaluation.score = step.score;
      evaluation.weight = step.weight;
      evaluation.rationale = step.rationale;
      if (evaluation.rationale.empty()) {
        evaluation.rationale = "reference evaluation of candidate " +
                               request.candidate.to_string();
      }
      return evaluation;
    case ReferenceStepKind::produce_abstain:
      evaluation.judgment = CategoricalJudgment::abstain;
      evaluation.verification = VerificationState::abstain;
      evaluation.rationale = step.rationale.empty() ? "abstained" : step.rationale;
      return evaluation;
    case ReferenceStepKind::produce_ok:
      // A production step reused for an evaluation slot is accepted for the
      // default judge case: ACCEPT with an undefined score.  This keeps short
      // scripts usable while remaining explicit.
      evaluation.judgment = CategoricalJudgment::accept;
      evaluation.verification = VerificationState::pass;
      evaluation.rationale = "reference default acceptance";
      return evaluation;
    case ReferenceStepKind::produce_failure:
    case ReferenceStepKind::produce_malformed:
    case ReferenceStepKind::wait_gate:
      break;
  }
  return fail<ParticipantEvaluation>(EnsembleError::malformed_evaluation,
                                     "reference script supplied no evaluation step");
}

}  // namespace ensemble_fabric
