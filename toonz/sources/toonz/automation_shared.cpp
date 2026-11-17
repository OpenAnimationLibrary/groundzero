#include "automation_shared.h"

#include "toonz/libraryitems.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>

#include <algorithm>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace GroundzeroAutomation {
namespace {

constexpr int MaxSegments      = 16;
constexpr int MaxPathLength    = 1024;
constexpr int MaxSegmentLength = 120;

bool forbiddenCharacter(QChar character) {
  const ushort code = character.unicode();
  return code < 0x20 || code == 0x7f ||
         QStringLiteral("/\\:*?\"<>|").contains(character);
}

bool linkLike(const QFileInfo& info) {
  return info.isSymLink() || info.isJunction();
}

char entryCode(const QFileInfo& info) {
  if (linkLike(info)) return 'l';
  if (info.isDir()) return 'd';
  return info.isFile() ? 'f' : 'o';
}

QByteArray listingLine(const QFileInfo& info) {
  const char code   = entryCode(info);
  const bool file   = code == 'f';
  const qint64 size = file ? info.size() : 0;
  const qint64 changed =
      code == 'l' ? 0 : info.lastModified().toMSecsSinceEpoch();
  return info.fileName().toUtf8().toHex() + ' ' + code + ' ' +
         QByteArray::number(size) + ' ' + QByteArray::number(changed);
}

QJsonValue layoutFlag(const QString& root, const QString& child) {
  const QFileInfo info(root + '/' + child);
  return info.isDir() && !linkLike(info);
}

QJsonObject rootRow(const Context& context, const QString& role) {
  const bool configured       = context.sharedRoots.contains(role);
  const SharedRootGrant grant = context.sharedRoots.value(role);
  const bool available        = configured && sharedRootAvailable(grant.path);
  QJsonObject layout{{"global_palettes_folder", QJsonValue()},
                     {"templates_folder", QJsonValue()},
                     {"poses_folder", QJsonValue()}};
  if (available && role == "studio_palettes")
    layout.insert("global_palettes_folder",
                  layoutFlag(grant.path, "Global Palettes"));
  if (available && role == "library") {
    layout.insert("templates_folder", layoutFlag(grant.path, "templates"));
    layout.insert("poses_folder", layoutFlag(grant.path, "poses"));
  }
  return {{"root", role},
          {"configured", configured},
          {"writable", configured && grant.writable},
          {"available", available},
          {"path", configured ? QJsonValue(grant.path) : QJsonValue()},
          {"layout", layout}};
}

QJsonObject sharedRoots(const Context& context) {
  QJsonArray rows;
  for (const QString& role : sharedRootRoles())
    rows.append(rootRow(context, role));
  return {{"roots", rows},
          {"recovery_folder_name", ".groundzero-recovery"},
          {"lock_file_name", ".groundzero.lock"}};
}

}  // namespace

const QStringList& sharedRootRoles() {
  static const QStringList roles{"studio_palettes", "project_palettes",
                                 "library"};
  return roles;
}

bool sharedRootAvailable(const QString& path) {
  const QFileInfo info(path);
  return info.isDir() && !linkLike(info) && info.isReadable() &&
         info.canonicalFilePath() == path;
}

QString sharedRootPath(const Context& context, const QString& role) {
  const auto grant = context.sharedRoots.constFind(role);
  if (grant == context.sharedRoots.constEnd())
    throw Error("root_unavailable",
                "This shared root role was not granted at worker launch.",
                {{"root", role}, {"reason", "not_configured"}});
  if (!sharedRootAvailable(grant->path))
    throw Error("root_unavailable",
                "The granted shared root is missing, unreadable or changed.",
                {{"root", role}, {"reason", "unavailable"}});
  return grant->path;
}

bool safeSharedName(const QString& name) {
  if (name.isEmpty() || name.size() > MaxSegmentLength ||
      name.normalized(QString::NormalizationForm_C) != name)
    return false;
  for (const QChar character : name)
    if (forbiddenCharacter(character)) return false;
  for (const QChar edge : {name.front(), name.back()})
    if (edge == '.' || edge == ' ') return false;
  return !LibraryItems::isReservedName(name.toStdWString());
}

