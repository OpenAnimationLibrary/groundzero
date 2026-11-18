#include "automation_files.h"
#include "automation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifndef _WIN32
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

OpenResult openApprovedFile(QFile& file, const QString& path) {
  const QFileInfo info(path);
  if (!info.isFile() || info.isSymLink() || info.isJunction())
    return OpenResult::NotRegular;
  file.setFileName(path);
  return file.open(QIODevice::ReadOnly) ? OpenResult::Opened
                                        : OpenResult::Failed;
}

bool publishApproved(const QString& staged, const QString& path) {
  if (QFileInfo(path).exists() || QFileInfo(path).isSymLink()) return false;
  return QFileInfo(staged).isDir() ? QDir().rename(staged, path)
                                   : QFile::rename(staged, path);
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

#endif

}  // namespace GroundzeroAutomation
