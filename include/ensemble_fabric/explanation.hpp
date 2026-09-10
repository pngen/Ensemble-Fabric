#pragma once

#include <string>
#include <vector>

#include "ensemble_fabric/ids.hpp"

namespace ensemble_fabric {

/// One deterministic explanation line.  Explanations are data, not log text, so
/// tests can assert on them and tools can render them.
struct ExplanationEntry {
  std::string subject;
  std::string detail;
};

}  // namespace ensemble_fabric
