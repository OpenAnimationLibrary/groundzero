#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "mcp_live_security_fixture.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <memory>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <aclapi.h>
#include <sddl.h>
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

LocalMemory securityDescriptor(bool broad, const QString& broker = {}) {
  QString text = "D:P(A;;FA;;;SY)(A;;FA;;;" + currentSid() + ")";
  if (broad) text += "(A;;FR;;;WD)";
  if (!broker.isEmpty()) text += "(A;;FA;;;" + broker + ")";
  PSECURITY_DESCRIPTOR raw = nullptr;
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
              reinterpret_cast<LPCWSTR>(text.utf16()), SDDL_REVISION_1, &raw,
              nullptr),
          "Cannot construct the fixture DACL");
  return LocalMemory(raw, &::LocalFree);
}

void applyDacl(const QString& path, bool broad, bool protectedAcl,
               const QString& broker = {}) {
  const auto descriptor = securityDescriptor(broad, broker);
  PACL acl              = nullptr;
  BOOL present          = FALSE;
  BOOL defaulted        = FALSE;
  require(
      GetSecurityDescriptorDacl(descriptor.get(), &present, &acl, &defaulted) &&
          present && acl,
      "Cannot inspect the fixture DACL");
  const DWORD protection = protectedAcl ? PROTECTED_DACL_SECURITY_INFORMATION
                                        : UNPROTECTED_DACL_SECURITY_INFORMATION;
  const auto native      = QDir::toNativeSeparators(path).toStdWString();
  require(
      SetNamedSecurityInfoW(const_cast<LPWSTR>(native.c_str()), SE_FILE_OBJECT,
                            DACL_SECURITY_INFORMATION | protection, nullptr,
                            nullptr, acl, nullptr) == ERROR_SUCCESS,
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

QJsonObject controlObject(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly) && file.size() <= 16384,
          "Cannot read bounded Windows fixture control");
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  require(file.error() == QFile::NoError &&
              error.error == QJsonParseError::NoError && document.isObject(),
          "Windows fixture control must be a valid JSON object");
  return document.object();
}

QString controlDirectory() {
  require(qEnvironmentVariable("GITHUB_ACTIONS") == "true" &&
              qEnvironmentVariable("RUNNER_ENVIRONMENT") == "github-hosted" &&
              qEnvironmentVariable("RUNNER_OS") == "Windows",
          "Windows live reparse fixtures require their hosted test launcher");
  const QString path = QDir::fromNativeSeparators(
      qEnvironmentVariable("GROUNDZERO_WINDOWS_LIVE_CONTROL"));
  const QFileInfo info(path);
  require(info.isAbsolute() && info.isDir() && !info.isSymLink() &&
              info.canonicalFilePath().compare(path, Qt::CaseInsensitive) == 0,
          "Windows fixture control must be its canonical staged directory");
  return path;
}

QString brokerIdentity(const QString& control) {
  const auto context = controlObject(QDir(control).filePath("context.json"));
  require(context.keys() == QStringList{"launcher_sid", "owner_logon_sha256",
                                        "owner_user_sha256", "protocol"} &&
              context.value("protocol") == "groundzero-windows-live-ci-v1",
          "Windows fixture launcher context is invalid");
  const QString broker = context.value("launcher_sid").toString();
  PSID brokerSid       = nullptr;
  require(ConvertStringSidToSidW(reinterpret_cast<LPCWSTR>(broker.utf16()),
                                 &brokerSid),
          "Windows fixture broker SID is invalid");
  const LocalMemory brokerMemory(brokerSid, &::LocalFree);
  PSID ownerSid       = nullptr;
  const QString owner = currentSid();
  require(ConvertStringSidToSidW(reinterpret_cast<LPCWSTR>(owner.utf16()),
                                 &ownerSid),
          "Windows fixture owner SID is invalid");
  const LocalMemory ownerMemory(ownerSid, &::LocalFree);
  const QByteArray bytes(static_cast<const char*>(ownerSid),
                         int(GetLengthSid(ownerSid)));
  const auto hash =
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
  require(!EqualSid(brokerSid, ownerSid) &&
              context.value("owner_user_sha256") == QString::fromLatin1(hash),
          "Windows fixture launcher does not describe the current owner");
  LPWSTR encoded = nullptr;
  require(ConvertSidToStringSidW(brokerSid, &encoded),
          "Cannot normalize broker SID");
  const LocalMemory encodedMemory(encoded, &::LocalFree);
  return QString::fromWCharArray(encoded);
}

