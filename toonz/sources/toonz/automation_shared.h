#pragma once

#include "automation.h"
#include "automation_files.h"

#include <QFileInfo>
#include <QVector>

#include <functional>

namespace GroundzeroAutomation {

constexpr int SharedFolderEntries = 8192;

// The exact bytes of one shared file and their sha256 file revision.
struct SharedFile final {
  QByteArray bytes;
  QString revision;
};

// Every entry of one folder and its listing revision.
struct SharedFolder final {
  QVector<EntryInfo> entries;
  QString revision;
};

enum class SharedEntryKind { Folder, File, Symlink, Hidden, Unsafe, Other };

// Return the configured canonical root of a role.
//
// Throws:
//   Error: root_unavailable when the role is not configured or the directory
//   is missing, unreadable or no longer canonical.
QString sharedRootPath(const Context& context, const QString& role);

// Whether a configured root still names a readable canonical directory.
bool sharedRootAvailable(const QString& path);

// Whether one path segment obeys the shared naming rules (NFC, 1-120
// characters, no forbidden or control characters, no leading dot or space, no
// trailing dot or space, no Windows reserved device name).
bool safeSharedName(const QString& name);

// Split a root-relative path into checked segments.
//
// Args:
//   path: Root-relative path with forward slashes.
//   allowEmpty: Whether "" (the root itself) is admitted.
// Throws:
//   Error: invalid_path for any grammar violation.
QStringList sharedSegments(const QString& path, bool allowEmpty);

// Resolve checked segments under a canonical root without following links.
//
// Throws:
//   Error: path_denied for a symbolic link or junction component, not_found
//   when the entry does not exist with exactly these names.
QString resolveSharedPath(const QString& root, const QStringList& segments);

// Classify one directory entry without following links.
SharedEntryKind sharedEntryKind(const EntryInfo& entry);

// Read at most SharedFolderEntries entries of a folder through a descriptor
// pinned to it.
//
// Throws:
//   Error: resource_limit above the entry bound, io_error when unreadable,
//   path_denied when the folder was replaced after it was approved.
SharedFolder readSharedFolder(const QString& directory);

// Read the folder twice around the work and retry once when it changed.
//
// Args:
//   directory: Folder whose listing pins the result.
//   build: Produces the result from one listing.
// Throws:
//   Error: revision_conflict with scope "listing" when it keeps changing.
QJsonObject stableSharedListing(
    const QString& directory,
    const std::function<QJsonObject(const SharedFolder&)>& build);

// Require an optional expected listing revision to match the folder.
void requireListingRevision(const QJsonObject& params,
                            const SharedFolder& folder);

// Read one regular file, hash it and refuse it above the byte bound.
//
// Throws:
//   Error: resource_limit above maximum, io_error when unreadable, not_found
//   when it is not a regular file.
SharedFile readSharedFile(const QString& path, qint64 maximum);

struct SharedDigest final {
  QString revision;
  qint64 size = 0;
};

// Stream-hash one regular file of at most maximum bytes.
//
// Throws:
//   Error: resource_limit above maximum, io_error when unreadable, not_found
//   when it is not a regular file.
SharedDigest hashSharedFile(const QString& path, qint64 maximum);

// Read at most maximum leading bytes of one regular file.
//
// Throws:
//   Error: io_error when unreadable, not_found when not a regular file.
QByteArray readSharedHead(const QString& path, int maximum);

QString sha256Revision(const QByteArray& bytes);
QString foldedName(const QString& name);
// Whether a string matches the sha256:/listing:/deps: revision grammar.
bool validRevisionText(const QString& value, const QString& prefix);

// Schema fragments shared by the studio palette and Library descriptors.
QString sharedSegmentPattern();
QJsonObject sharedDescriptor(const QString& name, const QString& description,
                             const QJsonObject& properties,
                             const QJsonArray& required);
QJsonObject sharedIntegerSchema(int minimum, int maximum);
QJsonObject sharedRevisionSchema(const QString& prefix);

QJsonArray studioPaletteOperations();
QJsonValue executeStudioPalette(Context& context, const QString& method,
                                const QJsonObject& params);
QJsonArray libraryOperations();
QJsonValue executeLibrary(Context& context, const QString& method,
                          const QJsonObject& params);

// Return the strict descriptors of every shared root and asset discovery tool.
QJsonArray sharedOperations();
// Execute a read-only shared discovery operation after schema validation.
QJsonValue executeShared(Context& context, const QString& method,
                         const QJsonObject& params);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationSharedTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
