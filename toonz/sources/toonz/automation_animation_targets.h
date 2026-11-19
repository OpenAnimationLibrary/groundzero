#pragma once

#include <QJsonObject>

class TDoubleParam;

namespace GroundzeroAutomation {

struct Document;

// True when an animation request names an effect or a column instead of a
// stage object.
bool isAnimationExternalRequest(const QJsonObject& params);

// The animation.channels result for fx_id (every numeric parameter component
// of the effect) or column_id (the time remap curve of a level column).
//
// Throws:
//   Error: invalid_params when several of object_id, fx_id and column_id are
//   given, invalid_handle for an unknown handle, unsupported_target for a
//   macro or a column that is not a level column.
QJsonObject describeAnimationExternalChannels(Document& document,
                                              const QJsonObject& params);

// The animation.keys result for a channel the descriptors above publish.
QJsonObject inspectAnimationExternalKeys(Document& document,
                                         const QJsonObject& params);

// The animation.sample result for a channel the descriptors above publish.
// Curves with expression, file or other content outside the profile are
// refused with unsupported_interpolation.
QJsonObject sampleAnimationExternal(Document& document,
                                    const QJsonObject& params);

}  // namespace GroundzeroAutomation
