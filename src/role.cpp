#include "ensemble_fabric/role.hpp"

#include <array>
#include <vector>

namespace ensemble_fabric {

std::string_view to_string(ParticipantRole role) noexcept {
  switch (role) {
    case ParticipantRole::candidate: return "CANDIDATE";
    case ParticipantRole::specialist: return "SPECIALIST";
    case ParticipantRole::judge: return "JUDGE";
    case ParticipantRole::verifier: return "VERIFIER";
    case ParticipantRole::fallback: return "FALLBACK";
  }
  return "UNKNOWN_ROLE";
}

std::optional<ParticipantRole> parse_role(std::string_view text) noexcept {
  if (text == "CANDIDATE" || text == "candidate") return ParticipantRole::candidate;
  if (text == "SPECIALIST" || text == "specialist") return ParticipantRole::specialist;
  if (text == "JUDGE" || text == "judge") return ParticipantRole::judge;
  if (text == "VERIFIER" || text == "verifier") return ParticipantRole::verifier;
  if (text == "FALLBACK" || text == "fallback") return ParticipantRole::fallback;
  return std::nullopt;
}

bool role_produces_candidates(ParticipantRole role) noexcept {
  switch (role) {
    case ParticipantRole::candidate:
    case ParticipantRole::specialist:
    case ParticipantRole::fallback:
      return true;
    case ParticipantRole::judge:
    case ParticipantRole::verifier:
      return false;
  }
  return false;
}

bool role_evaluates(ParticipantRole role) noexcept {
  switch (role) {
    case ParticipantRole::judge:
    case ParticipantRole::verifier:
      return true;
    case ParticipantRole::candidate:
    case ParticipantRole::specialist:
    case ParticipantRole::fallback:
      return false;
  }
  return false;
}

bool role_can_be_selected(ParticipantRole role) noexcept { return role_produces_candidates(role); }

bool role_requires_activation(ParticipantRole role) noexcept {
  return role == ParticipantRole::fallback;
}

std::uint32_t RoleSet::size() const noexcept {
  std::uint32_t count = 0;
  for (std::uint32_t bit = 0; bit < 32u; ++bit) {
    if ((mask_ & (static_cast<std::uint32_t>(1u) << bit)) != 0u) {
      ++count;
    }
  }
  return count;
}

std::string RoleSet::to_string() const {
  static constexpr std::array<ParticipantRole, participant_role_count> kOrder = {
      ParticipantRole::candidate, ParticipantRole::specialist, ParticipantRole::judge,
      ParticipantRole::verifier, ParticipantRole::fallback};
  std::string out;
  for (const ParticipantRole role : kOrder) {
    if (!contains(role)) {
      continue;
    }
    if (!out.empty()) {
      out.push_back('|');
    }
    out.append(ensemble_fabric::to_string(role));
  }
  if (out.empty()) {
    return "NONE";
  }
  return out;
}

std::string_view to_string(VerificationState state) noexcept {
  switch (state) {
    case VerificationState::pass: return "PASS";
    case VerificationState::fail: return "FAIL";
    case VerificationState::abstain: return "ABSTAIN";
    case VerificationState::unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

std::optional<VerificationState> parse_verification_state(std::string_view text) noexcept {
  if (text == "PASS" || text == "pass") return VerificationState::pass;
  if (text == "FAIL" || text == "fail") return VerificationState::fail;
  if (text == "ABSTAIN" || text == "abstain") return VerificationState::abstain;
  if (text == "UNKNOWN" || text == "unknown") return VerificationState::unknown;
  return std::nullopt;
}

std::string_view to_string(CategoricalJudgment judgment) noexcept {
  switch (judgment) {
    case CategoricalJudgment::accept: return "ACCEPT";
    case CategoricalJudgment::reject: return "REJECT";
    case CategoricalJudgment::abstain: return "ABSTAIN";
    case CategoricalJudgment::unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

std::optional<CategoricalJudgment> parse_categorical_judgment(std::string_view text) noexcept {
  if (text == "ACCEPT" || text == "accept") return CategoricalJudgment::accept;
  if (text == "REJECT" || text == "reject") return CategoricalJudgment::reject;
  if (text == "ABSTAIN" || text == "abstain") return CategoricalJudgment::abstain;
  if (text == "UNKNOWN" || text == "unknown") return CategoricalJudgment::unknown;
  return std::nullopt;
}

std::string_view to_string(CriterionKind kind) noexcept {
  switch (kind) {
    case CriterionKind::hard_predicate: return "HARD_PREDICATE";
    case CriterionKind::ranking_signal: return "RANKING_SIGNAL";
  }
  return "UNKNOWN_KIND";
}

std::optional<CriterionKind> parse_criterion_kind(std::string_view text) noexcept {
  if (text == "HARD_PREDICATE" || text == "hard_predicate") return CriterionKind::hard_predicate;
  if (text == "RANKING_SIGNAL" || text == "ranking_signal") return CriterionKind::ranking_signal;
  return std::nullopt;
}

}  // namespace ensemble_fabric
