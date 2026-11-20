#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "automation.h"
#include "automation_live_security_p.h"

#ifdef Q_OS_WIN
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <exception>
#include <utility>
#include <vector>

namespace GroundzeroAutomation {
namespace Live {
namespace SecurityDetail {
namespace {

using LocalMemory = std::unique_ptr<void, decltype(&::LocalFree)>;

class Handle final {
 public:
  explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : m_value(value) {}
  ~Handle() { close(); }
  Handle(Handle&& other) noexcept
      : m_value(std::exchange(other.m_value, INVALID_HANDLE_VALUE)) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this == &other) return *this;
    close();
    m_value = std::exchange(other.m_value, INVALID_HANDLE_VALUE);
    return *this;
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE get() const { return m_value; }
  bool valid() const { return m_value && m_value != INVALID_HANDLE_VALUE; }

 private:
  void close() {
    if (valid()) CloseHandle(m_value);
  }
  HANDLE m_value;
};

[[noreturn]] void denied(const QString& message) {
  throw Error("live_security", message);
}

QByteArray tokenData(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
  DWORD length = 0;
  GetTokenInformation(token, kind, nullptr, 0, &length);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0 ||
      length > 65536)
    denied("Cannot size the native security token.");
  QByteArray result(int(length), '\0');
  if (!GetTokenInformation(token, kind, result.data(), length, &length))
    denied("Cannot inspect the native security token.");
  return result;
}

QByteArray copySid(PSID sid) {
  if (!IsValidSid(sid)) denied("The native security identity is invalid.");
  const DWORD length = GetLengthSid(sid);
  QByteArray result(int(length), '\0');
  if (!CopySid(length, result.data(), sid))
    denied("Cannot retain the native security identity.");
  return result;
}

QByteArray logonSid(HANDLE token) {
  const QByteArray bytes = tokenData(token, TokenGroups);
  const auto groups = reinterpret_cast<const TOKEN_GROUPS*>(bytes.constData());
  QByteArray result;
  for (DWORD index = 0; index < groups->GroupCount; ++index) {
    const auto& group = groups->Groups[index];
    if ((group.Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID) continue;
    if (!result.isEmpty())
      denied("The native token has ambiguous logon identities.");
    result = copySid(group.Sid);
  }
  if (result.isEmpty())
    denied("The native token has no interactive logon identity.");
  return result;
}

struct Identity final {
  QByteArray user;
  QByteArray logon;
  LUID authentication{};
};

Identity identity(HANDLE token) {
  const QByteArray elevation = tokenData(token, TokenElevation);
  if (reinterpret_cast<const TOKEN_ELEVATION*>(elevation.constData())
          ->TokenIsElevated)
    denied("Elevated live automation processes are unsupported.");
  const QByteArray user = tokenData(token, TokenUser);
  const QByteArray statistics = tokenData(token, TokenStatistics);
  return {
      copySid(reinterpret_cast<const TOKEN_USER*>(user.constData())->User.Sid),
      logonSid(token),
      reinterpret_cast<const TOKEN_STATISTICS*>(statistics.constData())
          ->AuthenticationId};
}

Handle processToken(HANDLE process) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token))
    denied("Cannot inspect the native peer process token.");
  return Handle(token);
}

Identity currentIdentity() {
  const auto token = processToken(GetCurrentProcess());
  return identity(token.get());
}

void requireSameIdentity(const Identity& expected, const Identity& actual) {
  if (expected.user != actual.user || expected.logon != actual.logon ||
      expected.authentication.HighPart != actual.authentication.HighPart ||
      expected.authentication.LowPart != actual.authentication.LowPart)
    denied("The local peer belongs to a different user or logon session.");
}

Identity pipeClientIdentity(HANDLE pipe) {
  if (!ImpersonateNamedPipeClient(pipe))
    denied("Cannot verify the actual named-pipe client security context.");
  HANDLE token = nullptr;
  const BOOL opened =
      OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token);
  // Continuing the editor under a client token would violate the security
  // boundary.
  if (!RevertToSelf()) std::terminate();
  const Handle owner(token);
  if (!opened) denied("Cannot inspect the named-pipe client security token.");
  return identity(owner.get());
}

