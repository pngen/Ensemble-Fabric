#include "ensemble_fabric/arbitration.hpp"

#include <algorithm>
#include <cmath>

#include "detail/text.hpp"

namespace ensemble_fabric {

std::string_view to_string(EliminationReason reason) noexcept {
  switch (reason) {
    case EliminationReason::none: return "NONE";
    case EliminationReason::not_a_content_state: return "NOT_A_CONTENT_STATE";
    case EliminationReason::invalid_candidate: return "INVALID_CANDIDATE";
    case EliminationReason::failed_attempt: return "FAILED_ATTEMPT";
    case EliminationReason::abstained: return "ABSTAINED";
    case EliminationReason::stale_generation: return "STALE_GENERATION";
    case EliminationReason::superseded: return "SUPERSEDED";
    case EliminationReason::cancelled: return "CANCELLED";
    case EliminationReason::participant_not_authoritative: return "PARTICIPANT_NOT_AUTHORITATIVE";
    case EliminationReason::incompatible_participant: return "INCOMPATIBLE_PARTICIPANT";
    case EliminationReason::verifier_failed: return "VERIFIER_FAILED";
    case EliminationReason::hard_predicate_failed: return "HARD_PREDICATE_FAILED";
    case EliminationReason::missing_required_evidence: return "MISSING_REQUIRED_EVIDENCE";
    case EliminationReason::below_minimum_score: return "BELOW_MINIMUM_SCORE";
    case EliminationReason::insufficient_judge_acceptance: return "INSUFFICIENT_JUDGE_ACCEPTANCE";
    case EliminationReason::not_selectable_role: return "NOT_SELECTABLE_ROLE";
    case EliminationReason::lost_ranking: return "LOST_RANKING";
    case EliminationReason::tie_lost: return "TIE_LOST";
    case EliminationReason::fallback_not_selected: return "FALLBACK_NOT_SELECTED";
  }
  return "UNKNOWN_REASON";
}

std::string render_factors(const std::vector<ArbitrationFactor>& factors) {
  if (factors.empty()) {
    return "aggregate_score";
  }
  std::vector<std::string> parts;
  parts.reserve(factors.size());
  for (const ArbitrationFactor factor : factors) {
    parts.emplace_back(ensemble_fabric::to_string(factor));
  }
  return detail::join(parts, ">");
}

namespace {

/// True when a larger value is better for this factor.  Cost and latency are
/// costs, not scores: they rank ascending.
[[nodiscard]] bool higher_is_better(ArbitrationFactor factor) noexcept {
  switch (factor) {
    case ArbitrationFactor::execution_cost:
    case ArbitrationFactor::latency:
      return false;
    default:
      return true;
  }
}

[[nodiscard]] double factor_value(ArbitrationFactor factor,
                                  const ArbitrationCandidateInput& candidate) noexcept {
  const CandidateEvaluationSummary& evaluation = candidate.evaluation;
  switch (factor) {
    case ArbitrationFactor::judge_acceptance_ratio: {
      const std::uint32_t decided = evaluation.accepts + evaluation.rejects;
      return decided == 0u ? 0.0
                           : static_cast<double>(evaluation.accepts) / static_cast<double>(decided);
    }
    case ArbitrationFactor::verifier_state: {
      if (evaluation.verifier_fail > 0u) {
        return 0.0;
      }
      if (evaluation.verifier_pass > 0u) {
        return 2.0;
      }
      if (evaluation.verifier_abstain > 0u) {
        return 0.5;
      }
      return 0.0;
    }
    case ArbitrationFactor::vote_count:
      return static_cast<double>(evaluation.accepts);
    case ArbitrationFactor::weighted_vote:
      return evaluation.weighted_score_sum > 0.0 ? evaluation.weighted_score_sum : 0.0;
    case ArbitrationFactor::aggregate_score:
      return evaluation.mean_score();
    case ArbitrationFactor::min_score:
      return evaluation.evaluations == 0u ? 0.0 : evaluation.min_score;
    case ArbitrationFactor::confidence:
      return evaluation.evaluations == 0u
                 ? 0.0
                 : static_cast<double>(evaluation.accepts) /
                       static_cast<double>(evaluation.evaluations);
    case ArbitrationFactor::participant_priority:
      return static_cast<double>(candidate.participant_priority);
    case ArbitrationFactor::specialist_relevance:
      return static_cast<double>(candidate.specialist_matches);
    case ArbitrationFactor::execution_cost:
      return static_cast<double>(candidate.execution_cost);
    case ArbitrationFactor::latency:
      return static_cast<double>(candidate.latency);
    case ArbitrationFactor::provenance_quality:
      return candidate.participant_authoritative ? 1.0 : 0.0;
    case ArbitrationFactor::fallback_status:
      return candidate.fallback ? 1.0 : 0.0;
  }
  return 0.0;
}

[[nodiscard]] std::string factor_detail(ArbitrationFactor factor,
                                        const ArbitrationCandidateInput& candidate) {
  switch (factor) {
    case ArbitrationFactor::judge_acceptance_ratio:
      return "accepts=" + std::to_string(candidate.evaluation.accepts) +
             " rejects=" + std::to_string(candidate.evaluation.rejects);
    case ArbitrationFactor::verifier_state:
      return "pass=" + std::to_string(candidate.evaluation.verifier_pass) +
             " fail=" + std::to_string(candidate.evaluation.verifier_fail) +
             " abstain=" + std::to_string(candidate.evaluation.verifier_abstain) +
             " unknown=" + std::to_string(candidate.evaluation.verifier_unknown);
    case ArbitrationFactor::vote_count:
      return "accepts=" + std::to_string(candidate.evaluation.accepts);
    case ArbitrationFactor::weighted_vote:
      return "weighted_score_sum=" + detail::format_double(candidate.evaluation.weighted_score_sum);
    case ArbitrationFactor::aggregate_score:
      return "mean_score=" + detail::format_double(candidate.evaluation.mean_score()) +
             " evaluations=" + std::to_string(candidate.evaluation.evaluations);
    case ArbitrationFactor::min_score:
      return "min_score=" + detail::format_double(candidate.evaluation.min_score);
    case ArbitrationFactor::confidence:
      return "confidence_inputs=" + std::to_string(candidate.evaluation.evaluations);
    case ArbitrationFactor::participant_priority:
      return "participant_priority=" + std::to_string(candidate.participant_priority);
    case ArbitrationFactor::specialist_relevance:
      return "domain_matches=" + std::to_string(candidate.specialist_matches);
    case ArbitrationFactor::execution_cost:
      return "budget_units=" + std::to_string(candidate.execution_cost);
    case ArbitrationFactor::latency:
      return "logical_latency=" + std::to_string(candidate.latency);
    case ArbitrationFactor::provenance_quality:
      return candidate.participant_authoritative ? "authoritative" : "not_authoritative";
    case ArbitrationFactor::fallback_status:
      return candidate.fallback ? "fallback" : "primary";
  }
  return std::string();
}

/// Compares two candidates on one factor.  Returns true when lhs ranks ahead.
[[nodiscard]] bool factor_precedes(ArbitrationFactor factor,
                                   const ArbitrationCandidateInput& lhs,
                                   const ArbitrationCandidateInput& rhs) noexcept {
  const double left = factor_value(factor, lhs);
  const double right = factor_value(factor, rhs);
  if (left == right) {
    return false;
  }
  return higher_is_better(factor) ? left > right : left < right;
}

[[nodiscard]] bool factor_ties(ArbitrationFactor factor, const ArbitrationCandidateInput& lhs,
                               const ArbitrationCandidateInput& rhs) noexcept {
  return factor_value(factor, lhs) == factor_value(factor, rhs);
}

struct Eligibility {
  bool eligible{false};
  EliminationReason reason{EliminationReason::none};
  std::string detail;
};

[[nodiscard]] Eligibility check_eligibility(const ArbitrationCandidateInput& candidate,
                                            const ArbitrationInputs& inputs,
                                            bool verifier_gating, bool judge_gating) {
  Eligibility result;
  const CandidateEvaluationSummary& evaluation = candidate.evaluation;

  if (!candidate.generation_current) {
    result.reason = EliminationReason::stale_generation;
    result.detail = "candidate generation is no longer current";
    return result;
  }
  switch (candidate.state) {
    case CandidateState::superseded:
      result.reason = EliminationReason::superseded;
      result.detail = "candidate was superseded by a newer generation";
      return result;
    case CandidateState::cancelled:
      result.reason = EliminationReason::cancelled;
      result.detail = "candidate was cancelled";
      return result;
    case CandidateState::failed:
      result.reason = EliminationReason::failed_attempt;
      result.detail = "candidate attempt failed";
      return result;
    case CandidateState::abstained:
      result.reason = EliminationReason::abstained;
      result.detail = "participant abstained and produced no content";
      return result;
    case CandidateState::invalid:
      result.reason = EliminationReason::invalid_candidate;
      result.detail = "candidate did not pass validation";
      return result;
    default:
      break;
  }
  if (!is_content_state(candidate.state)) {
    result.reason = EliminationReason::not_a_content_state;
    result.detail = "candidate has not produced content yet";
    return result;
  }
  if (!candidate.participant_authoritative) {
    result.reason = EliminationReason::participant_not_authoritative;
    result.detail = "producing participant is not authoritative in the current epoch";
    return result;
  }
  if (!candidate.compatible) {
    result.reason = EliminationReason::incompatible_participant;
    result.detail = "producing participant is incompatible with the ensemble requirement";
    return result;
  }
  if (!candidate.role_selectable) {
    result.reason = EliminationReason::not_selectable_role;
    result.detail = "participant role may not become the final answer under this policy";
    return result;
  }
  if (!inputs.evidence.mandatory_criteria.empty()) {
    if (evaluation.hard_failures > 0u) {
      result.reason = EliminationReason::hard_predicate_failed;
      result.detail = "failed " + std::to_string(evaluation.hard_failures) +
                      " mandatory validation predicate(s)";
      return result;
    }
    if (evaluation.hard_unknowns > 0u && inputs.evidence.unknown_verification_is_failure) {
      result.reason = EliminationReason::hard_predicate_failed;
      result.detail = "mandatory validation predicate returned UNKNOWN, which this policy treats "
                      "as failure";
      return result;
    }
    if (evaluation.hard_predicates < inputs.evidence.mandatory_criteria.size()) {
      result.reason = EliminationReason::missing_required_evidence;
      result.detail = "missing evidence for " +
                      std::to_string(inputs.evidence.mandatory_criteria.size() -
                                     evaluation.hard_predicates) +
                      " mandatory validation predicate(s)";
      return result;
    }
  }
  if (verifier_gating) {
    // Verification gating is strict: only an explicit PASS admits a candidate.
    // UNKNOWN, ABSTAIN, and missing verification never become PASS.
    if (evaluation.verifier_fail > 0u) {
      result.reason = EliminationReason::verifier_failed;
      result.detail = "a verifier returned FAIL";
      return result;
    }
    if (evaluation.verifier_pass == 0u) {
      result.reason = EliminationReason::verifier_failed;
      if (evaluation.verifier_unknown > 0u) {
        result.detail = "verification is UNKNOWN; this policy admits only an explicit PASS";
      } else if (evaluation.verifier_abstain > 0u) {
        result.detail = "verification abstained; this policy admits only an explicit PASS";
      } else {
        result.detail = "no verifier returned PASS for this candidate";
      }
      return result;
    }
  }
  if (judge_gating) {
    const std::uint32_t decided = evaluation.accepts + evaluation.rejects;
    const double ratio = decided == 0u
                             ? 0.0
                             : static_cast<double>(evaluation.accepts) / static_cast<double>(decided);
    if (decided == 0u || ratio < inputs.aggregation.min_judge_acceptance_ratio) {
      result.reason = EliminationReason::insufficient_judge_acceptance;
      result.detail = "judge acceptance ratio " + detail::format_double(ratio) +
                      " is below the required minimum " +
                      detail::format_double(inputs.aggregation.min_judge_acceptance_ratio);
      return result;
    }
  }
  if (evaluation.evaluations < inputs.evidence.min_evaluations_per_candidate) {
    result.reason = EliminationReason::missing_required_evidence;
    result.detail = "candidate has " + std::to_string(evaluation.evaluations) +
                    " evaluation(s), the ensemble requires " +
                    std::to_string(inputs.evidence.min_evaluations_per_candidate);
    return result;
  }
  if (inputs.aggregation.min_aggregate_score > 0.0 &&
      evaluation.mean_score() < inputs.aggregation.min_aggregate_score) {
    result.reason = EliminationReason::below_minimum_score;
    result.detail = "aggregate score " + detail::format_double(evaluation.mean_score()) +
                    " is below the required minimum " +
                    detail::format_double(inputs.aggregation.min_aggregate_score);
    return result;
  }
  result.eligible = true;
  return result;
}

}  // namespace

ArbitrationReport arbitrate(const ArbitrationInputs& inputs) {
  ArbitrationReport report;
  report.ensemble_generation = inputs.ensemble_generation;
  report.coordinator_epoch = inputs.coordinator_epoch;
  report.computed_sequence = inputs.sequence;
  report.tie_break = inputs.policy.tie_break;
  report.factor_order = inputs.policy.factors;
  report.considered = static_cast<std::uint32_t>(inputs.candidates.size());

  const bool verifier_gating = inputs.aggregation.require_verification_pass ||
                               inputs.aggregation.kind == AggregationKind::verifier_gated;
  const bool judge_gating = inputs.aggregation.require_judge_acceptance ||
                            inputs.aggregation.kind == AggregationKind::judge_arbitration;

  std::vector<const ArbitrationCandidateInput*> eligible;
  for (const ArbitrationCandidateInput& candidate : inputs.candidates) {
    CandidateRanking ranking;
    ranking.candidate = candidate.candidate;
    ranking.generation = candidate.generation;
    ranking.participant = candidate.participant;
    ranking.participant_generation = candidate.participant_generation;
    ranking.role = candidate.role;

    const Eligibility eligibility = check_eligibility(candidate, inputs, verifier_gating, judge_gating);
    ranking.eligible = eligibility.eligible;
    ranking.elimination = eligibility.reason;
    ranking.elimination_detail = eligibility.detail;
    if (eligibility.eligible) {
      eligible.push_back(&candidate);
      for (const ArbitrationFactor factor : inputs.policy.factors) {
        RankingFactorValue value;
        value.factor = factor;
        value.value = factor_value(factor, candidate);
        value.detail = factor_detail(factor, candidate);
        ranking.factors.push_back(std::move(value));
      }
    } else {
      report.eliminated += 1u;
    }
    report.ranking.push_back(std::move(ranking));
  }

  if (eligible.empty()) {
    report.no_eligible_candidate = true;
    report.notes.push_back("no candidate satisfied hard eligibility");
    return report;
  }

  // Deterministic ordering: the configured factors in order, then the stable
  // policy tie-break.  No container iteration order is ever consulted.
  const std::vector<ArbitrationFactor>& factors = inputs.policy.factors;
  std::sort(eligible.begin(), eligible.end(),
            [&factors](const ArbitrationCandidateInput* lhs,
                       const ArbitrationCandidateInput* rhs) {
              for (const ArbitrationFactor factor : factors) {
                if (factor_ties(factor, *lhs, *rhs)) {
                  continue;
                }
                return factor_precedes(factor, *lhs, *rhs);
              }
              return lhs->candidate.value() < rhs->candidate.value();
            });

  // Detect whether the top two are indistinguishable on every configured factor.
  bool top_tie = false;
  if (eligible.size() >= 2u) {
    top_tie = true;
    for (const ArbitrationFactor factor : factors) {
      if (!factor_ties(factor, *eligible[0], *eligible[1])) {
        top_tie = false;
        break;
      }
    }
  }

  std::int64_t rank = 0;
  for (const ArbitrationCandidateInput* candidate : eligible) {
    rank += 1;
    for (CandidateRanking& ranking : report.ranking) {
      if (ranking.candidate == candidate->candidate) {
        ranking.rank = rank;
        break;
      }
    }
  }

  // Reorder the report so that eligible candidates appear in rank order, which
  // makes the rendered explanation deterministic.
  std::vector<CandidateRanking> ordered;
  ordered.reserve(report.ranking.size());
  for (const ArbitrationCandidateInput* candidate : eligible) {
    for (CandidateRanking& ranking : report.ranking) {
      if (ranking.candidate == candidate->candidate && ranking.eligible) {
        ordered.push_back(ranking);
        break;
      }
    }
  }
  for (const CandidateRanking& ranking : report.ranking) {
    if (!ranking.eligible) {
      ordered.push_back(ranking);
    }
  }
  report.ranking = std::move(ordered);

  const ArbitrationCandidateInput& winner = *eligible.front();
  if (top_tie) {
    switch (inputs.policy.tie_break) {
      case TieBreakPolicy::report_tie:
        report.tie_unresolved = true;
        report.notes.push_back("the two highest ranked candidates are indistinguishable on every "
                               "configured factor; the policy reports a tie");
        return report;
      case TieBreakPolicy::lowest_candidate_id:
        // The sort already ordered by identity, so the winner is deterministic.
        report.notes.push_back("tie resolved by stable candidate identity (lowest identity wins)");
        break;
      case TieBreakPolicy::highest_score_then_id:
        if (eligible.size() >= 2u &&
            eligible[1]->evaluation.mean_score() > eligible[0]->evaluation.mean_score()) {
          report.selected = eligible[1]->candidate;
          report.selected_generation = eligible[1]->generation;
          report.selected_participant = eligible[1]->participant;
          report.selected_role = eligible[1]->role;
          report.notes.push_back("tie resolved by highest aggregate score");
          for (CandidateRanking& ranking : report.ranking) {
            if (ranking.candidate == report.selected) {
              ranking.selected = true;
            }
          }
          return report;
        }
        report.notes.push_back("tie resolved by stable candidate identity (equal aggregate scores)");
        break;
      case TieBreakPolicy::prefer_required_participant:
        if (eligible.size() >= 2u && !eligible[0]->required_participant &&
            eligible[1]->required_participant) {
          report.selected = eligible[1]->candidate;
          report.selected_generation = eligible[1]->generation;
          report.selected_participant = eligible[1]->participant;
          report.selected_role = eligible[1]->role;
          report.notes.push_back("tie resolved in favour of a required participant");
          for (CandidateRanking& ranking : report.ranking) {
            if (ranking.candidate == report.selected) {
              ranking.selected = true;
            }
          }
          return report;
        }
        report.notes.push_back("tie resolved by stable candidate identity");
        break;
    }
  }

  report.selected = winner.candidate;
  report.selected_generation = winner.generation;
  report.selected_participant = winner.participant;
  report.selected_role = winner.role;
  for (CandidateRanking& ranking : report.ranking) {
    if (ranking.candidate == report.selected && ranking.eligible) {
      ranking.selected = true;
      break;
    }
  }
  report.notes.push_back("selected by factor order: " + render_factors(factors));
  return report;
}

std::string ArbitrationReport::render() const {
  std::string out = "arbitration factors=";
  out.append(render_factors(factor_order));
  out.append(" considered=").append(std::to_string(considered));
  out.append(" eliminated=").append(std::to_string(eliminated));
  out.append(" selected=").append(selected.to_string());
  out.append(" tie_break=").append(ensemble_fabric::to_string(tie_break));
  if (tie_unresolved) {
    out.append(" tie_unresolved=true");
  }
  if (no_eligible_candidate) {
    out.append(" no_eligible_candidate=true");
  }
  return out;
}

}  // namespace ensemble_fabric
