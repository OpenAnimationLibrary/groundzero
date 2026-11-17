#pragma once

class ToonzScene;
class QString;

namespace GroundzeroAutomation {

bool isAutomationFxTypeSupported(const QString& type);

// Validate builtin admission, graph ownership, and acyclicity before
// serialization or rendering. Throws Error with an actionable diagnostic when
// the native graph is unsupported or malformed.
void validateAutomationFxGraph(ToonzScene& scene);

}  // namespace GroundzeroAutomation
