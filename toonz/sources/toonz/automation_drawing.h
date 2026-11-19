#pragma once

#include "timage.h"
#include <QString>
#include <QtGlobal>

class TPalette;
class TXshSimpleLevel;
class ToonzScene;

namespace GroundzeroAutomation {

constexpr qint64 AutomationAssetLimitBytes      = 256 * 1024 * 1024;
constexpr qint64 AutomationDrawingOverheadBytes = 65536;
constexpr qint64 AutomationStyleBytes           = 1024;
constexpr qint64 AutomationPageBytes            = 1024;

// Conservative native asset estimates, not a bound on process memory.
qint64 automationRasterBytes(int width, int height);
qint64 automationImageBytes(const TImageP& image);
void requireAssetGrowth(ToonzScene& scene, qint64 additionalBytes,
                        int additionalDrawings = 0, int additionalLevels = 0);
// The estimate of one drawing level: its palette and every drawing.
qint64 automationLevelBytes(TXshSimpleLevel& level);

// Rejects drawings and palette dependencies outside the admitted persistence
// profile.
void validateDrawingProfile(TXshSimpleLevel* level);
// The limits of validateDrawingProfile that need no decoded drawing, with the
// palette checked as `palette` (the level's own or an admitted copy). Returns
// drawing_limit, raster_size, palette_limit or format with `detail` set, or
// empty when none applies.
QString drawingProfileLimit(TXshSimpleLevel* level, TPalette* palette,
                            QString& detail);
}  // namespace GroundzeroAutomation