PSID sid(const QByteArray& bytes) {
  return const_cast<char*>(bytes.constData());
}

LocalMemory descriptorFor(const Identity& owner) {
  LPWSTR encoded = nullptr;
  if (!ConvertSidToStringSidW(sid(owner.user), &encoded))
    denied("Cannot encode the live grant owner.");
  const LocalMemory encodedOwner(encoded, &::LocalFree);
  const QString user = QString::fromWCharArray(encoded);
  const QString sddl = "O:" + user + "D:P(A;;FA;;;" + user + ")(A;;FA;;;SY)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &descriptor,
          nullptr))
    denied("Cannot create the private live grant security descriptor.");
  return LocalMemory(descriptor, &::LocalFree);
}

void requireAllowedAce(const ACE_HEADER* header, const Identity& owner,
                       DWORD& ownerRights) {
  if (header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
      (header->AceFlags & (INHERITED_ACE | INHERIT_ONLY_ACE)))
    denied(
        "The private live grant has unsupported inherited or conditional "
        "permissions.");
  const auto ace = reinterpret_cast<const ACCESS_ALLOWED_ACE*>(header);
  PSID subject = const_cast<DWORD*>(&ace->SidStart);
  if (EqualSid(subject, sid(owner.user)))
    ownerRights |= ace->Mask;
  else if (!IsWellKnownSid(subject, WinLocalSystemSid))
    denied("The private live grant permits another security principal.");
}

void requireAclRights(PACL acl, const Identity& owner) {
  DWORD ownerRights = 0;
  for (DWORD index = 0; index < acl->AceCount; ++index) {
    void* entry = nullptr;
    if (!GetAce(acl, index, &entry))
      denied("Cannot inspect a private permission entry.");
    requireAllowedAce(static_cast<ACE_HEADER*>(entry), owner, ownerRights);
  }
  GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE,
                          FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
  MapGenericMask(&ownerRights, &mapping);
  if ((ownerRights & FILE_ALL_ACCESS) != FILE_ALL_ACCESS)
    denied("The private live grant owner requires full control.");
}

void requirePrivateAcl(HANDLE object, const Identity& owner) {
  PSID actualOwner = nullptr;
  PACL acl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetSecurityInfo(object, SE_FILE_OBJECT,
                      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                      &actualOwner, nullptr, &acl, nullptr,
                      &descriptor) != ERROR_SUCCESS)
    denied("Cannot inspect the opened live grant permissions.");
  const LocalMemory memory(descriptor, &::LocalFree);
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  if (!GetSecurityDescriptorControl(descriptor, &control, &revision) ||
      !(control & SE_DACL_PROTECTED) || !acl || !IsValidAcl(acl))
    denied("The private live grant requires an explicit protected DACL.");
  if (!EqualSid(actualOwner, sid(owner.user)))
    denied("The private live grant belongs to another user.");
  requireAclRights(acl, owner);
}

BY_HANDLE_FILE_INFORMATION fileInfo(HANDLE file) {
  BY_HANDLE_FILE_INFORMATION info{};
  if (GetFileType(file) != FILE_TYPE_DISK ||
      !GetFileInformationByHandle(file, &info))
    denied("The opened live-session object is not a local filesystem object.");
  if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
    denied("Live-session reparse points are unsupported.");
  return info;
}

bool sameObject(const BY_HANDLE_FILE_INFORMATION& first,
                const BY_HANDLE_FILE_INFORMATION& second) {
  return first.dwVolumeSerialNumber == second.dwVolumeSerialNumber &&
         first.nFileIndexHigh == second.nFileIndexHigh &&
         first.nFileIndexLow == second.nFileIndexLow;
}

void requireFile(HANDLE file, const Identity& owner) {
  const auto info = fileInfo(file);
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      info.nNumberOfLinks != 1)
    denied("The live grant must be a regular file with one link.");
  requirePrivateAcl(file, owner);
}