QStringList sharedSegments(const QString& path, bool allowEmpty) {
  if (path.isEmpty()) {
    if (allowEmpty) return {};
    throw Error("invalid_path", "Name a nonempty root-relative path.");
  }
  if (path.size() > MaxPathLength)
    throw Error("invalid_path", "Shared paths have at most 1024 characters.");
  const QStringList segments = path.split('/');
  if (segments.size() > MaxSegments)
    throw Error("invalid_path", "Shared paths have at most 16 segments.");
  for (const QString& segment : segments)
    if (!safeSharedName(segment))
      throw Error("invalid_path",
                  "Each segment needs 1-120 NFC characters without / \\ : * "
                  "? \" < > | or controls, no leading or trailing dot or "
                  "space, and no reserved device name.");
  return segments;
}

QString resolveSharedPath(const QString& root, const QStringList& segments) {
  QString current = root;
  for (const QString& segment : segments) {
    current += '/' + segment;
    const QFileInfo info(current);
    if (linkLike(info))
      throw Error("path_denied",
                  "Shared paths never follow symbolic links or junctions.");
    if (!info.exists())
      throw Error("not_found", "The shared entry does not exist.");
  }
  if (QFileInfo(current).canonicalFilePath() != current)
    throw Error("not_found",
                "The shared entry does not exist with exactly this name.");
  return current;
}

SharedEntryKind sharedEntryKind(const QFileInfo& entry) {
  if (linkLike(entry)) return SharedEntryKind::Symlink;
  if (entry.fileName().startsWith('.')) return SharedEntryKind::Hidden;
  if (!safeSharedName(entry.fileName())) return SharedEntryKind::Unsafe;
  if (entry.isDir()) return SharedEntryKind::Folder;
  return entry.isFile() ? SharedEntryKind::File : SharedEntryKind::Other;
}

SharedFolder readSharedFolder(const QString& directory) {
  if (!QFileInfo(directory).isReadable())
    throw Error("io_error", "Cannot read the shared folder.");
  QDirIterator iterator(directory, QDir::AllEntries | QDir::Hidden |
                                       QDir::System | QDir::NoDotAndDotDot);
  SharedFolder folder;
  QVector<QByteArray> lines;
  while (iterator.hasNext()) {
    iterator.next();
    if (folder.entries.size() == SharedFolderEntries)
      throw Error("resource_limit",
                  "The shared folder has more than 8192 entries.",
                  {{"limit", SharedFolderEntries}});
    folder.entries.append(iterator.fileInfo());
    lines.append(listingLine(iterator.fileInfo()));
  }
  std::sort(lines.begin(), lines.end());
  QCryptographicHash hash(QCryptographicHash::Sha256);
  for (const QByteArray& line : lines) hash.addData(line + '\n');
  folder.revision = "listing:" + QString::fromLatin1(hash.result().toHex());
  return folder;
}

QJsonObject stableSharedListing(
    const QString& directory,
    const std::function<QJsonObject(const SharedFolder&)>& build) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    const SharedFolder before = readSharedFolder(directory);
    QJsonObject result        = build(before);
    if (readSharedFolder(directory).revision == before.revision) return result;
  }
  throw Error("revision_conflict",
              "The shared folder kept changing while it was listed.",
              {{"scope", "listing"}});
}

void requireListingRevision(const QJsonObject& params,
                            const SharedFolder& folder) {
  if (!params.contains("expected_listing_revision")) return;
  const QString expected = params.value("expected_listing_revision").toString();
  if (!validRevisionText(expected, "listing"))
    throw Error("invalid_params",
                "expected_listing_revision must be listing:<64 hex>.");
  if (expected != folder.revision)
    throw Error("revision_conflict",
                "The shared folder changed since that listing.",
                {{"scope", "listing"}, {"listing_revision", folder.revision}});
}

namespace {

void openRegularFile(QFile& file, const QString& path) {
#ifdef _WIN32
  const QFileInfo info(path);
  if (!info.isFile() || linkLike(info))
    throw Error("not_found", "The shared entry is not a regular file.");
  file.setFileName(path);
  if (!file.open(QIODevice::ReadOnly))
    throw Error("io_error", "Cannot open the shared file.");
#else
  const int descriptor = ::open(QFile::encodeName(path).constData(),
                                O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (descriptor < 0) throw Error("io_error", "Cannot open the shared file.");
  struct stat status{};
  if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
    ::close(descriptor);
    throw Error("not_found", "The shared entry is not a regular file.");
  }
  if (!file.open(descriptor, QIODevice::ReadOnly,
                 QFileDevice::AutoCloseHandle)) {
    ::close(descriptor);
    throw Error("io_error", "Cannot open the shared file.");
  }
#endif
}

}  // namespace

