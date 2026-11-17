#include "mcp_live_security_fixture.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>
#include <memory>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

QStringList files;
QStringList directories;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void requireEmptyDirectory(const QString& path) {
  const QFileInfo info(path);
  require(!info.isSymLink(), "Live fixture directory must not be a link");
  require(QDir().mkpath(path), "Cannot create live fixture directory");
  const auto entries =
      QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
  require(entries.isEmpty(), "Live fixture directory must be empty");
}

#ifdef _WIN32

using LocalMemory = std::unique_ptr<void, decltype(&::LocalFree)>;

QString currentSid() {
  HANDLE raw = nullptr;
  require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw),
          "Cannot inspect the fixture process user");
  const std::unique_ptr<void, decltype(&::CloseHandle)> token(raw, &::CloseHandle);
  DWORD length = 0;
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
  require(GetLastError() == ERROR_INSUFFICIENT_BUFFER && length > 0,
          "Cannot size the fixture user SID");
  std::vector<unsigned char> bytes(length);
  require(GetTokenInformation(token.get(), TokenUser, bytes.data(), length, &length),
          "Cannot read the fixture user SID");
  LPWSTR text = nullptr;
  require(
      ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(bytes.data())->User.Sid, &text),
      "Cannot encode the fixture user SID");
  const LocalMemory memory(text, &::LocalFree);
  return QString::fromWCharArray(text);
}

LocalMemory securityDescriptor(bool broad) {
  QString text = "D:P(A;;FA;;;SY)(A;;FA;;;" + currentSid() + ")";
  if (broad) text += "(A;;FR;;;WD)";
  PSECURITY_DESCRIPTOR raw = nullptr;
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
              reinterpret_cast<LPCWSTR>(text.utf16()), SDDL_REVISION_1, &raw, nullptr),
          "Cannot construct the fixture DACL");
  return LocalMemory(raw, &::LocalFree);
}

void applyDacl(const QString& path, bool broad, bool protectedAcl) {
  const auto descriptor = securityDescriptor(broad);
  PACL acl = nullptr;
  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  require(GetSecurityDescriptorDacl(descriptor.get(), &present, &acl, &defaulted) && present && acl,
          "Cannot inspect the fixture DACL");
  const DWORD protection =
      protectedAcl ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
  const auto native = QDir::toNativeSeparators(path).toStdWString();
  require(SetNamedSecurityInfoW(const_cast<LPWSTR>(native.c_str()), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | protection, nullptr, nullptr, acl,
                                nullptr) == ERROR_SUCCESS,
          "Cannot apply the fixture DACL");
}

void requireUnlinkedDirectory(const QString& path) {
  const auto native = QDir::toNativeSeparators(path).toStdWString();
  const DWORD attributes = GetFileAttributesW(native.c_str());
  require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
              (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
          "Live fixture directory must be a regular local directory");
}

QString privateDirectory(const QString& root, const QString& name) {
  const QString path = QDir(root).filePath(name);
  directories.append(path);
  prepareMcpLivePrivateDirectory(path);
  return path;
}

