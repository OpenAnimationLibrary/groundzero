#include "automation_ownership.h"
#include "automation.h"
#include "automation_files.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef _WIN32
#include <qt_windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace GroundzeroAutomation {
namespace {

const char* const MarkerName = "/.owner.lock";

QString markerPath(const QString& directory) {
  return QDir::cleanPath(directory) + MarkerName;
}

}  // namespace

#ifdef _WIN32

// A marker is held while its owner keeps it open without read sharing; the
// owner shares only deletion so the marker can be renamed into place already
// held.
bool OwnershipClaim::acquire(const QString& directory) {
  release();
  const QString marker    = markerPath(directory);
  const QString temporary = marker + ".new";
  HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(temporary.utf16()),
                              GENERIC_READ | DELETE, FILE_SHARE_DELETE, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  if (!MoveFileExW(reinterpret_cast<LPCWSTR>(temporary.utf16()),
                   reinterpret_cast<LPCWSTR>(marker.utf16()),
                   MOVEFILE_REPLACE_EXISTING)) {
    CloseHandle(handle);
    DeleteFileW(reinterpret_cast<LPCWSTR>(temporary.utf16()));
    return false;
  }
  m_handle = handle;
  m_marker = marker;
  return true;
}

void OwnershipClaim::release() {
  if (!m_handle) return;
  CloseHandle(m_handle);
  m_handle = nullptr;
  if (!m_keepMarker) DeleteFileW(reinterpret_cast<LPCWSTR>(m_marker.utf16()));
  m_marker.clear();
}

bool OwnershipClaim::abandoned(const QString& directory) {
  const QString marker = markerPath(directory);
  if (!QFileInfo::exists(marker)) return false;
  const HANDLE probe =
      CreateFileW(reinterpret_cast<LPCWSTR>(marker.utf16()), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (probe == INVALID_HANDLE_VALUE) return false;
  CloseHandle(probe);
  return true;
}

#else

// The marker is created, locked and only then renamed into place, so a reader
// never sees an unlocked marker of a live owner.
bool OwnershipClaim::acquire(const QString& directory) {
  release();
  const QString marker    = markerPath(directory);
  const QString temporary = marker + ".new";
  const QByteArray from   = QFile::encodeName(temporary);
  const int descriptor =
      ::open(from.constData(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0) return false;
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0 ||
      ::rename(from.constData(), QFile::encodeName(marker).constData()) != 0) {
    ::close(descriptor);
    ::unlink(from.constData());
    return false;
  }
  m_descriptor = descriptor;
  m_marker     = marker;
  return true;
}

bool OwnershipClaim::acquireAt(int directory) {
  release();
  const char* temporary = ".owner.lock.new";
  const int descriptor =
      ::openat(directory, temporary,
               O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (descriptor < 0) return false;
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0 ||
      ::renameat(directory, temporary, directory, MarkerName + 1) != 0) {
    ::close(descriptor);
    ::unlinkat(directory, temporary, 0);
    return false;
  }
  m_descriptor = descriptor;
  m_directory  = ::dup(directory);
  return true;
}

bool OwnershipClaim::abandonedAt(int directory) {
  const int descriptor =
      ::openat(directory, MarkerName + 1, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) return false;
  const bool free = ::flock(descriptor, LOCK_EX | LOCK_NB) == 0;
  ::close(descriptor);
  return free;
}

void OwnershipClaim::release() {
  if (m_descriptor < 0) return;
  if (!m_keepMarker && m_directory >= 0)
    ::unlinkat(m_directory, MarkerName + 1, 0);
  else if (!m_keepMarker)
    ::unlink(QFile::encodeName(m_marker).constData());
  ::close(m_descriptor);
  if (m_directory >= 0) ::close(m_directory);
  m_descriptor = -1;
  m_directory  = -1;
  m_marker.clear();
}

bool OwnershipClaim::abandoned(const QString& directory) {
  const int descriptor =
      ::open(QFile::encodeName(markerPath(directory)).constData(),
             O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) return false;
  const bool free = ::flock(descriptor, LOCK_EX | LOCK_NB) == 0;
  ::close(descriptor);
  return free;
}

#endif

int OwnershipClaim::removeAbandoned(const QString& parent,
                                    const QStringList& filters) {
  int removed = 0;
  const QDir root(parent);
  for (const QFileInfo& entry :
       root.entryInfoList(filters, QDir::Dirs | QDir::NoDotAndDotDot)) {
    if (entry.isSymLink() || !abandoned(entry.absoluteFilePath())) continue;
    try {
      if (removeApprovedTree(entry.absoluteFilePath())) ++removed;
    } catch (const Error&) {
    }
  }
  return removed;
}

}  // namespace GroundzeroAutomation
