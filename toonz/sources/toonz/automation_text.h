#pragma once

#include "automation.h"

namespace GroundzeroAutomation {

QJsonArray textOperations();
QJsonValue executeText(Context& context, const QString& method,
                       const QJsonObject& params);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationTextTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
