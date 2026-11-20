#pragma once

#include "automation.h"
#include "tvectorimage.h"

class TXshSimpleLevel;
class TStroke;

namespace GroundzeroAutomation {
QJsonArray vectorOpacityOperations();
int nativeOpacityPerMille(const TStroke& stroke);
void validateVectorOpacity(const TVectorImageP& image);
QString vectorLevelOpacityDigest(TXshSimpleLevel& level);
#ifdef BUILD_UI_PORT_TESTS
void runAutomationVectorOpacityTests(Context& context);
#endif
}  // namespace GroundzeroAutomation