SharedFile readSharedFile(const QString& path, qint64 maximum) {
  QFile file;
  openRegularFile(file, path);
  const QJsonObject bound{{"size_bytes", double(file.size())},
                          {"limit_bytes", double(maximum)}};
  if (file.size() > maximum)
    throw Error("resource_limit", "The shared file exceeds its byte bound.",
                bound);
  const QByteArray bytes = file.read(maximum + 1);
  if (file.error() != QFileDevice::NoError)
    throw Error("io_error", "Cannot read the shared file.");
  if (bytes.size() > maximum)
    throw Error("resource_limit", "The shared file grew past its byte bound.",
                bound);
  return {bytes, sha256Revision(bytes)};
}

QByteArray readSharedHead(const QString& path, int maximum) {
  QFile file;
  openRegularFile(file, path);
  const QByteArray bytes = file.read(maximum);
  if (file.error() != QFileDevice::NoError)
    throw Error("io_error", "Cannot read the shared file.");
  return bytes;
}

SharedDigest hashSharedFile(const QString& path, qint64 maximum) {
  QFile file;
  openRegularFile(file, path);
  if (file.size() > maximum)
    throw Error("resource_limit", "Shared files exceed the hashing budget.",
                {{"limit_bytes", double(maximum)}});
  QCryptographicHash hash(QCryptographicHash::Sha256);
  SharedDigest digest;
  while (!file.atEnd()) {
    const QByteArray block = file.read(1024 * 1024);
    if (block.isEmpty() || file.error() != QFileDevice::NoError)
      throw Error("io_error", "Cannot read a shared dependency.");
    digest.size += block.size();
    if (digest.size > maximum)
      throw Error("resource_limit", "Shared files exceed the hashing budget.",
                  {{"limit_bytes", double(maximum)}});
    hash.addData(block);
  }
  digest.revision = "sha256:" + QString::fromLatin1(hash.result().toHex());
  return digest;
}

QString sha256Revision(const QByteArray& bytes) {
  return "sha256:" + QString::fromLatin1(QCryptographicHash::hash(
                                             bytes, QCryptographicHash::Sha256)
                                             .toHex());
}

QString foldedName(const QString& name) {
  return name.normalized(QString::NormalizationForm_C).toCaseFolded();
}

bool validRevisionText(const QString& value, const QString& prefix) {
  static const QRegularExpression digest("^[a-z0-9]+:[0-9a-f]{64}$");
  return value.startsWith(prefix + ':') && digest.match(value).hasMatch();
}

QString sharedSegmentPattern() {
  return QStringLiteral(
      R"([^/\\:*?"<>|\x00-\x1f\x7f. ](?:[^/\\:*?"<>|\x00-\x1f\x7f]{0,118})"
      R"([^/\\:*?"<>|\x00-\x1f\x7f. ])?)");
}

QJsonObject sharedDescriptor(const QString& name, const QString& description,
                             const QJsonObject& properties,
                             const QJsonArray& required) {
  return {{"name", name},
          {"description", description},
          {"inputSchema", QJsonObject{{"type", "object"},
                                      {"additionalProperties", false},
                                      {"properties", properties},
                                      {"required", required}}},
          {"annotations", QJsonObject{{"readOnlyHint", true},
                                      {"destructiveHint", false},
                                      {"idempotentHint", true}}}};
}

QJsonObject sharedIntegerSchema(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject sharedRevisionSchema(const QString& prefix) {
  const int length = prefix.size() + 65;
  return {{"type", "string"},
          {"minLength", length},
          {"maxLength", length},
          {"pattern", "^" + prefix + ":[0-9a-f]{64}$"}};
}

QJsonArray sharedOperations() {
  QJsonArray operations{sharedDescriptor(
      "shared.roots",
      "List the shared studio-palette and Library roots this worker was "
      "started with, their write grant and native layout. No other paths are "
      "reachable.",
      {}, {})};
  for (const QJsonValue& value : studioPaletteOperations())
    operations.append(value);
  for (const QJsonValue& value : libraryOperations()) operations.append(value);
  return operations;
}

QJsonValue executeShared(Context& context, const QString& method,
                         const QJsonObject& params) {
  if (method == "shared.roots") return sharedRoots(context);
  if (method.startsWith("studio_palette."))
    return executeStudioPalette(context, method, params);
  if (method.startsWith("library."))
    return executeLibrary(context, method, params);
  throw Error("method_not_found", "The shared operation is not available.");
}

}  // namespace GroundzeroAutomation
