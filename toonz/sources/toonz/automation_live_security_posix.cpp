#include "automation.h"
#include "automation_live_security_p.h"

#ifndef Q_OS_WIN

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <cerrno>
#include <climits>
#include <cstring>
#include <utility>
#include <vector>

#ifdef Q_OS_MACOS
#include <sys/acl.h>
#include <sys/mount.h>
#elif defined(Q_OS_LINUX)
#include <linux/magic.h>
#include <sys/vfs.h>
#endif

namespace GroundzeroAutomation {
namespace Live {
namespace SecurityDetail {
namespace {

class Fd final {
 public:
  explicit Fd(int value = -1) : m_value(value) {}
  ~Fd() {
    if (m_value >= 0) ::close(m_value);
  }
  Fd(Fd&& other) noexcept : m_value(std::exchange(other.m_value, -1)) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this == &other) return *this;
    if (m_value >= 0) ::close(m_value);
    m_value = std::exchange(other.m_value, -1);
    return *this;
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return m_value; }
  int release() { return std::exchange(m_value, -1); }

 private:
  int m_value;
};

[[noreturn]] void denied(const QString& message) {
  throw Error("live_security", message);
}

struct stat status(int descriptor) {
  struct stat result{};
  if (::fstat(descriptor, &result) != 0)
    denied("Cannot inspect the opened live-session object.");
  return result;
}

bool sameObject(const struct stat& first, const struct stat& second) {
  return first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}

#ifdef Q_OS_LINUX
bool localFilesystemType(long type) {
  switch (type) {
    case EXT4_SUPER_MAGIC:
    case XFS_SUPER_MAGIC:
    case BTRFS_SUPER_MAGIC:
    case TMPFS_MAGIC:
    case RAMFS_MAGIC:
      return true;
    default:
      return false;
  }
}
#endif

void requireLocalFilesystem(int descriptor) {
#ifdef Q_OS_MACOS
  struct statfs info{};
  if (::fstatfs(descriptor, &info) != 0 || !(info.f_flags & MNT_LOCAL))
    denied("The live-session directory must use a local filesystem.");
#elif defined(Q_OS_LINUX)
  struct statfs info{};
  if (::fstatfs(descriptor, &info) != 0)
    denied("Cannot identify the live-session filesystem.");
  if (!localFilesystemType(info.f_type))
    denied(
        "The live-session filesystem is outside the local filesystem profile.");
#else
  Q_UNUSED(descriptor);
  denied("This platform has no qualified live-session filesystem checks.");
#endif
}

void requirePrivateDirectory(const struct stat& info) {
  if (info.st_uid != ::geteuid() || (info.st_mode & 07777) != 0700)
    denied(
        "The live-session directory must be owned by this user with mode "
        "0700.");
}

void requireNoExtendedAcl(int descriptor) {
#ifdef Q_OS_MACOS
  errno = 0;
  acl_t acl = ::acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED);
  if (!acl && errno == ENOENT) return;
  if (!acl) denied("Cannot inspect the private live-session ACL.");
  const std::unique_ptr<void, decltype(&::acl_free)> owner(acl, &::acl_free);
  if (::acl_valid(acl) != 0) denied("The private live-session ACL is invalid.");
  acl_entry_t entry = nullptr;
  errno = 0;
  if (::acl_get_entry(acl, ACL_FIRST_ENTRY, &entry) == 0 || errno != EINVAL)
    denied("Private live-session objects must not have extended ACL entries.");
#else
  Q_UNUSED(descriptor);
#endif
}

void requireDirectory(const struct stat& info, bool privateDirectory) {
  if (!S_ISDIR(info.st_mode))
    denied("A live-session ancestor is not a directory.");
  if (info.st_uid != 0 && info.st_uid != ::geteuid())
    denied("A live-session ancestor belongs to another user.");
  const bool writableByOthers = (info.st_mode & 0022) != 0;
  if (writableByOthers && !(info.st_mode & S_ISVTX))
    denied(
        "A live-session ancestor permits unsafe replacement by other users.");
  if (privateDirectory) requirePrivateDirectory(info);
}

std::vector<Fd> openDirectories(const QString& directory) {
  std::vector<Fd> result;
  result.emplace_back(
      ::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  requireDirectory(status(result.back().get()), false);
  const QStringList parts = directory.split('/', Qt::SkipEmptyParts);
  for (const QString& part : parts) {
    const QByteArray encoded = QFile::encodeName(part);
    const int fd = ::openat(result.back().get(), encoded.constData(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
      denied("Cannot open a live-session ancestor without following links.");
    result.emplace_back(fd);
    requireDirectory(status(fd), false);
  }
  requireDirectory(status(result.back().get()), true);
  requireNoExtendedAcl(result.back().get());
  requireLocalFilesystem(result.back().get());
  return result;
}

QString normalizedPath(const QString& path) {
  if (!QDir::isAbsolutePath(path) || path.contains(QChar('\0')) ||
      QDir::cleanPath(path) != path || QFileInfo(path).fileName().isEmpty())
    denied(
        "The live-session path must be absolute with no aliased components.");
  return path;
}

void requireFile(const struct stat& info) {
  if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
      (info.st_mode & 07777) != 0600 || info.st_nlink != 1)
    denied(
        "The opened live grant must be a private regular file with mode 0600 "
        "and one link.");
}

QByteArray readBytes(int descriptor, int size) {
  QByteArray result(size, '\0');
  int position = 0;
  while (position < size) {
    const ssize_t count =
        ::read(descriptor, result.data() + position, size_t(size - position));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) denied("Cannot read the complete opened live grant.");
    position += int(count);
  }
  return result;
}

void writeBytes(int descriptor, const QByteArray& bytes) {
  int position = 0;
  while (position < bytes.size()) {
    const ssize_t count = ::write(descriptor, bytes.constData() + position,
                                  size_t(bytes.size() - position));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) denied("Cannot write the private live grant.");
    position += int(count);
  }
  if (::fsync(descriptor) != 0)
    denied("Cannot finalize the private live grant.");
}

void prepareDescriptor(int descriptor) {
  if (descriptor < 0) denied("Cannot create the native live socket.");
  const int flags = ::fcntl(descriptor, F_GETFL);
  if (flags < 0 || ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0 ||
      ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0)
    denied(
        "Cannot make the native live socket nonblocking and private to this "
        "process.");
}

class PosixFiles final : public PrivateFiles {
 public:
  PosixFiles(const QString& path, bool create)
      : m_path(normalizedPath(path)),
        m_directory(QFileInfo(m_path).path()),
        m_name(QFile::encodeName(QFileInfo(m_path).fileName())),
        m_directories(openDirectories(m_directory)),
        m_create(create) {
    if (::getuid() != ::geteuid())
      denied("Set-user-ID live sessions are unsupported.");
    if (create)
      requireNewName();
    else
      openGrant();
  }
  ~PosixFiles() override { revoke(); }
  QString directory() const override { return m_directory; }

