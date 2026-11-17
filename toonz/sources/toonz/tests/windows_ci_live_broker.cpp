#include "windows_ci_live_broker.h"

#ifdef _WIN32
#include <aclapi.h>
#include <sddl.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <exception>
#include <memory>
#include <stdexcept>
#include <vector>

namespace GroundzeroWindowsLiveTest {
namespace {

using Handle = std::unique_ptr<void, decltype(&::CloseHandle)>;
using LocalMemory = std::unique_ptr<void, decltype(&::LocalFree)>;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QByteArray userSid(HANDLE token) {
  DWORD length = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &length);
  require(GetLastError() == ERROR_INSUFFICIENT_BUFFER && length > 0 &&
              length <= 65536,
          "Cannot size fixture owner token");
  QByteArray bytes(int(length), '\0');
  require(GetTokenInformation(token, TokenUser, bytes.data(), length, &length),
          "Cannot inspect fixture owner token");
  const auto sid =
      reinterpret_cast<const TOKEN_USER*>(bytes.constData())->User.Sid;
  require(IsValidSid(sid), "Fixture owner SID is invalid");
  QByteArray result(int(GetLengthSid(sid)), '\0');
  require(CopySid(DWORD(result.size()), result.data(), sid),
          "Cannot retain fixture owner SID");
  return result;
}

Handle currentToken(DWORD access) {
  HANDLE token = nullptr;
  require(OpenProcessToken(GetCurrentProcess(), access, &token),
          "Cannot open broker token");
  return Handle(token, &::CloseHandle);
}

class Impersonation final {
 public:
  explicit Impersonation(HANDLE token) {
    require(ImpersonateLoggedOnUser(token),
            "Cannot inspect fixtures as their owner");
  }
  ~Impersonation() {
    if (!RevertToSelf()) std::terminate();
  }
  Impersonation(const Impersonation&) = delete;
  Impersonation& operator=(const Impersonation&) = delete;
};

class Privilege final {
 public:
  explicit Privilege(const wchar_t* name)
      : m_token(currentToken(TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES)) {
    TOKEN_PRIVILEGES desired{};
    desired.PrivilegeCount = 1;
    require(LookupPrivilegeValueW(nullptr, name, &desired.Privileges[0].Luid),
            "The hosted runner lacks a required broker privilege");
    desired.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    DWORD size = sizeof(m_previous);
    SetLastError(ERROR_SUCCESS);
    require(AdjustTokenPrivileges(m_token.get(), FALSE, &desired, size,
                                  &m_previous, &size) &&
                GetLastError() == ERROR_SUCCESS,
            "Cannot enable an existing broker privilege");
  }
  ~Privilege() {
    if (!AdjustTokenPrivileges(m_token.get(), FALSE, &m_previous, 0, nullptr,
                               nullptr))
      std::terminate();
  }
  Privilege(const Privilege&) = delete;
  Privilege& operator=(const Privilege&) = delete;

 private:
  Handle m_token;
  TOKEN_PRIVILEGES m_previous{};
};

Handle openObject(const QString& path, DWORD access, bool follow = false) {
  const QString native = QDir::toNativeSeparators(path);
  DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
  if (!follow) flags |= FILE_FLAG_OPEN_REPARSE_POINT;
  const HANDLE raw = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),
                                 access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, flags, nullptr);
  require(raw != INVALID_HANDLE_VALUE,
          "Cannot pin a fixture filesystem object");
  return Handle(raw, &::CloseHandle);
}

BY_HANDLE_FILE_INFORMATION information(HANDLE object) {
  BY_HANDLE_FILE_INFORMATION result{};
  require(GetFileType(object) == FILE_TYPE_DISK &&
              GetFileInformationByHandle(object, &result),
          "Cannot inspect a fixture filesystem identity");
  return result;
}

void requirePrincipal(PSID actual, const QByteArray& expected) {
  require(IsValidSid(actual) &&
              EqualSid(actual, const_cast<char*>(expected.constData())),
          "A fixture object belongs to a different user");
}

void requireAce(const ACE_HEADER* header, const QByteArray& owner,
                const QByteArray& broker) {
  require(header->AceType == ACCESS_ALLOWED_ACE_TYPE &&
              !(header->AceFlags & (INHERITED_ACE | INHERIT_ONLY_ACE)),
          "Fixture permissions contain inherited or unsupported entries");
  const auto ace = reinterpret_cast<const ACCESS_ALLOWED_ACE*>(header);
  PSID subject = const_cast<DWORD*>(&ace->SidStart);
  if (EqualSid(subject, const_cast<char*>(owner.constData()))) return;
  if (IsWellKnownSid(subject, WinLocalSystemSid)) return;
  require(!broker.isEmpty() &&
              EqualSid(subject, const_cast<char*>(broker.constData())),
          "Fixture permissions grant an unexpected principal");
}