void requirePathComponent(const QString& part) {
  if (part.isEmpty() || part.endsWith('.') || part.endsWith(' ') ||
      part.contains(':') || part.contains(QChar('\0')))
    denied("The live-session path contains an unsafe component.");
}

QString normalizedPath(const QString& path) {
  const QString normalized = QDir::fromNativeSeparators(path);
  const QRegularExpression drive("^[A-Za-z]:/");
  if (!drive.match(normalized).hasMatch() ||
      QDir::cleanPath(normalized) != normalized)
    denied("The live-session path must be an absolute local drive path.");
  for (const QString& part : normalized.mid(3).split('/'))
    requirePathComponent(part);
  const QString root = QDir::toNativeSeparators(normalized.left(3));
  const UINT type = GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16()));
  if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
    denied("The live-session directory must be on a local drive.");
  return normalized;
}

Handle openPath(const QString& path, DWORD access, DWORD share, DWORD creation,
                SECURITY_ATTRIBUTES* attributes = nullptr) {
  const QString native = QDir::toNativeSeparators(path);
  return Handle(CreateFileW(
      reinterpret_cast<LPCWSTR>(native.utf16()), access, share, attributes,
      creation, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
}

std::vector<Handle> openDirectories(const QString& directory,
                                    const Identity& owner) {
  std::vector<Handle> result;
  QString current = directory.left(3);
  const QStringList parts = directory.mid(3).split('/', Qt::SkipEmptyParts);
  const auto open = [&result](const QString& path) {
    result.emplace_back(openPath(path, FILE_READ_ATTRIBUTES | READ_CONTROL,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 OPEN_EXISTING));
    const auto info = fileInfo(result.back().get());
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      denied("A live-session ancestor is not a directory.");
  };
  open(current);
  for (const QString& part : parts) {
    current = QDir(current).filePath(part);
    open(current);
  }
  requirePrivateAcl(result.back().get(), owner);
  return result;
}

QByteArray readBytes(HANDLE file, int size) {
  QByteArray bytes(size, '\0');
  DWORD read = 0;
  if (!ReadFile(file, bytes.data(), DWORD(size), &read, nullptr) ||
      read != DWORD(size))
    denied("Cannot read the complete opened live grant.");
  return bytes;
}

void requirePipeName(const QString& endpoint) {
  const QString prefix = "\\\\.\\pipe\\groundzero-live-";
  const QRegularExpression random(
      QRegularExpression::anchoredPattern("[0-9a-f]{32}"));
  if (!endpoint.startsWith(prefix) ||
      !random.match(endpoint.mid(prefix.size())).hasMatch())
    denied("The live endpoint is not a qualified local named pipe.");
}

class WindowsFiles final : public PrivateFiles {
 public:
  WindowsFiles(const QString& path, bool create)
      : m_path(normalizedPath(path)),
        m_directory(QFileInfo(m_path).path()),
        m_owner(currentIdentity()),
        m_directories(openDirectories(m_directory, m_owner)),
        m_create(create) {
    if (create)
      requireNewName();
    else {
      m_file =
          openPath(m_path, GENERIC_READ | READ_CONTROL | FILE_READ_ATTRIBUTES,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                   OPEN_EXISTING);
      requireFile(m_file.get(), m_owner);
    }
  }
  ~WindowsFiles() override { revoke(); }
  QString directory() const override { return m_directory; }
  QByteArray read() override {
    const auto before = fileInfo(m_file.get());
    if (before.nFileSizeHigh != 0 || before.nFileSizeLow == 0 ||
        before.nFileSizeLow > maximumSessionBytes)
      denied("The opened live grant exceeds its size limit or is empty.");
    const QByteArray result = readBytes(m_file.get(), int(before.nFileSizeLow));
    const auto after = fileInfo(m_file.get());
    if (!sameObject(before, after) ||
        before.nFileSizeLow != after.nFileSizeLow ||
        CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) != 0)
      denied("The opened live grant changed while being read.");
    return result;
  }
  void publish(const QByteArray& bytes) override {
    if (!m_create || m_file.valid()) denied("The live grant already exists.");
    const auto descriptor = descriptorFor(m_owner);
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),
                                   descriptor.get(), FALSE};
    m_file = openPath(
        m_path, GENERIC_WRITE | READ_CONTROL | DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_DELETE, CREATE_NEW, &attributes);
    if (!m_file.valid())
      denied("Cannot exclusively create the private live grant.");
    m_owned = true;
    m_identity = fileInfo(m_file.get());
    requireFile(m_file.get(), m_owner);
    DWORD written = 0;
    if (!WriteFile(m_file.get(), bytes.constData(), DWORD(bytes.size()),
                   &written, nullptr) ||
        written != DWORD(bytes.size()) || !FlushFileBuffers(m_file.get()))
      denied("Cannot finalize the private live grant.");
  }
  qintptr listenDescriptor(const QString&) override {
    denied("Windows live listeners use the native Qt named-pipe server.");
  }
  qintptr acceptDescriptor() override {
    denied("Windows live connections use the native Qt named-pipe server.");
  }
  void captureSocket(const QString& endpoint) override {
    requirePipeName(endpoint);
  }
  void verifySocket(const QString& endpoint) const override {
    requirePipeName(endpoint);
  }
  bool revoke() noexcept override {
    if (!m_owned) return true;
    m_owned = false;
    const auto current = openPath(
        m_path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
    BY_HANDLE_FILE_INFORMATION info{};
    bool removed = current.valid() &&
                   GetFileInformationByHandle(current.get(), &info) &&
                   sameObject(info, m_identity);
    if (removed) {
      FILE_DISPOSITION_INFO disposition{TRUE};
      removed = SetFileInformationByHandle(m_file.get(), FileDispositionInfo,
                                           &disposition,
                                           sizeof(disposition)) != FALSE;
    }
    m_file = Handle();
    return removed;
  }

 private:
  void requireNewName() const {
    const QString native = QDir::toNativeSeparators(m_path);
    const DWORD attributes =
        GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES ||
        GetLastError() != ERROR_FILE_NOT_FOUND)
      denied("The live-session file must not already exist.");
  }
  QString m_path;
  QString m_directory;
  Identity m_owner;
  std::vector<Handle> m_directories;
  Handle m_file;
  bool m_create;
  bool m_owned = false;
  BY_HANDLE_FILE_INFORMATION m_identity{};
};

}  // namespace

