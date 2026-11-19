#pragma once

#include "automation.h"

#include <QJsonArray>
#include <QJsonObject>

namespace GroundzeroAutomation {

// The limits of one scene package.
constexpr int PackageMaxFiles        = 8192;
constexpr qint64 PackageMaxBytes     = 256 * 1024 * 1024;
constexpr int PackageMaxDependencies = 4096;

// Where the drawing level at cast index `index` of native type `type` is
// written, relative to the package directory. A raster level's path names a
// frame sequence ("..png").
//
// Throws:
//   Error: unsupported_level for a type a package cannot hold.
QString packageLevelPath(int index, int type);

// What a package never contains, as the fixed rows {class, reason}.
QJsonArray packageNotContained();

// scene.save_preflight: every check scene.save_copy runs for one editable
// document and one destination, its dependencies mapped into the package and
// an upper bound of the package's size; nothing is written.
QJsonObject savePreflightOperation();
QJsonObject savePreflight(Context& context, const QJsonObject& params);

}  // namespace GroundzeroAutomation
