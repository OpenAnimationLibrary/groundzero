#pragma once

#include "toonz/rasterbrushengine.h"
#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

namespace RasterBrush {
// Preserved declarations, never active evaluator inputs. IDs/scopes/targets are
// fixed technical allowlists validated by the versioned codec. Null optionals
// mean absent; unsupportedFields distinguishes present but unprojectable data.
struct SourceSensorV1 {
  QString id;
  std::optional<std::vector<Knot>> curve;
  std::optional<double> interval;
  std::optional<bool> periodic;
  std::optional<bool> fanCornersEnabled;
  std::optional<double> fanCornersStep, angleOffset;
  std::optional<int> lockedAngleMode;
};
struct SourceSensorControlV1 {
  QString scope = "primary", target;
  std::optional<bool> enabled, useCurve, useSameCurve;
  std::optional<double> strength;
  // This is the serialized numeric declaration, not a native combiner choice.
  std::optional<int> combinationCode;
  std::optional<std::vector<Knot>> commonCurve;
  QString layout = "absent";  // absent, single, list, unsupported
  QString sourceFieldsSha256;
  QStringList unsupportedFields;
  std::vector<SourceSensorV1> sensors;
};
struct SourceSensorsV1 {
  std::vector<SourceSensorControlV1> controls;
};
}  // namespace RasterBrush
