#pragma once

#include "automation.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QStringList>

#include <functional>

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

// What the file system says about a destination at this moment.
struct DestinationFacts final {
  QString rootCode;
  QString rootDetail;
  QString pathCode;
  QString pathDetail;
  bool parentFound = false;
  bool writable    = false;
  int exists       = -1;  // 1, 0, or -1 when it cannot be told
  qint64 freeBytes = -1;
  int sameVolume   = -1;  // 1, 0, or -1 when it cannot be told
  // expected, unknown or failed_last_attempt: whether the parent's volume
  // renames without replacing.
  QString rename = "unknown";
};

// The destination `target` in an output root, reached through the pinned
// walk: the root's and the path's refusals, and the parent's facts.
DestinationFacts observeOutputDestination(const Context& context,
                                          const PackageTarget& target);

// The checks of a preflight in their reporting order, each {name, state
// (pass, fail or unknown), code, detail}; one not set reports unknown.
class PreflightChecks final {
public:
  explicit PreflightChecks(QStringList names);
  void pass(const QString& name) { set(name, "pass", {}, {}); }
  void fail(const QString& name, const QString& code, const QString& detail) {
    set(name, "fail", code, detail);
  }
  void unknown(const QString& name, const QString& detail) {
    set(name, "unknown", {}, detail);
  }
  QJsonArray report() const;
  // The name of the first failed check, or null.
  QJsonValue firstFailure() const;
  // The failed check of this name, or empty.
  QJsonObject failed(const QString& name) const;

private:
  void set(const QString& name, const QString& state, const QString& code,
           const QString& detail);

  QStringList m_names;
  QMap<QString, QJsonObject> m_checks;
};

// What a package never contains, as the fixed rows {class, reason}.
QJsonArray packageNotContained();

// Where an imported document's source scene lies in its packages.
inline QString packageSourceScenePath() {
  return QStringLiteral("provenance/source-scene.tnz");
}

// What a package carries beside the scene and its assets.
struct PackageExtras final {
  // Import provenance written into the manifest as "provenance"; empty for a
  // document that was not imported.
  QJsonObject provenance;
  // A private copy of the imported source scene, attached byte for byte at
  // packageSourceScenePath() after its revision is checked against
  // provenance.source.scene_file_revision; empty for none.
  QString sourceScene;
  // Called with the drawings written or verified so far, between drawings;
  // it may throw to stop the write.
  std::function<void(int)> progress;
  // The document's intake records, written into the manifest as "intake";
  // empty for a document that imported no level through intake.
  QJsonObject intake;
  // The project a draft is bound to, written into the manifest as "project":
  // its id and the digest of its folder, never a path; empty for none.
  QJsonObject project;
};

// A package written and verified in private storage.
struct VerifiedPackage final {
  QJsonObject manifest;
  QByteArray manifestBytes;
  // Hex SHA-256 of manifestBytes.
  QString manifestSha256;
  qint64 bytes = 0;
  int drawings = 0;
};

// Validates `scene` against the editable profile, writes it with the native
// writers into the existing empty private `directory`, and reopens it there
// with the package verifier. scene.save_copy, render snapshots and the import
// job all write through it.
//
// Throws:
//   Error: the profile refusal, or the failing step's error with
//   details.stage, details.file and details.published false.
VerifiedPackage writeVerifiedPackage(Context& context, ToonzScene& scene,
                                     const QString& revision,
                                     const QString& directory,
                                     const PackageExtras& extras = {});

// scene.save_preflight: every check scene.save_copy runs for one editable
// document and one destination, its dependencies mapped into the package and
// an upper bound of the package's size; nothing is written.
QJsonObject savePreflightOperation();
QJsonObject savePreflight(Context& context, const QJsonObject& params);

}  // namespace GroundzeroAutomation