QString privateFile(const QString& directory, const QByteArray& contents) {
  const QString path = QDir(directory).filePath("session.json");
  const auto descriptor = securityDescriptor(false);
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
  const auto native = QDir::toNativeSeparators(path).toStdWString();
  const HANDLE raw = CreateFileW(native.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
  require(raw != INVALID_HANDLE_VALUE, "Cannot create private fixture session");
  const std::unique_ptr<void, decltype(&::CloseHandle)> file(raw, &::CloseHandle);
  files.append(path);
  DWORD written = 0;
  require(WriteFile(file.get(), contents.constData(), DWORD(contents.size()), &written, nullptr) &&
              written == DWORD(contents.size()),
          "Cannot write the private fixture session");
  return path;
}

QString symbolicFile(const QString& root, const QString& target) {
  const QString path = QDir(root).filePath("session-link.json");
  const auto native = QDir::toNativeSeparators(path).toStdWString();
  const auto destination = QDir::toNativeSeparators(target).toStdWString();
  require(CreateSymbolicLinkW(native.c_str(), destination.c_str(),
                              SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE),
          "Windows live security qualification requires file symlink creation");
  files.append(path);
  return path;
}

QString symbolicDirectory(const QString& root, const QString& target) {
  const QString path = QDir(root).filePath("directory-link");
  const auto native = QDir::toNativeSeparators(path).toStdWString();
  const auto destination = QDir::toNativeSeparators(target).toStdWString();
  require(CreateSymbolicLinkW(
              native.c_str(), destination.c_str(),
              SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE),
          "Windows live security qualification requires directory symlink creation");
  directories.append(path);
  return QDir(path).filePath("session.json");
}

QJsonObject variant(const QString& name, const QString& path) {
  return {{"name", name}, {"path", path}};
}

QJsonArray windowsVariants(const QString& root, const QByteArray& contents) {
  const auto unprotected = privateDirectory(root, "unprotected-file");
  const auto unprotectedFile = privateFile(unprotected, contents);
  applyDacl(unprotectedFile, false, false);
  const auto broad = privateDirectory(root, "broad-file");
  const auto broadFile = privateFile(broad, contents);
  applyDacl(broadFile, true, true);
  const auto parent = privateDirectory(root, "unprotected-parent");
  const auto parentFile = privateFile(parent, contents);
  applyDacl(parent, false, false);
  const auto target = privateDirectory(root, "link-target");
  const auto targetFile = privateFile(target, contents);
  QJsonArray result{variant("unprotected session DACL", unprotectedFile),
                    variant("Everyone-readable session DACL", broadFile),
                    variant("unprotected parent DACL", parentFile),
                    variant("reparse session file", symbolicFile(root, targetFile)),
                    variant("reparse directory ancestor", symbolicDirectory(root, target))};
  for (int index = 0; index < result.size(); ++index) {
    QJsonObject entry = result[index].toObject();
    entry.insert("baseline", targetFile);
    result[index] = entry;
  }
  return result;
}

#endif

}  // namespace

void prepareMcpLivePrivateDirectory(const QString& path) {
  require(QDir::isAbsolutePath(path), "Live fixture directory must be absolute");
  requireEmptyDirectory(path);
#ifdef _WIN32
  requireUnlinkedDirectory(path);
  applyDacl(path, false, true);
#else
  const auto native = QFile::encodeName(path);
  struct stat info{};
  require(
      lstat(native.constData(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == geteuid(),
      "Live fixture directory must be owned by the current user");
  require(chmod(native.constData(), 0700) == 0,
          "Cannot restrict the live fixture directory to its owner");
#endif
}

QJsonArray createMcpLiveInsecureFiles(const QString& sessionPath) {
  require(files.isEmpty() && directories.isEmpty(),
          "Previous live security variants must be removed first");
#ifdef _WIN32
  QFile session(sessionPath);
  require(session.open(QIODevice::ReadOnly) && session.size() <= 4096,
          "Cannot read the bounded live fixture grant");
  const QByteArray contents = session.readAll();
  require(session.error() == QFile::NoError, "Cannot read the live fixture grant");
  QTemporaryDir root(QDir::tempPath() + "/gz-live-security-XXXXXX");
  require(root.isValid(), "Cannot create disposable live security directory");
  root.setAutoRemove(false);
  directories.append(root.path());
  try {
    prepareMcpLivePrivateDirectory(root.path());
    return windowsVariants(root.path(), contents);
  } catch (...) {
    removeMcpLiveInsecureFiles();
    throw;
  }
#else
  Q_UNUSED(sessionPath);
  return {};
#endif
}

void removeMcpLiveInsecureFiles() {
  while (!files.isEmpty()) {
    const auto path = files.last();
    require(QFile::remove(path), "Cannot remove a token-bearing security fixture");
    files.removeLast();
  }
  while (!directories.isEmpty()) {
    const auto path = directories.last();
#ifdef _WIN32
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    require(RemoveDirectoryW(native.c_str()),
            "Cannot remove the private security fixture directory");
#else
    require(QDir().rmdir(path), "Cannot remove the private security fixture directory");
#endif
    directories.removeLast();
  }
}
