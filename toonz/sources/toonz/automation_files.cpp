#include "automation_files.h"
#include "automation.h"
#include "automation_journal.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <qt_windows.h>

#include <fcntl.h>
#include <io.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif

namespace GroundzeroAutomation {
namespace {

#ifdef BUILD_UI_PORT_TESTS
struct PublishFaults final {
  bool exclusiveRenameRefused = false;
  bool hardLinkRefused        = false;
  bool racingDestination      = false;
};

PublishFaults& publishFaults() {
  static PublishFaults faults;
  return faults;
}

struct PathSwap final {
  QString path;
  QString target;
  QString stage;
};

PathSwap& pendingSwap() {
  static PathSwap swap;
  return swap;
}

bool beneath(const QString& path, const QString& root) {
  return path == root ||
         path.startsWith(root.endsWith('/') ? root : root + '/');
}
#endif

#ifndef _WIN32

[[noreturn]] void redirected() {
  throw Error("path_denied",
              "A component of the path changed after it was approved.");
}

QString descriptorPath(int descriptor) {
  char buffer[PATH_MAX] = {};
#ifdef __APPLE__
  if (fcntl(descriptor, F_GETPATH, buffer) != 0) return QString();
  return QString::fromUtf8(buffer);
#else
  const QByteArray link = "/proc/self/fd/" + QByteArray::number(descriptor);
  const ssize_t length = readlink(link.constData(), buffer, sizeof(buffer) - 1);
  return length > 0 ? QString::fromUtf8(buffer, int(length)) : QString();
#endif
}

bool sameDirectory(const QString& opened, const QString& approved) {
  const auto form = QString::NormalizationForm_C;
  return !opened.isEmpty() &&
         opened.normalized(form) == QDir::cleanPath(approved).normalized(form);
}

// -1 when the directory cannot be opened for a reason other than a redirect.
int pinDirectory(const QString& directory) {
  const int descriptor =
      ::open(QFile::encodeName(directory).constData(),
             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    if (errno == ELOOP || errno == ENOTDIR) redirected();
    return -1;
  }
  if (!sameDirectory(descriptorPath(descriptor), directory)) {
    ::close(descriptor);
    redirected();
  }
  return descriptor;
}

// Pushes a descriptor's data to the device; on macOS only F_FULLFSYNC also
// empties the drive's own cache.
bool flushDescriptor(int descriptor, bool device) {
#ifdef __APPLE__
  if (device && fcntl(descriptor, F_FULLFSYNC) == 0) return true;
  return fsync(descriptor) == 0;
#else
  Q_UNUSED(device);
  return fdatasync(descriptor) == 0;
#endif
}

#ifdef BUILD_UI_PORT_TESTS
// Plays the other party at the decisive moment: a destination that appears
// after the directory was pinned and right before the final atomic step.
QString& racingStaged() {
  static QString staged;
  return staged;
}

void raceDestination(int directory, const QByteArray& name) {
  if (!publishFaults().racingDestination) return;
  publishFaults().racingDestination = false;
  const QString& staged             = racingStaged();
  struct stat source{};
  if (lstat(QFile::encodeName(staged).constData(), &source) == 0 &&
      S_ISDIR(source.st_mode)) {
    mkdirat(directory, name.constData(), 0700);
    return;
  }
  const int created =
      ::openat(directory, name.constData(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (created < 0) return;
  const char text[] = "other party\n";
  if (::write(created, text, sizeof(text) - 1) < 0) {
  }
  ::close(created);
}
#endif

// The kernel call that renames without ever replacing: renameatx_np with
// RENAME_EXCL on macOS, renameat2 with RENAME_NOREPLACE on Linux.
int exclusiveRename(int source, const QByteArray& from, int directory,
                    const QByteArray& name) {
#ifdef BUILD_UI_PORT_TESTS
  if (publishFaults().exclusiveRenameRefused) {
    errno = ENOTSUP;
    return -1;
  }
  raceDestination(directory, name);
#endif
#ifdef __APPLE__
  return renameatx_np(source, from.constData(), directory, name.constData(),
                      RENAME_EXCL);
#elif defined(__linux__)
  return int(syscall(SYS_renameat2, source, from.constData(), directory,
                     name.constData(), 1u));
#else
  Q_UNUSED(source);
  Q_UNUSED(from);
  Q_UNUSED(directory);
  Q_UNUSED(name);
  errno = ENOTSUP;
  return -1;
#endif
}

bool refusedForLackOfSupport(int error) {
  return error == ENOTSUP || error == EOPNOTSUPP || error == EINVAL ||
         error == ENOSYS;
}

// Hard-link the finished file to its name, which fails if the name exists,
// then drop the staged name. The name appears complete or not at all.
PublishResult linkPublish(const QByteArray& from, int directory,
                          const QByteArray& name) {
#ifdef BUILD_UI_PORT_TESTS
  if (publishFaults().hardLinkRefused) return PublishResult::Unsupported;
  raceDestination(directory, name);
#endif
  if (linkat(AT_FDCWD, from.constData(), directory, name.constData(), 0) == 0) {
    ::unlink(from.constData());
    return PublishResult::Published;
  }
  if (errno == EEXIST) return PublishResult::Exists;
  return refusedForLackOfSupport(errno) || errno == EPERM
             ? PublishResult::Unsupported
             : PublishResult::Failed;
}

// Atomic create-if-absent publication, or a refusal; never a check followed by
// a rename. A folder has no atomic alternative to the exclusive rename.
PublishResult exclusivePublish(const QString& staged, int directory,
                               const QByteArray& name) {
  const QByteArray from = QFile::encodeName(staged);
  struct stat source{};
  const bool folder =
      lstat(from.constData(), &source) == 0 && S_ISDIR(source.st_mode);
  if (exclusiveRename(AT_FDCWD, from, directory, name) == 0)
    return PublishResult::Published;
  if (errno == EEXIST) return PublishResult::Exists;
  if (!refusedForLackOfSupport(errno)) return PublishResult::Failed;
  return folder ? PublishResult::Unsupported
                : linkPublish(from, directory, name);
}

// Empties the folder behind a pinned descriptor, however many passes the
// companion files take to disappear.
bool emptyFolder(int directory, int depth) {
  if (depth > 64) return false;
  for (int pass = 0; pass < 4; ++pass) {
    const int reader = fcntl(directory, F_DUPFD_CLOEXEC, 0);
    DIR* stream      = reader < 0 ? nullptr : fdopendir(reader);
    if (!stream) {
      if (reader >= 0) ::close(reader);
      return false;
    }
    QVector<QByteArray> names;
    while (const dirent* item = readdir(stream)) {
      const QByteArray name = item->d_name;
      if (name != "." && name != "..") names.append(name);
    }
    closedir(stream);
    if (names.isEmpty()) return true;
    for (const QByteArray& name : names) {
      struct stat status{};
      if (fstatat(directory, name.constData(), &status, AT_SYMLINK_NOFOLLOW) !=
          0)
        continue;
      if (S_ISDIR(status.st_mode)) {
        const int inner =
            ::openat(directory, name.constData(),
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (inner < 0) return false;
        const bool emptied = emptyFolder(inner, depth + 1);
        ::close(inner);
        if (!emptied ||
            (unlinkat(directory, name.constData(), AT_REMOVEDIR) != 0 &&
             errno != ENOENT))
          return false;
      } else if (unlinkat(directory, name.constData(), 0) != 0 &&
                 errno != ENOENT) {
        return false;
      }
    }
  }
  return false;
}

EntryInfo::Kind entryKind(mode_t mode) {
  if (S_ISLNK(mode)) return EntryInfo::Kind::Link;
  if (S_ISDIR(mode)) return EntryInfo::Kind::Directory;
  return S_ISREG(mode) ? EntryInfo::Kind::File : EntryInfo::Kind::Other;
}

qint64 modifiedMs(const struct stat& status) {
#ifdef __APPLE__
  return qint64(status.st_mtimespec.tv_sec) * 1000 +
         status.st_mtimespec.tv_nsec / 1000000;
#else
  return qint64(status.st_mtim.tv_sec) * 1000 +
         status.st_mtim.tv_nsec / 1000000;
#endif
}

ListResult readEntries(int directory, const QString& path,
                       QVector<EntryInfo>& entries, int maximum) {
  const int reader = fcntl(directory, F_DUPFD_CLOEXEC, 0);
  if (reader < 0) return ListResult::Failed;
  DIR* stream = fdopendir(reader);
  if (!stream) {
    ::close(reader);
    return ListResult::Failed;
  }
  ListResult result = ListResult::Listed;
  while (const dirent* item = readdir(stream)) {
    const QByteArray name = item->d_name;
    if (name == "." || name == "..") continue;
    if (maximum > 0 && entries.size() >= maximum) {
      result = ListResult::TooMany;
      break;
    }
    struct stat status{};
    if (fstatat(directory, name.constData(), &status, AT_SYMLINK_NOFOLLOW) != 0)
      continue;
    const bool writable = faccessat(directory, name.constData(), W_OK, 0) == 0;
    entries.append({path, QFile::decodeName(name), entryKind(status.st_mode),
                    qint64(status.st_size), modifiedMs(status), writable});
  }
  closedir(stream);
  return result;
}

#endif

}  // namespace

#ifdef BUILD_UI_PORT_TESTS
void armPathSwap(const QString& path, const QString& target,
                 const QString& stage) {
  pendingSwap() = {path, target, stage};
}

void armPublishFaults(const QStringList& faults) {
  PublishFaults& active         = publishFaults();
  active.exclusiveRenameRefused = faults.contains("exclusive_rename_refused");
  active.hardLinkRefused        = faults.contains("hard_link_refused");
  active.racingDestination      = faults.contains("racing_destination");
}

void firePathSwap(const QString& stage, const QString& path) {
  PathSwap& swap = pendingSwap();
  if (swap.path.isEmpty() || stage != swap.stage || !beneath(path, swap.path))
    return;
  const PathSwap taken = swap;
  swap                 = {};
  const QFileInfo existing(taken.path);
  const bool present = existing.exists() || existing.isSymLink();
  QFile witness(taken.path + ".fired");
  if ((present && !QFile::rename(taken.path, taken.path + ".swapped")) ||
      !QFile::link(taken.target, taken.path) ||
      !witness.open(QIODevice::WriteOnly))
    throw Error("internal_error", "The test path swap failed.");
}
#endif

#ifdef _WIN32

// Handle-based implementation. It is written for Windows only and has not been
// compiled or run in the environment this file was written in. Every step that
// cannot be completed fails the call: a path-based open is never the fallback.
namespace {

class Handle final {
public:
  explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : m_handle(handle) {}
  Handle(Handle&& other) noexcept : m_handle(other.take()) {}
  Handle(const Handle&)            = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() { release(); }
  HANDLE get() const { return m_handle; }
  bool valid() const { return m_handle != INVALID_HANDLE_VALUE; }
  HANDLE take() {
    const HANDLE handle = m_handle;
    m_handle            = INVALID_HANDLE_VALUE;
    return handle;
  }
  void release() {
    if (valid()) CloseHandle(m_handle);
    m_handle = INVALID_HANDLE_VALUE;
  }

private:
  HANDLE m_handle;
};

[[noreturn]] void redirected() {
  throw Error("path_denied",
              "A component of the path changed after it was approved.");
}

std::wstring nativeName(const QString& path) {
  return QDir::toNativeSeparators(path).toStdWString();
}

// The location the handle really refers to, in the spelling of this module.
QString finalPath(HANDLE handle) {
  std::wstring buffer(1024, L'\0');
  DWORD length =
      GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()),
                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length >= buffer.size()) {
    buffer.assign(length + 1, L'\0');
    length =
        GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  }
  if (length == 0 || length >= buffer.size()) return QString();
  QString text = QString::fromWCharArray(buffer.data(), int(length));
  if (text.startsWith("\\\\?\\UNC\\"))
    text = "\\\\" + text.mid(8);
  else if (text.startsWith("\\\\?\\"))
    text = text.mid(4);
  return QDir::fromNativeSeparators(text);
}

bool attributes(HANDLE handle, DWORD& value) {
  FILE_ATTRIBUTE_TAG_INFO info{};
  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info,
                                    sizeof(info)))
    return false;
  value = info.FileAttributes;
  return true;
}

bool sameLocation(const QString& opened, const QString& approved) {
  return !opened.isEmpty() &&
         opened.compare(QDir::cleanPath(approved), Qt::CaseInsensitive) == 0;
}

// Opened without FILE_SHARE_DELETE, so while the handle lives neither the
// directory nor any ancestor can be renamed or replaced.
Handle pinDirectory(const QString& directory, DWORD access) {
  Handle handle(CreateFileW(
      nativeName(directory).c_str(),
      access | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!handle.valid()) return Handle();
  DWORD flags = 0;
  if (!attributes(handle.get(), flags)) return Handle();
  if (flags & FILE_ATTRIBUTE_REPARSE_POINT) redirected();
  if (!(flags & FILE_ATTRIBUTE_DIRECTORY) ||
      !sameLocation(finalPath(handle.get()), directory))
    redirected();
  return handle;
}

qint64 modifiedMsOf(const LARGE_INTEGER& time) {
  const qint64 ticks = time.QuadPart;
  return ticks / 10000 - 11644473600000LL;
}

EntryInfo::Kind kindOf(DWORD flags) {
  if (flags & FILE_ATTRIBUTE_REPARSE_POINT) return EntryInfo::Kind::Link;
  return flags & FILE_ATTRIBUTE_DIRECTORY ? EntryInfo::Kind::Directory
                                          : EntryInfo::Kind::File;
}

}  // namespace

OpenResult openApprovedFile(QFile& file, const QString& path) {
  const QFileInfo target(path);
  const Handle directory =
      pinDirectory(target.absolutePath(), FILE_LIST_DIRECTORY);
  if (!directory.valid()) return OpenResult::Failed;
#ifdef BUILD_UI_PORT_TESTS
  firePathSwap("pinned", path);
#endif
  Handle opened(CreateFileW(nativeName(path).c_str(), GENERIC_READ,
                            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!opened.valid()) return OpenResult::Failed;
  DWORD flags = 0;
  if (!attributes(opened.get(), flags)) return OpenResult::Failed;
  if (flags & FILE_ATTRIBUTE_REPARSE_POINT) redirected();
  if ((flags & FILE_ATTRIBUTE_DIRECTORY) ||
      GetFileType(opened.get()) != FILE_TYPE_DISK)
    return OpenResult::NotRegular;
  if (!sameLocation(finalPath(opened.get()), path)) redirected();
  const int descriptor =
      _open_osfhandle(reinterpret_cast<intptr_t>(opened.get()), _O_RDONLY);
  if (descriptor < 0) return OpenResult::Failed;
  opened.take();
  if (!file.open(descriptor, QIODevice::ReadOnly,
                 QFileDevice::AutoCloseHandle)) {
    _close(descriptor);
    return OpenResult::Failed;
  }
  return OpenResult::Opened;
}

PublishResult publishApproved(const QString& staged, const QString& path,
                              bool* flushed) {
  // Windows has no folder flush; NTFS journals the name with the move.
  if (flushed) *flushed = true;
  const QFileInfo target(path);
  const Handle directory = pinDirectory(target.absolutePath(), FILE_ADD_FILE);
  if (!directory.valid()) return PublishResult::Failed;
#ifdef BUILD_UI_PORT_TESTS
  firePathSwap("pinned", path);
#endif
  // Without MOVEFILE_REPLACE_EXISTING the move fails when the name exists,
  // and the file system decides that atomically.
  if (MoveFileExW(nativeName(staged).c_str(), nativeName(path).c_str(), 0))
    return PublishResult::Published;
  const DWORD error = GetLastError();
  return error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS
             ? PublishResult::Exists
             : PublishResult::Failed;
}

bool removeApprovedTree(const QString& directory) {
  const QString clean = QDir::cleanPath(directory);
  QVector<EntryInfo> entries;
  const ListResult listed = listApprovedDirectory(clean, entries);
  if (listed == ListResult::Failed) return !QFileInfo::exists(clean);
  bool removed = true;
  for (const EntryInfo& entry : entries) {
    const QString path = entry.absoluteFilePath();
    if (entry.isDir())
      removed = removeApprovedTree(path) && removed;
    else
      removed = (QFile::remove(path) || QDir().rmdir(path)) && removed;
  }
  return QDir().rmdir(clean) && removed;
}

ListResult listApprovedDirectory(const QString& directory,
                                 QVector<EntryInfo>& entries, int maximum) {
  const QString clean = QDir::cleanPath(directory);
  const Handle pinned = pinDirectory(clean, FILE_LIST_DIRECTORY);
  if (!pinned.valid()) return ListResult::Failed;
#ifdef BUILD_UI_PORT_TESTS
  firePathSwap("pinned", clean);
#endif
  alignas(8) char buffer[64 * 1024];
  FILE_INFO_BY_HANDLE_CLASS request = FileIdBothDirectoryRestartInfo;
  for (;;) {
    if (!GetFileInformationByHandleEx(pinned.get(), request, buffer,
                                      sizeof(buffer)))
      return GetLastError() == ERROR_NO_MORE_FILES ? ListResult::Listed
                                                   : ListResult::Failed;
    request            = FileIdBothDirectoryInfo;
    const char* cursor = buffer;
    for (;;) {
      const auto* item = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(cursor);
      const QString name = QString::fromWCharArray(
          item->FileName, int(item->FileNameLength / sizeof(wchar_t)));
      if (name != "." && name != "..") {
        if (maximum > 0 && entries.size() >= maximum)
          return ListResult::TooMany;
        entries.append({clean, name, kindOf(item->FileAttributes),
                        qint64(item->EndOfFile.QuadPart),
                        modifiedMsOf(item->LastWriteTime),
                        !(item->FileAttributes & FILE_ATTRIBUTE_READONLY)});
      }
      if (item->NextEntryOffset == 0) break;
      cursor += item->NextEntryOffset;
    }
  }
}

#else

OpenResult openApprovedFile(QFile& file, const QString& path) {
  const QFileInfo target(path);
  const int directory = pinDirectory(target.absolutePath());
  if (directory < 0) return OpenResult::Failed;
#ifdef BUILD_UI_PORT_TESTS
  firePathSwap("pinned", path);
#endif
  const int descriptor =
      ::openat(directory, QFile::encodeName(target.fileName()).constData(),
               O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  const int failure = errno;
  ::close(directory);
  if (descriptor < 0) {
    if (failure == ELOOP) redirected();
    return OpenResult::Failed;
  }
  struct stat status{};
  if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
    ::close(descriptor);
    return OpenResult::NotRegular;
  }
  if (!file.open(descriptor, QIODevice::ReadOnly,
                 QFileDevice::AutoCloseHandle)) {
    ::close(descriptor);
    return OpenResult::Failed;
  }
  return OpenResult::Opened;
}

PublishResult publishApproved(const QString& staged, const QString& path,
                              bool* flushed) {
  const QFileInfo target(path);
  const int directory = pinDirectory(target.absolutePath());
  if (directory < 0) return PublishResult::Failed;
  const QByteArray name = QFile::encodeName(target.fileName());
#ifdef BUILD_UI_PORT_TESTS
  firePathSwap("pinned", path);
  racingStaged() = staged;
#endif
  const PublishResult result = exclusivePublish(staged, directory, name);
  // The folder that received the name, through the descriptor pinned to it.
  if (flushed && result == PublishResult::Published)
    *flushed = flushDescriptor(directory, true);
  ::close(directory);
  return result;
}

bool removeApprovedTree(const QString& directory) {
  const QString clean = QDir::cleanPath(directory);
  const QFileInfo target(clean);
  const int parent = pinDirectory(target.absolutePath());
  if (parent < 0) return !QFileInfo::exists(clean);
  const QByteArray name = QFile::encodeName(target.fileName());
  const int inner = ::openat(parent, name.constData(),
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  bool removed    = inner < 0 && errno == ENOENT;
  if (inner >= 0) {
    removed = emptyFolder(inner, 0);
    ::close(inner);
    removed = removed && unlinkat(parent, name.constData(), AT_REMOVEDIR) == 0;
  }
  ::close(parent);
  return removed;
}

ListResult listApprovedDirectory(const QString& directory,
                                 QVector<EntryInfo>& entries, int maximum) {
  const QString clean = QDir::cleanPath(directory);
  const int pinned    = pinDirectory(clean);
  if (pinned < 0) return ListResult::Failed;
#ifdef BUILD_UI_PORT_TESTS
  firePathSwap("pinned", clean);
#endif
  const ListResult result = readEntries(pinned, clean, entries, maximum);
  ::close(pinned);
  return result;
}

#endif

#ifdef _WIN32

namespace {

bool flushFile(const QString& path) {
  const Handle file(CreateFileW(nativeName(path).c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr));
  return file.valid() && FlushFileBuffers(file.get());
}

}  // namespace

bool flushTree(const QString& path) {
  const QFileInfo root(path);
  if (!root.isDir()) return flushFile(path);
  QDirIterator files(path, QDir::Files | QDir::Hidden | QDir::System,
                     QDirIterator::Subdirectories);
  while (files.hasNext())
    if (!flushFile(files.next())) return false;
  return true;
}

bool flushDirectory(const QString&) { return true; }

#else

namespace {

bool flushPath(const QString& path, bool directory, bool device) {
  const int descriptor =
      ::open(QFile::encodeName(path).constData(),
             O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (directory ? O_DIRECTORY : 0));
  if (descriptor < 0) return false;
  const bool flushed = flushDescriptor(descriptor, device);
  ::close(descriptor);
  return flushed;
}

}  // namespace

// Every entry is pushed to the device, then one F_FULLFSYNC empties the
// drive's cache for all of them at once.
bool flushTree(const QString& path) {
  const QFileInfo root(path);
  if (root.isSymLink()) return false;
  if (!root.isDir()) return flushPath(path, false, true);
  QDirIterator entries(path,
                       QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot |
                           QDir::Hidden | QDir::System,
                       QDirIterator::Subdirectories);
  while (entries.hasNext()) {
    const QFileInfo entry(entries.next());
    if (entry.isSymLink() || !flushPath(entry.filePath(), entry.isDir(), false))
      return false;
  }
  return flushPath(path, true, true);
}

bool flushDirectory(const QString& directory) {
  return flushPath(directory, true, true);
}

#endif

QString rootPathProblem(const QString& relative, int creating) {
  static const QRegularExpression created("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$");
  if (relative.isEmpty() || relative.size() > 1024) return "length";
  if (relative.contains('\\') || relative.startsWith('/') ||
      relative.contains(QChar::Null))
    return "separator";
  const QStringList parts = relative.split('/');
  if (parts.size() > 16) return "components";
  for (int index = 0; index < parts.size(); ++index) {
    const QString& part = parts.at(index);
    if (part.isEmpty() || part == "." || part == "..") return "component";
    if (part.startsWith(".groundzero-")) return "reserved";
    if (index >= parts.size() - creating && !created.match(part).hasMatch())
      return "name";
  }
  return {};
}

#ifdef _WIN32

int openRootDirectory(const QString&, const QString&) {
  throw Error("root_unavailable",
              "Output roots are not available on this platform yet.");
}

QString storedPathBelow(const QString& root, const QString& relative) {
  openRootDirectory(root, relative);
  return {};
}

#else

int openRootDirectory(const QString& root, const QString& relative) {
  int current = ::open(QFile::encodeName(root).constData(),
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (current < 0 || !sameDirectory(descriptorPath(current), root)) {
    if (current >= 0) ::close(current);
    throw Error("root_unavailable",
                "The output root is not the granted directory.");
  }
  QString reached = QDir::cleanPath(root);
  for (const QString& part : relative.split('/', Qt::SkipEmptyParts)) {
    const QByteArray name = QFile::encodeName(part);
    const int next = ::openat(current, name.constData(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat status{};
    const bool link =
        next < 0 &&
        fstatat(current, name.constData(), &status, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISLNK(status.st_mode);
    ::close(current);
    if (link) redirected();
    if (next < 0) return -1;
    reached += '/' + part;
    // A case-insensitive volume opens a variant spelling: refuse it.
    if (!sameDirectory(descriptorPath(next), reached)) {
      ::close(next);
      redirected();
    }
    current = next;
  }
  return current;
}

QString storedRelativePath(const QString& root, int descriptor) {
  const QStringList parts = descriptorPath(descriptor).split('/');
  const QStringList above = QDir::cleanPath(root).split('/');
  if (parts.size() <= above.size()) return {};
  const auto form = QString::NormalizationForm_C;
  for (int index = 0; index < above.size(); ++index)
    if (parts.at(index).normalized(form) != above.at(index).normalized(form))
      return {};
  return parts.mid(above.size()).join('/');
}

QString storedPathBelow(const QString& root, const QString& relative) {
  const int directory = openRootDirectory(root, relative);
  if (directory < 0) return {};
  const QString stored = storedRelativePath(root, directory);
  ::close(directory);
  return stored;
}

bool identityAt(int directory, const QString& name, FileIdentity* identity) {
  struct stat status{};
  if (::fstatat(directory, QFile::encodeName(name).constData(), &status,
                AT_SYMLINK_NOFOLLOW) != 0)
    return false;
  identity->device = qint64(status.st_dev);
  identity->inode  = qint64(status.st_ino);
  identity->links  = qint64(status.st_nlink);
  identity->size   = qint64(status.st_size);
#ifdef __APPLE__
  identity->modified = qint64(status.st_mtimespec.tv_sec) * 1000000000 +
                       status.st_mtimespec.tv_nsec;
#else
  identity->modified =
      qint64(status.st_mtim.tv_sec) * 1000000000 + status.st_mtim.tv_nsec;
#endif
  identity->regular   = S_ISREG(status.st_mode);
  identity->directory = S_ISDIR(status.st_mode);
  return true;
}

int createExclusive(int directory, const QString& name) {
  return ::openat(directory, QFile::encodeName(name).constData(),
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
}

int makeDirectoryAt(int directory, const QString& name) {
  const QByteArray encoded = QFile::encodeName(name);
  if (::mkdirat(directory, encoded.constData(), 0755) != 0) return -1;
  return ::openat(directory, encoded.constData(),
                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

PublishResult renameBetween(int fromDirectory, const QString& from,
                            int toDirectory, const QString& to) {
  if (exclusiveRename(fromDirectory, QFile::encodeName(from), toDirectory,
                      QFile::encodeName(to)) == 0)
    return PublishResult::Published;
  if (errno == EEXIST) return PublishResult::Exists;
  return refusedForLackOfSupport(errno) ? PublishResult::Unsupported
                                        : PublishResult::Failed;
}

bool fullSync(int descriptor) { return flushDescriptor(descriptor, true); }

#endif

PrivateDirectory::~PrivateDirectory() {
  if (m_remove && m_directory.isValid()) removeApprovedTree(m_directory.path());
}

Error stagedWriteError(const QString& code, const QString& message,
                       const QString& stage, const QString& file,
                       bool published) {
  return Error(code, message,
               {{"stage", stage},
                {"file", file.isEmpty() ? QJsonValue() : QJsonValue(file)},
                {"published", published}});
}

bool writeFaultFires(const Context& context, const char* point) {
#ifdef BUILD_UI_PORT_TESTS
  if (!context.writeFaultArmed(point)) return false;
  if (context.writeFaultMode == "crash") std::_Exit(WriteFaultCrashExitCode);
  if (context.writeFaultMode == "hold")
    std::this_thread::sleep_for(std::chrono::seconds(10));
  return true;
#else
  Q_UNUSED(context);
  Q_UNUSED(point);
  return false;
#endif
}

namespace {

QSet<qint64>& refusedVolumes() {
  static QSet<qint64> volumes;
  return volumes;
}

qint64 volumeOf(const QString& directory) {
#ifdef _WIN32
  Q_UNUSED(directory);
  return -1;
#else
  struct stat info{};
  if (::stat(QFile::encodeName(directory).constData(), &info) != 0) return -1;
  return qint64(info.st_dev);
#endif
}

}  // namespace

bool exclusivePublicationRefused(qint64 volume) {
  return refusedVolumes().contains(volume);
}

void noteExclusivePublicationRefused(qint64 volume) {
  refusedVolumes().insert(volume);
}

void publishStaged(Context& context, const QString& staged, const QString& path,
                   const QString& failureCode, const QString& failureMessage,
                   const QJsonObject& intent) {
  context.markMutationStarted();
  const bool durable = context.writeClass == "publication";
  if (durable) recordPublicationIntent(context, staged, path, intent);
  if (durable &&
      (writeFaultFires(context, "publish:flush") || !flushTree(staged)))
    throw stagedWriteError(failureCode,
                           "Cannot flush the staged output to disk; nothing "
                           "was published.",
                           "flush", QString(), false);
  if (writeFaultFires(context, "publish"))
    throw stagedWriteError(failureCode, failureMessage, "publish", QString(),
                           false);
  bool flushed = true;
  switch (publishApproved(staged, path, durable ? &flushed : nullptr)) {
  case PublishResult::Published:
    break;
  case PublishResult::Unsupported:
    context.resetMutationStarted();
    if (const qint64 volume = volumeOf(QFileInfo(path).absolutePath());
        volume >= 0)
      refusedVolumes().insert(volume);
    throw Error("publication_unsupported",
                "This file system can neither create a name exclusively nor "
                "hard-link a finished file to it, so the output would risk "
                "replacing a file; nothing was written.",
                {{"reason", "exclusive_publication_unavailable"},
                 {"stage", "publish"},
                 {"file", QJsonValue()},
                 {"published", false}});
  case PublishResult::Exists:
  case PublishResult::Failed:
    throw stagedWriteError(failureCode, failureMessage, "publish", QString(),
                           false);
  }
  writeFaultFires(context, "publish:renamed");
  if (!flushed)
    throw stagedWriteError(failureCode,
                           "The output was published but its folder could not "
                           "be flushed to disk.",
                           "flush", QString(), true);
}

}  // namespace GroundzeroAutomation