void requireAcl(HANDLE object, const QByteArray& owner,
                const QByteArray& broker = {}) {
  PSID actualOwner = nullptr;
  PACL acl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  require(
      GetSecurityInfo(object, SE_FILE_OBJECT,
                      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                      &actualOwner, nullptr, &acl, nullptr,
                      &descriptor) == ERROR_SUCCESS,
      "Cannot inspect fixture ownership and permissions");
  const LocalMemory memory(descriptor, &::LocalFree);
  requirePrincipal(actualOwner, owner);
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  require(GetSecurityDescriptorControl(descriptor, &control, &revision) &&
              (control & SE_DACL_PROTECTED) && acl && IsValidAcl(acl),
          "Fixture permissions must have an explicit protected DACL");
  for (DWORD index = 0; index < acl->AceCount; ++index) {
    void* entry = nullptr;
    require(GetAce(acl, index, &entry),
            "Cannot inspect a fixture permission entry");
    requireAce(static_cast<ACE_HEADER*>(entry), owner, broker);
  }
}

void requirePathComponents(const QString& path) {
  for (const QString& component : path.mid(3).split('/')) {
    require(!component.isEmpty() && !component.endsWith('.') &&
                !component.endsWith(' ') && !component.contains(':') &&
                !component.contains(QChar('\0')),
            "Fixture paths contain a Windows-normalized alias component");
  }
}

QString admittedRoot(const QString& stageRoot, const QString& fixtureRoot) {
  const QString stage = QDir::fromNativeSeparators(stageRoot);
  const QString fixture = QDir::fromNativeSeparators(fixtureRoot);
  require(QDir::isAbsolutePath(stage) && QDir::cleanPath(stage) == stage &&
              QDir::cleanPath(fixture) == fixture,
          "Fixture paths must be absolute without aliased components");
  require(fixture.startsWith(stage + "/temp/", Qt::CaseInsensitive),
          "Reparse fixtures must remain inside the new staged temporary tree");
  require(stage.size() >= 3 && stage[1] == ':' && stage[2] == '/',
          "Reparse fixtures require a local drive path");
  requirePathComponents(fixture);
  return fixture;
}

