#pragma once

#include "automation.h"

namespace GroundzeroAutomation {

// Inspect only the owned detached document, within the advertised read budgets.
QJsonArray productionOperations();
QJsonValue executeProduction(Context& context, const QString& method,
                             const QJsonObject& params);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationProductionTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
