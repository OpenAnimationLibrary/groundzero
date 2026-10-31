#pragma once

#include "toonz/rasterbrushengine.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QElapsedTimer>
#include <atomic>
#include <algorithm>
#include <limits>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace RasterBrush {
namespace Detail {
inline bool validateResponsePathInput(const QString &path, QString &error) {
  if (path.contains(QChar::Null)) {
    error = "Invalid response path";
    return false;
  }
#ifdef _WIN32
  // QFileInfo can normalize an extended/device prefix away. Reject these
  // namespaces before any normalization, not only on the resulting path.
  // Do not use QDir::fromNativeSeparators here: Qt 5 itself strips the
  // extended prefix in that function before replacing backslashes.
  const auto separator = [](QChar c) {
    return c == QLatin1Char('/') || c == QLatin1Char('\\');
  };
  if (path.size() >= 2 && separator(path[0]) && separator(path[1])) {
    error = "Regular-file protocol requires a local disk path";
    return false;
  }
#endif
  return true;
}
// The worker's directory contents are hostile, including on platforms whose
// privilege sandbox is not yet complete. Never inspect with QFileInfo and then
// open with QFile: that follows a replaced link and can block opening a FIFO.
// Validate the object actually opened, and retain that handle for every read.
class RegularResponseDirectory {
#ifdef _WIN32
  std::vector<HANDLE> directories_;
  QString path_;
#else
  int handle_ = -1;
#endif
  friend class RegularResponseFile;

public:
  RegularResponseDirectory()                                 = default;
  RegularResponseDirectory(const RegularResponseDirectory &) = delete;
  RegularResponseDirectory &operator=(const RegularResponseDirectory &) =
      delete;
  ~RegularResponseDirectory() {
#ifdef _WIN32
    for (HANDLE handle : directories_) CloseHandle(handle);
#else
    if (handle_ >= 0) close(handle_);
#endif
  }
  bool open(const QString &path, QString &error) {
    error.clear();
    if (!validateResponsePathInput(path, error)) return false;
    auto fail = [&](const char *message) {
      error = QString::fromLatin1(message);
      return false;
    };
    const QString absolute =
        QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (absolute.contains(QChar::Null))
      return fail("Invalid response directory path");
#ifdef _WIN32
    if (absolute.size() < 3 || absolute[1] != QLatin1Char(':') ||
        absolute[2] != QLatin1Char('/'))
      return fail("Regular-file protocol requires a local disk path");
    // Retaining every ancestor without delete sharing prevents directory
    // replacement between checking the path and opening a response file.
    // Attribute-only opens are exempt from share-access checks. Request actual
    // directory read access so denying FILE_SHARE_DELETE prevents rename.
    // https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-createfilew
    QString prefix   = absolute.left(3);
    const auto parts = absolute.mid(3).split('/', Qt::SkipEmptyParts);
    for (int i = 0; i <= parts.size(); ++i) {
      // Qt file APIs already add the extended-length prefix; raw Win32
      // opens must do so too. Only the validated local disk path above is
      // eligible: this does not admit UNC or caller-supplied device paths.
      const QString native =
          QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(prefix);
      HANDLE handle = CreateFileW(
          reinterpret_cast<LPCWSTR>(native.utf16()),
          FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (handle == INVALID_HANDLE_VALUE)
        return fail("Cannot anchor response directory");
      directories_.push_back(handle);
      FILE_ATTRIBUTE_TAG_INFO attributes{};
      if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                        &attributes, sizeof(attributes)) ||
          !(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
          (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return fail("Response directory is a reparse point or non-directory");
      if (i < parts.size()) prefix = QDir(prefix).filePath(parts[i]);
    }
    path_ = absolute;
#else
    // Resolve trusted ancestors once (macOS commonly uses /var ->
    // /private/var), then open the final directory itself without following a
    // link. The parent importer does this BEFORE launching the untrusted
    // worker. All subsequent response opens are relative to this retained
    // descriptor, not path lookups.
    const QFileInfo info(absolute);
    const QString parent = info.dir().canonicalPath();
    if (parent.isEmpty())
      return fail("Cannot resolve response directory parent");
    const QByteArray native =
        QFile::encodeName(QDir(parent).filePath(info.fileName()));
    handle_ = ::open(native.constData(), O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                                             O_NOFOLLOW | O_NONBLOCK);
    struct stat status {};
    if (handle_ < 0 || fstat(handle_, &status) != 0 || !S_ISDIR(status.st_mode))
      return fail("Response directory is a link or non-directory");
#endif
    return true;
  }
};

class RegularResponseFile {
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int handle_ = -1;
#endif
  qint64 size_ = -1, position_ = 0;
  bool fail(QString &error, const char *message) {
    error = QString::fromLatin1(message);
    return false;
  }

public:
  RegularResponseFile()                                       = default;
  RegularResponseFile(const RegularResponseFile &)            = delete;
  RegularResponseFile &operator=(const RegularResponseFile &) = delete;
  ~RegularResponseFile() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (handle_ >= 0) close(handle_);
#endif
  }
  bool open(const RegularResponseDirectory &directory, const QString &name,
            qint64 maximum, QString &error) {
    error.clear();
    if (maximum < 0 || maximum > std::numeric_limits<int>::max() - 1)
      return fail(error, "Invalid regular-file read budget");
    if (name.isEmpty() || name == "." || name == ".." || name.contains('/') ||
        name.contains('\\') || name.contains(':') || name.contains(QChar::Null))
      return fail(error, "Invalid response file name");
#ifdef _WIN32
    const QString native =
        QStringLiteral("\\\\?\\") +
        QDir::toNativeSeparators(QDir(directory.path_).filePath(name));
    handle_ = CreateFileW(
        reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE ||
        GetFileType(handle_) != FILE_TYPE_DISK)
      return fail(error, "Cannot open a regular response file");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle_, &info) ||
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1)
      return fail(error, "Response is not a single-link regular file");
    const quint64 length =
        (quint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (length > quint64(maximum))
      return fail(error, "Regular response file exceeds its byte budget");
    size_ = qint64(length);
#else
    const QByteArray native = QFile::encodeName(name);
    handle_                 = openat(directory.handle_, native.constData(),
                                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat info {};
    if (handle_ < 0 || fstat(handle_, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_nlink != 1)
      return fail(error, "Response is not a single-link regular file");
    if (info.st_size < 0 || quint64(info.st_size) > quint64(maximum))
      return fail(error, "Regular response file exceeds its byte budget");
    size_ = qint64(info.st_size);
#endif
    return true;
  }
  qint64 size() const { return size_; }
  QByteArray read(qint64 amount, QString &error) {
    if (amount < 0 || size_ < 0 || amount > size_ - position_ ||
        amount > std::numeric_limits<int>::max() - 1) {
      fail(error, "Regular response read exceeds its validated size");
      return {};
    }
    QByteArray result(int(amount), Qt::Uninitialized);
    qint64 offset = 0;
    while (offset < amount) {
      const int chunk = int(std::min(qint64(65536), amount - offset));
#ifdef _WIN32
      DWORD count   = 0;
      const bool ok = ReadFile(handle_, result.data() + offset, DWORD(chunk),
                               &count, nullptr);
      if (!ok || count == 0) {
#else
      ssize_t count;
      do {
        count = ::read(handle_, result.data() + offset, size_t(chunk));
      } while (count < 0 && errno == EINTR);
      if (count <= 0) {
#endif
        fail(error, "Cannot read complete regular response file");
        return {};
      }
      offset += count;
      position_ += count;
    }
    return result;
  }
  bool finish(QString &error) {
    if (position_ != size_)
      return fail(error, "Incomplete regular response read");
#ifdef _WIN32
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle_, &size) || size.QuadPart != size_)
#else
    struct stat info {};
    if (fstat(handle_, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_nlink != 1 || info.st_size != size_)
#endif
      return fail(error, "Regular response file changed during reading");
    return true;
  }
};
inline QByteArray readResponseFile(const RegularResponseDirectory &directory,
                                   const QString &name, qint64 maximum,
                                   QString &error) {
  RegularResponseFile file;
  if (!file.open(directory, name, maximum, error)) return {};
  auto bytes = file.read(file.size(), error);
  if (!error.isEmpty() || !file.finish(error)) return {};
  return bytes;
}
}  // namespace Detail
struct Record;
struct ImportResult;
RASTERBRUSH_API ImportResult readWorkerResponse(
    const Detail::RegularResponseDirectory &directory, bool normalExit,
    int exitCode, QString &error, size_t maximumDecodedBytes,
    const std::atomic_bool *cancelled = nullptr,
    const QElapsedTimer *elapsed      = nullptr);
// Internal worker entry point shares the directory anchored before child
// launch.
RASTERBRUSH_API Record decodeWorkerRecordInDirectory(
    const QByteArray &bytes, const Detail::RegularResponseDirectory &directory,
    QString &error, size_t maximumDecodedBytes);
}  // namespace RasterBrush
