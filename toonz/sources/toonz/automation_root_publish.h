#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace GroundzeroAutomation {

class Context;

// Publication into an output root (contract w2-persistence section 7). Native
// writers never write there: a verified tree in private storage is copied
// through descriptors into a claimed staging container in the destination's
// parent and published with one no-replace rename.

// One regular file of a staged tree: its path relative to the tree, its size
// and its SHA-256 as lowercase hex. `source` names the private file the bytes
// are read from when they do not lie at `path` under the staged tree.
struct StagedEntry final {
  QString path;
  qint64 size = 0;
  QByteArray sha256;
  QString source;
};

// Where a staged tree goes in an output root.
struct RootTarget final {
  QString root;      // the output root's name
  QString rootPath;  // its canonical directory
  QString parent;    // the parent, relative to the root; empty for the root
  QString name;      // the published directory's own name
};

// The staging container of an operation: ".groundzero-staging-" and the first
// 16 hex digits of the SHA-256 of its operation id.
QString stagingContainerName(const QString& operationId);

// Copy `staged` into `<parent>/<container>/<name>`, each file created
// exclusively, hashed while written, fully synced, reopened without following
// links, required a single-link regular file and hashed again against
// `entries`; then rename it to `<parent>/<name>` without replacing and remove
// the empty container. In a durable write the journal's stage record precedes
// the container and the publication intent (with `intent`'s fields) precedes
// the rename. The stage record names the manifest of intent.package or
// intent.delivery (its manifest_blob, manifest_bytes and manifest_name), so
// recovery removes only the files it lists. On any failure before the rename
// the container and everything copied into it are removed.
//
// Throws:
//   Error: missing_parent, already_exists, root_unavailable, path_denied,
//   publication_unsupported, or `failureCode` with details.stage (stage, copy,
//   verify, publish or flush), details.file, details.published and, when
//   known, details.reason: staged_changed (a staged file no longer matches its
//   entry), copy_changed (a copy changed before publication), name_taken (the
//   name appeared before the rename) or the system error's no_space,
//   not_writable, device_lost or io_error.
//
// Returns the published directory's path below the root in its stored
// spelling, which the journal records and results report.
QString publishToRoot(Context& context, const QString& staged,
                      const RootTarget& target,
                      const QVector<StagedEntry>& entries,
                      const QJsonObject& intent, const QString& failureCode);

// Before any work: the target's parent exists, is writable and does not hold
// the name. Throws missing_parent, already_exists, package_write_failed (not
// writable), root_unavailable or path_denied.
void requireRootTarget(const RootTarget& target);

// Whether the directory at `relative` in the output root exists, reached
// without a link or a variant spelling. Throws path_denied, root_unavailable.
bool rootDirectoryExists(const QString& rootPath, const QString& relative);

// What recovery found in an abandoned staging container.
struct ContainerCleanup final {
  bool found   = false;  // the container exists
  bool held    = false;  // a live process holds its claim: left untouched
  bool removed = false;  // the container is gone
  QStringList residue;   // entries left because no record names them
};

// Remove the staging container `container` under `parent` in the output root
// at `rootPath` when its claim is unheld: its marker and every file `listed`
// (relative to the package directory `name` inside it), then each directory
// they leave empty. Nothing is followed through a link and nothing else is
// removed.
ContainerCleanup removeAbandonedContainer(const QString& rootPath,
                                          const QString& parent,
                                          const QString& container,
                                          const QString& name,
                                          const QSet<QString>& listed);

// Removal of a package (contract w2-persistence section 8). Workspace
// packages use the workspace as their root.

// The trash name of a removal: ".groundzero-trash-" and the first 16 hex
// digits of the SHA-256 of its operation id.
QString trashName(const QString& operationId);

// Proves the directory at `relative` under `rootPath` holds exactly
// `entries`: each present, regular, single-link and unchanged, nothing else
// and no link. Returns each file's identity {device, inode, size,
// modified_ns} by its path.
//
// Throws:
//   Error: not_found; invalid_package with details.reason missing_file,
//   changed_file, unlisted_entry, link or hard_link; path_denied;
//   root_unavailable.
QJsonObject verifyPackageFiles(const QString& rootPath, const QString& relative,
                               const QVector<StagedEntry>& entries);

// Renames the directory at `relative` to `trash` beside it, never replacing.
//
// Throws:
//   Error: publication_unsupported when the volume has no such rename;
//   package_write_failed otherwise.
void trashPackage(const QString& rootPath, const QString& relative,
                  const QString& trash);

// What finishing a removal left.
struct TrashCleanup final {
  bool found        = false;  // the trash exists
  int filesRemoved  = 0;
  qint64 bytesFreed = 0;
  QStringList residue;  // at most 64 trash-relative names, sorted
  int residueCount = 0;
  bool removed     = false;  // the trash is gone
};

// Unlinks each file of the trash whose identity still equals the recorded
// one, then the folders that leaves empty and the trash itself when empty.
// `faults` arms remove:unlinking (test builds); may be null.
TrashCleanup finishTrash(const QString& rootPath, const QString& parent,
                         const QString& trash, const QJsonObject& identities,
                         const Context* faults);

}  // namespace GroundzeroAutomation
