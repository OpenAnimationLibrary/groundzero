#pragma once

#include "timage.h"
#include <QtGlobal>

class TXshSimpleLevel;
class ToonzScene;

namespace GroundzeroAutomation {

constexpr qint64 AutomationAssetLimitBytes      = 256 * 1024 * 1024;
constexpr qint64 AutomationDrawingOverheadBytes = 65536;
constexpr qint64 AutomationStyleBytes           = 1024;

// Conservative native asset estimates, not a bound on process memory.
qint64 automationRasterBytes(int width, int height);
qint64 automationImageBytes(const TImageP& image);
void requireAssetGrowth(ToonzScene& scene, qint64 additionalBytes,
                        int additionalDrawings = 0, int additionalLevels = 0);

// Rejects drawings and palette dependencies outside the admitted persistence
// profile.
void validateDrawingProfile(TXshSimpleLevel* level);
}  // namespace GroundzeroAutomation