void publishReparseRequest(const QString& control, const QString& root,
                           const QString& id) {
  const QString path = QDir(control).filePath("reparse-request.json");
  require(!QFileInfo::exists(path), "Reparse fixture request must be new");
  const QJsonObject request{{"protocol", "groundzero-windows-live-ci-v1"},
                            {"kind", "reparse"},
                            {"id", id},
                            {"fixture_root", root}};
  const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
  QSaveFile output(path);
  require(output.open(QIODevice::WriteOnly) &&
              output.write(bytes) == bytes.size() && output.commit(),
          "Cannot publish the fixed reparse fixture request");
}

void waitForReparseFixtures(const QString& control, const QString& id) {
  const QString path = QDir(control).filePath("reparse-response.json");
  QElapsedTimer deadline;
  deadline.start();
  while (!QFileInfo::exists(path) && deadline.elapsed() < 7000)
    QThread::msleep(20);
  require(QFileInfo::exists(path),
          "Privileged reparse fixture setup exceeded seven seconds");
  const auto response = controlObject(path);
  require(
      response.keys() ==
              QStringList{"created", "id", "ownership_verified", "status"} &&
          response.value("id") == id && response.value("status") == "passed" &&
          response.value("ownership_verified") == true &&
          response.value("created").toArray() ==
              QJsonArray{"session-link.json", "directory-link"},
      "Privileged reparse fixture setup failed its ownership checks");
}

void createReparseFixtures(const QString& root) {
  const QString control = controlDirectory();
  const QString broker  = brokerIdentity(control);
  files.append(QDir(root).filePath("session-link.json"));
  directories.append(QDir(root).filePath("directory-link"));
  applyDacl(root, false, true, broker);
  try {
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    publishReparseRequest(control, root, id);
    waitForReparseFixtures(control, id);
  } catch (...) {
    applyDacl(root, false, true);
    throw;
  }
  applyDacl(root, false, true);
}

QJsonObject variant(const QString& name, const QString& path) {
  return {{"name", name}, {"path", path}};
}

QJsonArray windowsVariants(const QString& root, const QByteArray& contents) {
  const auto unprotected     = privateDirectory(root, "unprotected-file");
  const auto unprotectedFile = privateFile(unprotected, contents);
  applyDacl(unprotectedFile, false, false);
  const auto broad     = privateDirectory(root, "broad-file");
  const auto broadFile = privateFile(broad, contents);
  applyDacl(broadFile, true, true);
  const auto parent     = privateDirectory(root, "unprotected-parent");
  const auto parentFile = privateFile(parent, contents);
  applyDacl(parent, false, false);
  const auto target     = privateDirectory(root, "link-target");
  const auto targetFile = privateFile(target, contents);
  createReparseFixtures(root);
  QJsonArray result{
      variant("unprotected session DACL", unprotectedFile),
      variant("Everyone-readable session DACL", broadFile),
      variant("unprotected parent DACL", parentFile),
      variant("reparse session file", QDir(root).filePath("session-link.json")),
      variant("reparse directory ancestor",
              QDir(root).filePath("directory-link/session.json"))};
  for (int index = 0; index < result.size(); ++index) {
    QJsonObject entry = result[index].toObject();
    entry.insert("baseline", targetFile);
    result[index] = entry;
  }
  return result;
}

void removeWindowsFixture(const QString& path, bool directory) {
  const auto native = QDir::toNativeSeparators(path).toStdWString();
  const bool removed = directory ? RemoveDirectoryW(native.c_str()) : DeleteFileW(native.c_str());
  const DWORD error = GetLastError();
  require(removed || error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND,
          "Cannot remove a private Windows security fixture");
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
#ifdef _WIN32
    removeWindowsFixture(path, false);
#else
    require(QFile::remove(path), "Cannot remove a token-bearing security fixture");
#endif
    files.removeLast();
  }
  while (!directories.isEmpty()) {
    const auto path = directories.last();
#ifdef _WIN32
    removeWindowsFixture(path, true);
#else
    require(QDir().rmdir(path), "Cannot remove the private security fixture directory");
#endif
    directories.removeLast();
  }
}