std::vector<Handle> pinDirectories(const QString& directory) {
  std::vector<Handle> pinned;
  QString path = directory.left(3);
  QStringList components = directory.mid(3).split('/', Qt::SkipEmptyParts);
  components.prepend(QString());
  for (const QString& component : components) {
    if (!component.isEmpty()) path = QDir(path).filePath(component);
    pinned.emplace_back(openObject(path, FILE_READ_ATTRIBUTES | READ_CONTROL));
    const auto info = information(pinned.back().get());
    require((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "Fixture ancestry contains a non-directory or reparse point");
  }
  return pinned;
}

struct Fixtures final {
  std::vector<Handle> directories;
  Handle file{nullptr, &::CloseHandle};
  BY_HANDLE_FILE_INFORMATION fileIdentity{};
  BY_HANDLE_FILE_INFORMATION directoryIdentity{};
};

Fixtures inspectFixtures(const QString& root, HANDLE ownerToken,
                         const QByteArray& broker) {
  const QByteArray owner = userSid(ownerToken);
  const Impersonation impersonation(ownerToken);
  Fixtures fixtures;
  fixtures.directories = pinDirectories(root);
  requireAcl(fixtures.directories.back().get(), owner, broker);
  fixtures.directories.emplace_back(
      openObject(root + "/link-target", FILE_READ_ATTRIBUTES | READ_CONTROL));
  const auto directory = information(fixtures.directories.back().get());
  require((directory.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
              !(directory.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
          "Fixture link target is not a regular directory");
  requireAcl(fixtures.directories.back().get(), owner);
  fixtures.directoryIdentity = directory;
  fixtures.file = openObject(root + "/link-target/session.json",
                             FILE_READ_ATTRIBUTES | READ_CONTROL);
  const auto file = information(fixtures.file.get());
  require(!(file.dwFileAttributes &
            (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
              file.nNumberOfLinks == 1,
          "Fixture link target is not a regular single-link file");
  requireAcl(fixtures.file.get(), owner);
  fixtures.fileIdentity = file;
  return fixtures;
}

LocalMemory ownerDescriptor(const QByteArray& owner) {
  LPWSTR encoded = nullptr;
  require(
      ConvertSidToStringSidW(const_cast<char*>(owner.constData()), &encoded),
      "Cannot encode fixture owner permissions");
  const LocalMemory encodedOwner(encoded, &::LocalFree);
  const QString text =
      "D:P(A;;FA;;;" + QString::fromWCharArray(encoded) + ")(A;;FA;;;SY)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
              reinterpret_cast<LPCWSTR>(text.utf16()), SDDL_REVISION_1,
              &descriptor, nullptr),
          "Cannot create fixture owner permissions");
  return LocalMemory(descriptor, &::LocalFree);
}

void transferLink(HANDLE link, const QByteArray& owner) {
  const auto descriptor = ownerDescriptor(owner);
  PACL acl = nullptr;
  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  require(
      GetSecurityDescriptorDacl(descriptor.get(), &present, &acl, &defaulted) &&
          present && acl,
      "Cannot inspect the new fixture permissions");
  require(
      SetSecurityInfo(link, SE_FILE_OBJECT,
                      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                          PROTECTED_DACL_SECURITY_INFORMATION,
                      const_cast<char*>(owner.constData()), nullptr, acl,
                      nullptr) == ERROR_SUCCESS,
      "Cannot transfer the opened reparse fixture to its owner");
  requireAcl(link, owner);
}

void requireSameObject(const BY_HANDLE_FILE_INFORMATION& expected,
                       const BY_HANDLE_FILE_INFORMATION& observed) {
  require(expected.dwVolumeSerialNumber == observed.dwVolumeSerialNumber &&
              expected.nFileIndexHigh == observed.nFileIndexHigh &&
              expected.nFileIndexLow == observed.nFileIndexLow,
          "The reparse fixture resolves to a different native object");
}

void createLink(const QString& root, const QString& name,
                const QByteArray& owner) {
  const bool directory = name == "directory-link";
  const QString destination = QDir::toNativeSeparators(root + '/' + name);
  const QString target = QDir::toNativeSeparators(
      root + (directory ? "/link-target" : "/link-target/session.json"));
  SetLastError(ERROR_SUCCESS);
  require(GetFileAttributesW(reinterpret_cast<LPCWSTR>(destination.utf16())) ==
                  INVALID_FILE_ATTRIBUTES &&
              GetLastError() == ERROR_FILE_NOT_FOUND,
          "The reparse fixture destination must not already exist");
  require(CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(destination.utf16()),
                              reinterpret_cast<LPCWSTR>(target.utf16()),
                              directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0),
          "Cannot create the fixed privileged reparse fixture");
  const auto link =
      openObject(destination,
                 FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_OWNER | WRITE_DAC);
  require(
      information(link.get()).dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT,
      "The created fixture is not a reparse object");
  transferLink(link.get(), owner);
}

void verifyTargets(const QString& root, const Fixtures& fixtures,
                   HANDLE ownerToken) {
  const Impersonation impersonation(ownerToken);
  const auto file =
      openObject(root + "/session-link.json", FILE_READ_ATTRIBUTES, true);
  const auto directory =
      openObject(root + "/directory-link", FILE_READ_ATTRIBUTES, true);
  requireSameObject(fixtures.fileIdentity, information(file.get()));
  requireSameObject(fixtures.directoryIdentity, information(directory.get()));
}

}  // namespace

QJsonObject createLiveReparseFixtures(const QString& stageRoot,
                                      const QString& fixtureRoot,
                                      HANDLE ownerToken) {
  require(
      qEnvironmentVariable("GITHUB_ACTIONS") == "true" &&
          qEnvironmentVariable("RUNNER_ENVIRONMENT") == "github-hosted" &&
          qEnvironmentVariable("RUNNER_OS") == "Windows",
      "The privileged fixture broker requires a hosted Windows Actions runner");
  const QString root = admittedRoot(stageRoot, fixtureRoot);
  const auto token = currentToken(TOKEN_QUERY);
  const QByteArray broker = userSid(token.get());
  const QByteArray owner = userSid(ownerToken);
  require(owner != broker,
          "Broker and fixture owner must be separate identities");
  const Fixtures fixtures = inspectFixtures(root, ownerToken, broker);
  const Privilege symbolicLink(L"SeCreateSymbolicLinkPrivilege");
  const Privilege restore(L"SeRestorePrivilege");
  createLink(root, "session-link.json", owner);
  createLink(root, "directory-link", owner);
  verifyTargets(root, fixtures, ownerToken);
  return {{"created", QJsonArray{"session-link.json", "directory-link"}},
          {"ownership_verified", true}};
}

}  // namespace GroundzeroWindowsLiveTest
#endif
