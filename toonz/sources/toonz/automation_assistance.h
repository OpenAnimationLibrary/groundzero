#pragma once

#include "automation.h"

namespace GroundzeroAutomation {

// Inspect fixed app-adjacent metadata without starting or verifying components.
QJsonArray assistanceOperations();
QJsonValue executeAssistance(const QString& method, const QJsonObject& params);

namespace AssistanceDetail {
enum class PackagePlatform { Posix, Windows };
struct Build {
  bool cadmium             = false;
  bool neuralStyle         = false;
  PackagePlatform platform = PackagePlatform::Posix;
};

// Args: applicationDirectory is canonical; build describes compiled native
// code. Returns: bounded declared presence, never integrity or inference
// qualification.
QJsonObject inspectPackages(const QString& applicationDirectory,
                            const Build& build,
                            const QString& component = "all");
}  // namespace AssistanceDetail

#ifdef BUILD_UI_PORT_TESTS
void runAutomationAssistanceTests();
#endif
}  // namespace GroundzeroAutomation
