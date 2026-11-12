#pragma once
#include "toonz/rasterbrushengine.h"
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace RasterBrush {
// Preserved source declaration only. Preset::sourceMasked remains the distinct
// measured fixed-generated renderer; this type cannot enable it or Secondary.
// Masked curves are in Record::sourceSensorsV1 with scope masked.
struct SourceMaskDefinitionV1 {
  std::optional<bool> enabled, useMasterSize;
  std::optional<double> masterSizeCoeff;
  QString composition = "absent", tipKind = "absent", application = "absent";
  QJsonObject geometry, controlValues;
  std::optional<SourceGeneratedDefinitionV1> generated;
  // One canonical decoded resource. Tagged PNGs may be normalized to sRGB;
  // decoded pixels do not preserve original file bytes or its ICC profile.
  // RGB is retained when alpha-application coverage may depend on color.
  std::shared_ptr<const Image> tipImage;
  QString sourceFieldsSha256, sourceResourceSha256;
  QString resourceEncoding = "absent";
  QStringList unsupportedFeatures;
};
}  // namespace RasterBrush
