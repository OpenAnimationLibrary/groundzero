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

// A package directory named by a request: in the workspace (or, for a read,
// an input root) when `root` is absent, else relative to the named output
// root.
struct PackageTarget final {
  QString root;      // workspace, or the output root's name
  QString rootPath;  // the output root's canonical directory
  QString relative;  // the package directory, relative to its root
  QString absolute;  // the resolved path, in the workspace or an input root
  QString parent;    // in an output root: the parent, relative to the root
  QString name;      // the package directory's own name
};

// The package directory in params[field]; `write` for a directory to create.
//
// Throws:
//   Error: path_denied or invalid_path from Context::resolvePath;
//   root_unavailable for an output root not granted; invalid_path (reason
//   grammar or reserved) for a root-relative path outside the grammar.
PackageTarget packageTarget(Context& context, const QJsonObject& params,
                            const QString& field, bool write);

// What a package never contains, as the fixed rows {class, reason}.
QJsonArray packageNotContained();

// scene.save_preflight: every check scene.save_copy runs for one editable
// document and one destination, its dependencies mapped into the package and
// an upper bound of the package's size; nothing is written.
QJsonObject savePreflightOperation();
QJsonObject savePreflight(Context& context, const QJsonObject& params);

}  // namespace GroundzeroAutomation