std::unique_ptr<PrivateFiles> openPrivateFiles(const QString& path,
                                               bool create) {
  return std::make_unique<WindowsFiles>(path, create);
}

QByteArray randomBytes(int count) {
  QByteArray result(count, '\0');
  if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(result.data()),
                      ULONG(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    denied("The operating-system random source is unavailable.");
  return result;
}

void verifyPeer(qintptr descriptor, bool client) {
  const HANDLE pipe = reinterpret_cast<HANDLE>(descriptor);
  const Identity expected = currentIdentity();
  ULONG pid = 0;
  const BOOL inspected = client ? GetNamedPipeClientProcessId(pipe, &pid)
                                : GetNamedPipeServerProcessId(pipe, &pid);
  if (!inspected || pid == 0)
    denied("Cannot identify the native named-pipe peer.");
  const Handle process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  const auto token = processToken(process.get());
  requireSameIdentity(expected, identity(token.get()));
  if (client) requireSameIdentity(expected, pipeClientIdentity(pipe));
}

void closeAccepted(qintptr descriptor) noexcept {
  if (descriptor != 0 && descriptor != qintptr(INVALID_HANDLE_VALUE))
    CloseHandle(reinterpret_cast<HANDLE>(descriptor));
}

QString endpointName(const QString& directory, const QString& randomName) {
  Q_UNUSED(directory);
  return "\\\\.\\pipe\\groundzero-live-" + randomName;
}

}  // namespace SecurityDetail
}  // namespace Live
}  // namespace GroundzeroAutomation
#endif
