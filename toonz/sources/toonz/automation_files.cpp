#include "automation_files.h"
#include "automation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

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

bool noReplaceRename(const QString& staged, int directory,
                     const QByteArray& name) {
  const QByteArray from = QFile::encodeName(staged);
#ifdef __APPLE__
  const int status = renameatx_np(AT_FDCWD, from.constData(), directory,
                                  name.constData(), RENAME_EXCL);
#elif defined(__linux__)
  const int status = int(syscall(SYS_renameat2, AT_FDCWD, from.constData(),
                                 directory, name.constData(), 1u));
#else
  const int status = -1;
  errno            = ENOTSUP;
#endif
  if (status == 0) return true;
  if (errno != ENOTSUP && errno != EINVAL) return false;
  struct stat existing{};
  if (fstatat(directory, name.constData(), &existing, AT_SYMLINK_NOFOLLOW) == 0)
    return false;
  return renameat(AT_FDCWD, from.constData(), directory, name.constData()) == 0;
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

bool publishApproved(const QString& staged, const QString& path) {
  const QFileInfo target(path);
  const Handle directory = pinDirectory(target.absolutePath(), FILE_ADD_FILE);
  if (!directory.valid()) return false;
#ifdef BUILD_UI_PORT_TESTS
  firePathSwap("pinned", path);
#endif
  return MoveFileExW(nativeName(staged).c_str(), nativeName(path).c_str(), 0) !=
         0;
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

bool publishApproved(const QString& staged, const QString& path) {
  const QFileInfo target(path);
  const int directory = pinDirectory(target.absolutePath());
  if (directory < 0) return false;
#ifdef BUILD_UI_PORT_TESTS
  firePathSwap("pinned", path);
#endif
  const bool published =
      noReplaceRename(staged, directory, QFile::encodeName(target.fileName()));
  ::close(directory);
  return published;
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

}  // namespace GroundzeroAutomation
