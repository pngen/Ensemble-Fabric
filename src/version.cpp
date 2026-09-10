#include "ensemble_fabric/version.hpp"

namespace ensemble_fabric {

std::string_view version_string() noexcept { return "1.0.0"; }

std::string_view build_description() noexcept {
  return "Ensemble Fabric 1.0.0 - governed multi-model ensemble execution runtime";
}

}  // namespace ensemble_fabric
