#pragma once

class ToonzScene;

namespace GroundzeroAutomation {

// Validate builtin admission, graph ownership, and acyclicity before
// serialization or rendering. Throws Error with an actionable diagnostic when
// the native graph is unsupported or malformed.
void validateAutomationFxGraph(ToonzScene& scene);

}  // namespace GroundzeroAutomation