  QByteArray read() override {
    const struct stat before = status(m_grant.get());
    requireFile(before);
    requireNoExtendedAcl(m_grant.get());
    if (before.st_size < 1 || before.st_size > maximumSessionBytes)
      denied("The opened live grant exceeds its size limit or is empty.");
    const QByteArray bytes = readBytes(m_grant.get(), int(before.st_size));
    const struct stat after = status(m_grant.get());
    requireFile(after);
    if (!sameObject(before, after) || before.st_size != after.st_size ||
        before.st_mtime != after.st_mtime)
      denied("The opened live grant changed while being read.");
    return bytes;
  }

  void publish(const QByteArray& bytes) override {
    if (!m_create || m_grant.get() >= 0)
      denied("The live grant already exists.");
    m_grant = Fd(::openat(parent(), m_name.constData(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                          0600));
    if (m_grant.get() < 0)
      denied("Cannot exclusively create the private live grant.");
    m_identity = status(m_grant.get());
    m_ownedGrant = true;
    if (::fchmod(m_grant.get(), 0600) != 0)
      denied("Cannot set restrictive permissions on the opened live grant.");
    requireFile(status(m_grant.get()));
    requireNoExtendedAcl(m_grant.get());
    writeBytes(m_grant.get(), bytes);
  }

  qintptr listenDescriptor(const QString& endpoint) override {
    if (!m_create || m_listener.get() >= 0)
      denied("The live listener already exists.");
    const QByteArray path = QFile::encodeName(endpoint);
    struct sockaddr_un address{};
    if (path.size() >= int(sizeof(address.sun_path)))
      denied("The native local socket path exceeds its platform limit.");
    address.sun_family = AF_UNIX;
#ifdef Q_OS_MACOS
    address.sun_len = sizeof(address);
#endif
    std::memcpy(address.sun_path, path.constData(), size_t(path.size() + 1));
    m_listener = Fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    prepareDescriptor(m_listener.get());
    if (::bind(m_listener.get(), reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0)
      denied("Cannot exclusively bind the native live endpoint.");
    captureSocket(endpoint);
    if (::listen(m_listener.get(), 4) != 0)
      denied("Cannot listen on the native live endpoint.");
    return m_listener.get();
  }

  qintptr acceptDescriptor() override {
    const int descriptor = ::accept(m_listener.get(), nullptr, nullptr);
    if (descriptor < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -1;
    Fd accepted(descriptor);
    prepareDescriptor(accepted.get());
    return accepted.release();
  }

  void captureSocket(const QString& endpoint) override {
    if (!m_create) denied("An attached connector cannot own the GUI socket.");
    verifySocket(endpoint);
    m_socketName = QFile::encodeName(QFileInfo(endpoint).fileName());
    struct stat identity{};
    if (::fstatat(parent(), m_socketName.constData(), &identity,
                  AT_SYMLINK_NOFOLLOW) != 0)
      denied("Cannot capture the created live socket identity.");
    if (m_ownedSocket && !sameObject(m_socketIdentity, identity))
      denied("The created live socket was replaced before grant publication.");
    m_socketIdentity = identity;
    m_ownedSocket = true;
  }

  void verifySocket(const QString& endpoint) const override {
    const QFileInfo file(endpoint);
    const QRegularExpression name(
        QRegularExpression::anchoredPattern("gz-live-[0-9a-f]{32}\\.sock"));
    if (file.path() != m_directory || !name.match(file.fileName()).hasMatch())
      denied("The live endpoint is outside the private grant directory.");
    struct stat info{};
    const QByteArray encoded = QFile::encodeName(file.fileName());
    if (::fstatat(parent(), encoded.constData(), &info, AT_SYMLINK_NOFOLLOW) !=
        0)
      denied("Cannot inspect the live socket without following links.");
    if (!S_ISSOCK(info.st_mode) || info.st_uid != ::geteuid() ||
        info.st_nlink != 1)
      denied("The live endpoint is not a socket owned by the current user.");
    requireDirectory(status(parent()), true);
    requireNoExtendedAcl(parent());
  }

  bool revoke() noexcept override {
    m_listener = Fd();
    bool removed = true;
    if (m_ownedGrant) removed = removeOwned(m_name, m_identity);
    if (m_ownedSocket)
      removed = removeOwned(m_socketName, m_socketIdentity) && removed;
    m_ownedGrant = false;
    m_ownedSocket = false;
    m_grant = Fd();
    return removed;
  }

 private:
  int parent() const { return m_directories.back().get(); }
  void requireNewName() const {
    struct stat info{};
    if (::fstatat(parent(), m_name.constData(), &info, AT_SYMLINK_NOFOLLOW) ==
            0 ||
        errno != ENOENT)
      denied("The live-session file must not already exist.");
  }
  void openGrant() {
    m_grant = Fd(::openat(parent(), m_name.constData(),
                          O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (m_grant.get() < 0)
      denied("Cannot safely open the existing live grant.");
    requireFile(status(m_grant.get()));
  }
  bool removeOwned(const QByteArray& name,
                   const struct stat& identity) noexcept {
    struct stat current{};
    if (::fstatat(parent(), name.constData(), &current, AT_SYMLINK_NOFOLLOW) !=
        0)
      return errno == ENOENT;
    if (!sameObject(current, identity)) return false;
    return ::unlinkat(parent(), name.constData(), 0) == 0;
  }
  QString m_path;
  QString m_directory;
  QByteArray m_name;
  std::vector<Fd> m_directories;
  Fd m_grant;
  Fd m_listener;
  bool m_create;
  bool m_ownedGrant = false;
  bool m_ownedSocket = false;
  struct stat m_identity{};
  struct stat m_socketIdentity{};
  QByteArray m_socketName;
};

}  // namespace

std::unique_ptr<PrivateFiles> openPrivateFiles(const QString& path,
                                               bool create) {
  return std::make_unique<PosixFiles>(path, create);
}

QByteArray randomBytes(int count) {
  Fd source(::open("/dev/urandom", O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  const struct stat info = status(source.get());
  if (!S_ISCHR(info.st_mode) || info.st_uid != 0)
    denied("The operating-system random source is unavailable.");
  return readBytes(source.get(), count);
}

namespace {
#ifdef Q_OS_MACOS
void requirePeerUid(int descriptor) {
  uid_t uid = 0;
  gid_t gid = 0;
  if (::getpeereid(descriptor, &uid, &gid) != 0 || uid != ::geteuid())
    denied("The local peer does not have the current user's credentials.");
}
#elif defined(Q_OS_LINUX)
void requirePeerUid(int descriptor) {
  struct ucred credentials{};
  socklen_t length = sizeof(credentials);
  if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials,
                   &length) != 0 ||
      length != sizeof(credentials) || credentials.uid != ::geteuid())
    denied("The local peer does not have the current user's credentials.");
}
#else
void requirePeerUid(int) {
  denied("This platform cannot verify local peer credentials.");
}
#endif
}  // namespace

void verifyPeer(qintptr descriptor, bool client) {
  Q_UNUSED(client);
  if (descriptor < 0 || descriptor > INT_MAX)
    denied("The local peer handle is invalid.");
  requirePeerUid(int(descriptor));
}

void closeAccepted(qintptr descriptor) noexcept {
  if (descriptor >= 0 && descriptor <= INT_MAX) ::close(int(descriptor));
}

QString endpointName(const QString& directory, const QString& randomName) {
  const QString endpoint = directory + "/gz-live-" + randomName + ".sock";
  const QByteArray encoded = QFile::encodeName(endpoint);
  struct sockaddr_un address{};
  if (encoded.size() >= int(sizeof(address.sun_path)))
    denied(
        "Use a shorter private directory for the native local socket path "
        "limit.");
  struct stat info{};
  if (::lstat(encoded.constData(), &info) == 0 || errno != ENOENT)
    denied("The randomly selected live endpoint already exists.");
  return endpoint;
}

}  // namespace SecurityDetail
}  // namespace Live
}  // namespace GroundzeroAutomation

#endif
