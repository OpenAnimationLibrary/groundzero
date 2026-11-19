#include "automation_root_publish.h"

#include "automation.h"
#include "automation_files.h"
#include "automation_journal.h"
#include "automation_ownership.h"

#include <QCryptographicHash>
#include <QFile>
#include <QMap>
#include <QUuid>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <functional>
#endif

namespace GroundzeroAutomation {

QString trashName(const QString& operationId) {
  return ".groundzero-trash-" +
         QString::fromLatin1(
             QCryptographicHash::hash(operationId.toUtf8(),
                                      QCryptographicHash::Sha256)
                 .toHex()
                 .left(16));
}

QString stagingContainerName(const QString& operationId) {
  return ".groundzero-staging-" +
         QString::fromLatin1(
             QCryptographicHash::hash(operationId.toUtf8(),
                                      QCryptographicHash::Sha256)
                 .toHex()
                 .left(16));
}

#ifdef _WIN32

QString publishToRoot(Context&, const QString&, const RootTarget&,
                      const QVector<StagedEntry>&, const QJsonObject&,
                      const QString&) {
  throw Error("root_unavailable",
              "Output roots are not available on this platform yet.");
}

ContainerCleanup removeAbandonedContainer(const QString&, const QString&,
                                          const QString&, const QString&,
                                          const QSet<QString>&) {
  return {};
}

void requireRootTarget(const RootTarget& target) {
  openRootDirectory(target.rootPath, target.parent);
}

QJsonObject verifyPackageFiles(const QString& rootPath, const QString&,
                               const QVector<StagedEntry>&) {
  openRootDirectory(rootPath, QString());
  return {};
}

void trashPackage(const QString& rootPath, const QString&, const QString&) {
  openRootDirectory(rootPath, QString());
}

TrashCleanup finishTrash(const QString&, const QString&, const QString&,
                         const QJsonObject&, const Context*) {
  return {};
}

bool rootDirectoryExists(const QString& rootPath, const QString& relative) {
  return openRootDirectory(rootPath, relative) >= 0;
}

#else

namespace {

constexpr qint64 CopyChunkBytes = 1024 * 1024;
const char* const MarkerName    = ".owner.lock";

// A descriptor closed when it goes out of scope.
class Descriptor final {
public:
  explicit Descriptor(int descriptor) : m_descriptor(descriptor) {}
  ~Descriptor() {
    if (m_descriptor >= 0) ::close(m_descriptor);
  }
  Descriptor(const Descriptor&)            = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  int get() const { return m_descriptor; }

private:
  int m_descriptor;
};

QString directoryOf(const QString& relative) {
  const int slash = relative.lastIndexOf('/');
  return slash < 0 ? QString() : relative.left(slash);
}

QString nameOf(const QString& relative) {
  return relative.mid(relative.lastIndexOf('/') + 1);
}

[[noreturn]] void refuseTree(const QString& reason, const QString& file) {
  throw Error("invalid_package",
              "The package does not match its registered manifest exactly; "
              "nothing was removed.",
              {{"reason", reason}, {"file", file}});
}

bool writeAll(int descriptor, const char* data, qint64 size) {
  while (size > 0) {
    const ssize_t written = ::write(descriptor, data, size_t(size));
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    data += written;
    size -= written;
  }
  return true;
}

// The SHA-256 of what a descriptor reads to its end, and the byte count.
QByteArray readDigest(int descriptor, qint64* bytes) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  QByteArray buffer(int(CopyChunkBytes), Qt::Uninitialized);
  *bytes = 0;
  for (;;) {
    const ssize_t got =
        ::read(descriptor, buffer.data(), size_t(buffer.size()));
    if (got < 0 && errno == EINTR) continue;
    if (got < 0) return {};
    if (got == 0) return hash.result().toHex();
    hash.addData(buffer.constData(), int(got));
    *bytes += got;
  }
}

// The directory at `relative` under `directory`, each component opened
// without following a link; -1 when one is missing or not a directory.
int openBelow(int directory, const QString& relative) {
  int current = ::dup(directory);
  for (const QString& part : relative.split('/', Qt::SkipEmptyParts)) {
    const int next = ::openat(current, QFile::encodeName(part).constData(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    ::close(current);
    if (next < 0) return -1;
    current = next;
  }
  return current;
}

// The staging container in the destination's parent and everything copied
// into it, removed again unless its package was published.
class Container final {
public:
  Container(int parent, const QString& name) : m_parent(parent), m_name(name) {}
  ~Container() { discard(); }
  Container(const Container&)            = delete;
  Container& operator=(const Container&) = delete;

  bool make() {
    m_directory = makeDirectoryAt(m_parent, m_name);
    if (m_directory < 0) return false;
    m_made = true;
    return m_claim.acquireAt(m_directory);
  }

  int descriptor() const { return m_directory; }

  // The directory at `relative` inside the container, made on first use.
  int directory(const QString& relative) {
    if (relative.isEmpty()) return m_directory;
    const auto found = m_directories.constFind(relative);
    if (found != m_directories.constEnd()) return found.value();
    const int parent = directory(directoryOf(relative));
    if (parent < 0) return -1;
    const int made = makeDirectoryAt(parent, nameOf(relative));
    if (made >= 0) {
      m_directories.insert(relative, made);
      m_order.append(relative);
    }
    return made;
  }

  void created(const QString& relative) { m_files.append(relative); }

  bool syncAll() {
    for (const int descriptor : m_directories)
      if (!fullSync(descriptor)) return false;
    return fullSync(m_directory);
  }

  // The package left the container by its rename.
  void published() {
    m_files.clear();
    m_order.clear();
  }

  // Ends the claim and removes the container once it is empty.
  void discard() {
    for (int index = m_files.size() - 1; index >= 0; --index) {
      const QString& file = m_files.at(index);
      ::unlinkat(directory(directoryOf(file)),
                 QFile::encodeName(nameOf(file)).constData(), 0);
    }
    for (int index = m_order.size() - 1; index >= 0; --index) {
      const QString& folder = m_order.at(index);
      ::unlinkat(directory(directoryOf(folder)),
                 QFile::encodeName(nameOf(folder)).constData(), AT_REMOVEDIR);
    }
    m_files.clear();
    m_order.clear();
    for (const int descriptor : m_directories) ::close(descriptor);
    m_directories.clear();
    m_claim.release();
    if (m_directory >= 0) ::close(m_directory);
    m_directory = -1;
    if (m_made)
      ::unlinkat(m_parent, QFile::encodeName(m_name).constData(), AT_REMOVEDIR);
    m_made = false;
  }

private:
  int m_parent;
  QString m_name;
  int m_directory = -1;
  bool m_made     = false;
  OwnershipClaim m_claim;
  QMap<QString, int> m_directories;
  QStringList m_order;
  QStringList m_files;
};

Error copyError(const QString& code, const QString& message,
                const QString& stage, const QString& file,
                const QString& reason = {}) {
  Error error = stagedWriteError(code, message, stage, file, false);
  if (!reason.isEmpty()) error.details.insert("reason", reason);
  return error;
}

// Copies one staged file into the container and proves the copy.
void copyEntry(const QString& staged, const StagedEntry& entry, int directory,
               const QString& code) {
  const QString name = nameOf(entry.path);
  const Descriptor source(
      ::open(QFile::encodeName(staged + '/' + entry.path).constData(),
             O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  const Descriptor target(createExclusive(directory, name));
  if (source.get() < 0 || target.get() < 0)
    throw copyError(code, "Cannot copy a staged file into the output root.",
                    "copy", entry.path);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  QByteArray buffer(int(CopyChunkBytes), Qt::Uninitialized);
  qint64 copied = 0;
  for (;;) {
    const ssize_t got =
        ::read(source.get(), buffer.data(), size_t(buffer.size()));
    if (got < 0 && errno == EINTR) continue;
    if (got == 0) break;
    if (got < 0 || !writeAll(target.get(), buffer.constData(), got))
      throw copyError(code, "Cannot copy a staged file into the output root.",
                      "copy", entry.path);
    hash.addData(buffer.constData(), int(got));
    copied += got;
  }
  if (copied != entry.size || hash.result().toHex() != entry.sha256)
    throw copyError(code,
                    "A staged file changed after it was verified; nothing "
                    "was published.",
                    "copy", entry.path, "staged_changed");
  if (!fullSync(target.get()))
    throw copyError(code, "Cannot flush a copied file to disk.", "flush",
                    entry.path);
}

// Reopens a copy by name and requires it unchanged, alone and regular.
void verifyEntry(const StagedEntry& entry, int directory, const QString& code) {
  const Descriptor copy(
      ::openat(directory, QFile::encodeName(nameOf(entry.path)).constData(),
               O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  struct stat status{};
  qint64 bytes = 0;
  if (copy.get() < 0 || ::fstat(copy.get(), &status) != 0 ||
      !S_ISREG(status.st_mode) || status.st_nlink != 1 ||
      readDigest(copy.get(), &bytes) != entry.sha256 || bytes != entry.size)
    throw copyError(code,
                    "A copied file changed before publication; nothing was "
                    "published.",
                    "verify", entry.path, "copy_changed");
}

void changeStagedFile(const QString& path) {
  QFile file(path);
  if (file.open(QIODevice::Append)) file.write("x");
}

[[noreturn]] void refuseUnsupported(Context& context, int parent) {
  struct stat status{};
  if (::fstat(parent, &status) == 0)
    noteExclusivePublicationRefused(qint64(status.st_dev));
  context.resetMutationStarted();
  throw Error("publication_unsupported",
              "This file system cannot create a name without replacing one, "
              "so the output would risk replacing a file; nothing was "
              "published.",
              {{"reason", "exclusive_publication_unavailable"},
               {"stage", "publish"},
               {"file", QJsonValue()},
               {"published", false}});
}

void copyAll(Context& context, const QString& staged, const RootTarget& target,
             const QVector<StagedEntry>& entries, Container& container,
             const QString& code) {
  if (writeFaultFires(context, "publish:staged-changed") && !entries.isEmpty())
    changeStagedFile(staged + '/' + entries.first().path);
  if (container.directory(target.name) < 0)
    throw copyError(code, "Cannot create the package in its container.",
                    "stage", QString());
  for (const StagedEntry& entry : entries) {
    const QString inner = directoryOf(entry.path);
    const QString folder =
        inner.isEmpty() ? target.name : target.name + '/' + inner;
    const int directory = container.directory(folder);
    if (directory < 0)
      throw copyError(code, "Cannot create a folder in the container.", "copy",
                      entry.path);
    container.created(folder + '/' + nameOf(entry.path));
    copyEntry(staged, entry, directory, code);
    verifyEntry(entry, directory, code);
  }
  if (!container.syncAll())
    throw copyError(code, "Cannot flush the staging container to disk.",
                    "flush", QString());
}

}  // namespace

void requireRootTarget(const RootTarget& target) {
  const Descriptor parent(openRootDirectory(target.rootPath, target.parent));
  if (parent.get() < 0)
    throw Error("missing_parent", "Create the package parent directory first.");
  FileIdentity existing;
  if (identityAt(parent.get(), target.name, &existing))
    throw Error("already_exists",
                "Choose a new package directory; existing paths are not "
                "overwritten.");
  if (::faccessat(parent.get(), ".", W_OK, 0) != 0)
    throw Error("package_write_failed",
                "The package parent directory is not writable.");
}

bool rootDirectoryExists(const QString& rootPath, const QString& relative) {
  const Descriptor directory(openRootDirectory(rootPath, relative));
  return directory.get() >= 0;
}

QString publishToRoot(Context& context, const QString& staged,
                      const RootTarget& target,
                      const QVector<StagedEntry>& entries,
                      const QJsonObject& intent, const QString& failureCode) {
  const Descriptor parent(openRootDirectory(target.rootPath, target.parent));
  if (parent.get() < 0)
    throw Error("missing_parent", "Create the package parent directory first.");
  FileIdentity existing;
  if (identityAt(parent.get(), target.name, &existing))
    throw Error("already_exists",
                "Choose a new package directory; existing paths are not "
                "overwritten.");
  context.markMutationStarted();
  // The parent as the file system spells it, so the recorded location is
  // the one text that reaches the package again.
  const auto stored = [&] {
    const QString above = storedRelativePath(target.rootPath, parent.get());
    return above.isEmpty() ? target.name : above + '/' + target.name;
  };
  const QJsonObject location = outputLocation(context, target.root, stored());
  const QString name = stagingContainerName(context.writeOperation.isEmpty()
                                                ? QUuid::createUuid().toString()
                                                : context.writeOperation);
  const bool durable = context.writeClass == "publication";
  const QJsonObject package = intent.value("package").toObject();
  if (durable)
    recordPublicationStage(
        context, location, name,
        {{"manifest_blob", package.value("manifest_blob")},
         {"manifest_bytes", package.value("manifest_bytes")}});
  Container container(parent.get(), name);
  if (!container.make() || writeFaultFires(context, "publish:staged-container"))
    throw copyError(failureCode, "Cannot create the staging container.",
                    "stage", QString());
  copyAll(context, staged, target, entries, container, failureCode);
  if (writeFaultFires(context, "publish:copied"))
    throw copyError(failureCode, "Cannot publish the copied package.", "copy",
                    QString());
  // The parent may have been moved meanwhile: the package goes where its
  // pinned descriptor is, and the intent names that place.
  if (durable)
    recordPublicationIntentAt(context, staged,
                              outputLocation(context, target.root, stored()),
                              intent);
  switch (renameBetween(container.descriptor(), target.name, parent.get(),
                        target.name)) {
  case PublishResult::Published:
    break;
  case PublishResult::Unsupported:
    refuseUnsupported(context, parent.get());
  case PublishResult::Exists:
  case PublishResult::Failed:
    throw copyError(failureCode,
                    "Cannot publish the package: its name was taken or the "
                    "move failed.",
                    "publish", QString());
  }
  container.published();
  writeFaultFires(context, "publish:renamed");
  const bool flushed = fullSync(parent.get());
  container.discard();
  if (!flushed)
    throw stagedWriteError(failureCode,
                           "The package was published but its folder could "
                           "not be flushed to disk.",
                           "flush", QString(), true);
  return stored();
}

namespace {

// Every entry under a directory, relative to `prefix`, without following
// links; at most 64.
void listTree(int directory, const QString& prefix, QStringList& found) {
  DIR* stream = ::fdopendir(::dup(directory));
  if (!stream) return;
  while (dirent* entry = ::readdir(stream)) {
    const QString name = QFile::decodeName(entry->d_name);
    if (name == "." || name == ".." || found.size() >= 64) continue;
    found.append(prefix + name);
    const Descriptor child(
        ::openat(directory, entry->d_name,
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (child.get() >= 0) listTree(child.get(), prefix + name + '/', found);
  }
  ::closedir(stream);
}

bool markerHeld(int container) {
  const Descriptor marker(
      ::openat(container, MarkerName, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  return marker.get() >= 0 && !OwnershipClaim::abandonedAt(container);
}

// Unlinks each listed file under `name` that `may` accepts, then every
// folder that leaves empty, deepest first. Returns the files and bytes removed.
QPair<int, qint64> removeListed(
    int container, const QString& name, const QSet<QString>& listed,
    const std::function<bool(const QString&, const FileIdentity&)>& may) {
  QPair<int, qint64> removed{0, 0};
  QSet<QString> folders;
  if (!name.isEmpty()) folders.insert(name);
  for (const QString& file : listed) {
    const QString relative = name.isEmpty() ? file : name + '/' + file;
    const Descriptor folder(openBelow(container, directoryOf(relative)));
    FileIdentity identity;
    if (folder.get() >= 0 &&
        identityAt(folder.get(), nameOf(relative), &identity) &&
        identity.regular && may(file, identity) &&
        ::unlinkat(folder.get(),
                   QFile::encodeName(nameOf(relative)).constData(), 0) == 0) {
      ++removed.first;
      removed.second += identity.size;
    }
    for (QString up = directoryOf(relative); !up.isEmpty();
         up         = directoryOf(up))
      folders.insert(up);
  }
  QStringList deepest = folders.values();
  std::sort(deepest.begin(), deepest.end(),
            [](const QString& a, const QString& b) {
              return a.count('/') > b.count('/');
            });
  for (const QString& folder : deepest) {
    const Descriptor above(openBelow(container, directoryOf(folder)));
    if (above.get() >= 0)
      ::unlinkat(above.get(), QFile::encodeName(nameOf(folder)).constData(),
                 AT_REMOVEDIR);
  }
  return removed;
}

QJsonObject identityJson(const FileIdentity& identity) {
  return {{"device", QString::number(identity.device)},
          {"inode", QString::number(identity.inode)},
          {"size", double(identity.size)},
          {"modified_ns", QString::number(identity.modified)}};
}

// Records every listed file's identity while proving the tree is exactly the
// listed files: present, regular, single-link, unchanged, and nothing else.
void verifyTree(int directory, const QString& prefix,
                const QHash<QString, StagedEntry>& listed,
                QJsonObject& identities) {
  DIR* stream = ::fdopendir(::dup(directory));
  if (!stream)
    throw Error("package_read_failed", "Cannot list the package directory.");
  QStringList names;
  while (dirent* entry = ::readdir(stream)) {
    const QString name = QFile::decodeName(entry->d_name);
    if (name != "." && name != "..") names.append(name);
  }
  ::closedir(stream);
  for (const QString& name : names) {
    const QString relative = prefix + name;
    FileIdentity identity;
    if (!identityAt(directory, name, &identity))
      refuseTree("missing_file", relative);
    if (identity.directory) {
      const Descriptor child(
          ::openat(directory, QFile::encodeName(name).constData(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (child.get() < 0) refuseTree("link", relative);
      verifyTree(child.get(), relative + '/', listed, identities);
      continue;
    }
    if (!identity.regular) refuseTree("link", relative);
    const auto found = listed.constFind(relative);
    if (found == listed.constEnd()) refuseTree("unlisted_entry", relative);
    if (identity.links != 1) refuseTree("hard_link", relative);
    const Descriptor file(::openat(directory,
                                   QFile::encodeName(name).constData(),
                                   O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    qint64 bytes = 0;
    if (file.get() < 0 || readDigest(file.get(), &bytes) != found->sha256 ||
        bytes != found->size || identity.size != found->size)
      refuseTree("changed_file", relative);
    identities.insert(relative, identityJson(identity));
  }
}

}  // namespace

ContainerCleanup removeAbandonedContainer(const QString& rootPath,
                                          const QString& parent,
                                          const QString& container,
                                          const QString& name,
                                          const QSet<QString>& listed) {
  ContainerCleanup result;
  int opened = -1;
  try {
    opened = openRootDirectory(rootPath, parent);
  } catch (const Error&) {
    return result;
  }
  const Descriptor above(opened);
  if (above.get() < 0 || !container.startsWith(".groundzero-staging-"))
    return result;
  const QByteArray encoded = QFile::encodeName(container);
  const Descriptor staging(
      ::openat(above.get(), encoded.constData(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (staging.get() < 0) return result;
  result.found = true;
  if (markerHeld(staging.get())) {
    result.held = true;
    return result;
  }
  removeListed(staging.get(), name, listed,
               [](const QString&, const FileIdentity&) { return true; });
  ::unlinkat(staging.get(), MarkerName, 0);
  ::unlinkat(staging.get(), ".owner.lock.new", 0);
  listTree(staging.get(), QString(), result.residue);
  result.residue.sort();
  result.removed =
      result.residue.isEmpty() &&
      ::unlinkat(above.get(), encoded.constData(), AT_REMOVEDIR) == 0;
  return result;
}

QJsonObject verifyPackageFiles(const QString& rootPath, const QString& relative,
                               const QVector<StagedEntry>& entries) {
  const Descriptor directory(openRootDirectory(rootPath, relative));
  if (directory.get() < 0)
    throw Error("not_found", "No package directory is at this path.");
  QHash<QString, StagedEntry> listed;
  for (const StagedEntry& entry : entries) listed.insert(entry.path, entry);
  QJsonObject identities;
  verifyTree(directory.get(), QString(), listed, identities);
  for (const StagedEntry& entry : entries)
    if (!identities.contains(entry.path))
      refuseTree("missing_file", entry.path);
  return identities;
}

void trashPackage(const QString& rootPath, const QString& relative,
                  const QString& trash) {
  const Descriptor parent(openRootDirectory(rootPath, directoryOf(relative)));
  if (parent.get() < 0)
    throw Error("not_found", "No package directory is at this path.");
  switch (renameBetween(parent.get(), nameOf(relative), parent.get(), trash)) {
  case PublishResult::Published:
    fullSync(parent.get());
    return;
  case PublishResult::Unsupported:
    throw Error("publication_unsupported",
                "This file system cannot rename without replacing, so the "
                "package cannot be removed safely; nothing was removed.",
                {{"reason", "exclusive_publication_unavailable"}});
  case PublishResult::Exists:
  case PublishResult::Failed:
    break;
  }
  throw Error("package_write_failed",
              "Cannot move the package aside for removal; nothing was "
              "removed.",
              {{"stage", "trash"}, {"published", false}});
}

TrashCleanup finishTrash(const QString& rootPath, const QString& parent,
                         const QString& trash, const QJsonObject& identities,
                         const Context* faults) {
  TrashCleanup result;
  int opened = -1;
  try {
    opened = openRootDirectory(rootPath, parent);
  } catch (const Error&) {
    return result;
  }
  const Descriptor above(opened);
  const QByteArray encoded = QFile::encodeName(trash);
  const Descriptor directory(
      above.get() < 0 || !trash.startsWith(".groundzero-trash-")
          ? -1
          : ::openat(above.get(), encoded.constData(),
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (directory.get() < 0) return result;
  result.found = true;
  QSet<QString> listed;
  for (auto it = identities.constBegin(); it != identities.constEnd(); ++it)
    listed.insert(it.key());
  bool first         = true;
  const auto removed = removeListed(
      directory.get(), QString(), listed,
      [&](const QString& path, const FileIdentity& now) {
        if (first && faults) writeFaultFires(*faults, "remove:unlinking");
        first = false;
        return identityJson(now) == identities.value(path).toObject();
      });
  result.filesRemoved = removed.first;
  result.bytesFreed   = removed.second;
  QStringList left;
  listTree(directory.get(), QString(), left);
  left.sort();
  result.residue      = left;
  result.residueCount = left.size();
  result.removed =
      left.isEmpty() &&
      ::unlinkat(above.get(), encoded.constData(), AT_REMOVEDIR) == 0;
  return result;
}

#endif

}  // namespace GroundzeroAutomation
