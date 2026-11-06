#pragma once

#include "rasterbrushresponse.h"
#include <atomic>
#ifdef RASTERBRUSH_SANDBOX_TESTING
#include <functional>
#include <utility>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#endif
#ifdef __APPLE__
#include <stdio.h>
#endif

namespace RasterBrush {
namespace Detail {
enum class LibraryPublication { Published, Exists, Cancelled, Failed };

#ifdef RASTERBRUSH_SANDBOX_TESTING
// Deterministic transaction-boundary faults; absent from production builds.
using LibraryInstallObserver =
    std::function<bool(const char *, const QString &, const QString &)>;
inline thread_local LibraryInstallObserver libraryInstallObserver;
inline void setLibraryInstallObserver(LibraryInstallObserver observer) {
  libraryInstallObserver = std::move(observer);
}
inline bool libraryInstallCheckpoint(const char *phase, const QString &source,
                                      const QString &destination,
                                      QString &error) {
  if (libraryInstallObserver &&
      !libraryInstallObserver(phase, source, destination)) {
    error = "Brush library transaction interrupted before publication";
    return false;
  }
  return true;
}
#endif

// Both files and directories use an atomic native create-if-absent rename.
// Qt's overwrite-capable save and check-then-rename paths cannot establish this
// contract. A different-filesystem or unsupported operation fails closed; there
// is no copy fallback and no flag that permits replacing the destination.
inline LibraryPublication publishLibraryEntryNoReplace(
    const QString &source, const QString &destination, QString &error,
    const std::atomic_bool *cancelled = nullptr) {
  error.clear();
  if (!validateResponsePathInput(source, error) ||
      !validateResponsePathInput(destination, error))
    return LibraryPublication::Failed;
#ifdef RASTERBRUSH_SANDBOX_TESTING
  if (!libraryInstallCheckpoint("before-publish", source, destination, error))
    return LibraryPublication::Failed;
#endif
  if (cancelled && cancelled->load(std::memory_order_relaxed)) {
    error = "Brush import cancelled";
    return LibraryPublication::Cancelled;
  }
#ifdef _WIN32
  const auto native = [](const QString &path) {
    return QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(
        QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
  };
  const auto from = native(source), to = native(destination);
  if (MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()),
                  reinterpret_cast<LPCWSTR>(to.utf16()),
                  MOVEFILE_WRITE_THROUGH))
    return LibraryPublication::Published;
  const DWORD code = GetLastError();
  if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS ||
      code == ERROR_DIR_NOT_EMPTY || QFileInfo(destination).exists() ||
      QFileInfo(destination).isSymLink())
    return LibraryPublication::Exists;
  error = QString("Cannot atomically publish brush library entry (Windows %1)")
              .arg(qulonglong(code));
#elif defined(__linux__) && defined(SYS_renameat2)
  const auto from = QFile::encodeName(source), to = QFile::encodeName(destination);
  // Linux UAPI RENAME_NOREPLACE is 1; avoid depending on a libc wrapper or
  // filesystem header exposing the flag. The kernel must implement it.
  if (syscall(SYS_renameat2, AT_FDCWD, from.constData(), AT_FDCWD,
               to.constData(), 1u) == 0)
    return LibraryPublication::Published;
  const int code = errno;
  if (code == EEXIST || code == ENOTEMPTY) return LibraryPublication::Exists;
  error = QString("Cannot atomically publish brush library entry (errno %1)")
              .arg(code);
#elif defined(__APPLE__) && defined(RENAME_EXCL)
  const auto from = QFile::encodeName(source), to = QFile::encodeName(destination);
  if (renamex_np(from.constData(), to.constData(), RENAME_EXCL) == 0)
    return LibraryPublication::Published;
  const int code = errno;
  if (code == EEXIST || code == ENOTEMPTY) return LibraryPublication::Exists;
  error = QString("Cannot atomically publish brush library entry (errno %1)")
              .arg(code);
#else
  error = "Atomic no-replace brush publication is unavailable on this platform";
#endif
  return LibraryPublication::Failed;
}
}  // namespace Detail
}  // namespace RasterBrush
