#include "ensemble_fabric/candidate.hpp"

#include "ensemble_fabric/persistence.hpp"

#include "detail/text.hpp"

namespace ensemble_fabric {

std::string_view to_string(CandidateState state) noexcept {
  switch (state) {
    case CandidateState::declared: return "DECLARED";
    case CandidateState::dispatched: return "DISPATCHED";
    case CandidateState::output_received: return "OUTPUT_RECEIVED";
    case CandidateState::valid: return "VALID";
    case CandidateState::invalid: return "INVALID";
    case CandidateState::eligible: return "ELIGIBLE";
    case CandidateState::rejected: return "REJECTED";
    case CandidateState::selected: return "SELECTED";
    case CandidateState::superseded: return "SUPERSEDED";
    case CandidateState::cancelled: return "CANCELLED";
    case CandidateState::failed: return "FAILED";
    case CandidateState::abstained: return "ABSTAINED";
    case CandidateState::retired: return "RETIRED";
  }
  return "UNKNOWN_STATE";
}

std::optional<CandidateState> parse_candidate_state(std::string_view text) noexcept {
  if (text == "DECLARED") return CandidateState::declared;
  if (text == "DISPATCHED") return CandidateState::dispatched;
  if (text == "OUTPUT_RECEIVED") return CandidateState::output_received;
  if (text == "VALID") return CandidateState::valid;
  if (text == "INVALID") return CandidateState::invalid;
  if (text == "ELIGIBLE") return CandidateState::eligible;
  if (text == "REJECTED") return CandidateState::rejected;
  if (text == "SELECTED") return CandidateState::selected;
  if (text == "SUPERSEDED") return CandidateState::superseded;
  if (text == "CANCELLED") return CandidateState::cancelled;
  if (text == "FAILED") return CandidateState::failed;
  if (text == "ABSTAINED") return CandidateState::abstained;
  if (text == "RETIRED") return CandidateState::retired;
  return std::nullopt;
}

bool is_legal_transition(CandidateState from, CandidateState to) noexcept {
  if (from == to) {
    // A repeated observation of the same state is not a transition.
    return false;
  }
  switch (from) {
    case CandidateState::declared:
      return to == CandidateState::dispatched || to == CandidateState::cancelled ||
             to == CandidateState::superseded || to == CandidateState::failed;
    case CandidateState::dispatched:
      return to == CandidateState::output_received || to == CandidateState::failed ||
             to == CandidateState::abstained || to == CandidateState::cancelled ||
             to == CandidateState::superseded;
    case CandidateState::output_received:
      return to == CandidateState::valid || to == CandidateState::invalid ||
             to == CandidateState::cancelled || to == CandidateState::superseded;
    case CandidateState::valid:
      return to == CandidateState::eligible || to == CandidateState::rejected ||
             to == CandidateState::cancelled || to == CandidateState::superseded ||
             to == CandidateState::retired;
    case CandidateState::invalid:
      return to == CandidateState::retired || to == CandidateState::superseded ||
             to == CandidateState::rejected;
    case CandidateState::eligible:
      return to == CandidateState::selected || to == CandidateState::rejected ||
             to == CandidateState::superseded || to == CandidateState::cancelled ||
             to == CandidateState::retired;
    case CandidateState::rejected:
      return to == CandidateState::retired || to == CandidateState::superseded;
    case CandidateState::selected:
      return to == CandidateState::retired || to == CandidateState::superseded;
    case CandidateState::failed:
      // A failed attempt may be retried inside the same logical candidate slot;
      // the retry advances the candidate generation so that evidence from the
      // failed attempt can never be attributed to the new one.
      return to == CandidateState::declared || to == CandidateState::retired ||
             to == CandidateState::superseded;
    case CandidateState::abstained:
      return to == CandidateState::retired || to == CandidateState::superseded;
    case CandidateState::cancelled:
      return to == CandidateState::retired;
    case CandidateState::superseded:
      return to == CandidateState::retired;
    case CandidateState::retired:
      return false;
  }
  return false;
}

bool is_terminal(CandidateState state) noexcept {
  switch (state) {
    case CandidateState::failed:
    case CandidateState::abstained:
    case CandidateState::cancelled:
    case CandidateState::superseded:
    case CandidateState::retired:
    case CandidateState::selected:
    case CandidateState::rejected:
    case CandidateState::invalid:
      return true;
    case CandidateState::declared:
    case CandidateState::dispatched:
    case CandidateState::output_received:
    case CandidateState::valid:
    case CandidateState::eligible:
      return false;
  }
  return false;
}

bool is_content_state(CandidateState state) noexcept {
  return state == CandidateState::valid || state == CandidateState::eligible ||
         state == CandidateState::selected;
}

bool permits_output(CandidateState state) noexcept {
  return state == CandidateState::declared || state == CandidateState::dispatched;
}

std::uint64_t fingerprint_payload(const std::vector<std::byte>& payload) noexcept {
  return sha256_digest64(std::span<const std::byte>(payload.data(), payload.size()));
}

}  // namespace ensemble_fabric
