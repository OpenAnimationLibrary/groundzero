#pragma once
// Private Windows launch boundary. This is deliberately independent of the
// worker's Job Object, which limits resources but does not restrict privileges.
#ifdef _WIN32
#include "rasterbrushresponse.h"
#include "rasterbrushtestobserver.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <atomic>
#include <QUuid>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>
#include <shlobj.h>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <set>
#include <vector>

namespace RasterBrush {
namespace {
class WindowsImportSandbox final {
  PSID sid = nullptr;
  QString profile;
  QString localAppData;
  QStringList grants;
  QTemporaryDir inputDirectory;
  QTemporaryDir codeDirectory;
  std::vector<unsigned char> attributeStorage;
  STARTUPINFOEXW startup{};
  SECURITY_CAPABILITIES capabilities{};
  HANDLE inherited[3]{};

  bool changeAccess(const QString &path, DWORD rights, DWORD inheritance,
                    ACCESS_MODE mode) {
    auto native   = QDir::toNativeSeparators(path).toStdWString();
    PACL previous = nullptr, replacement = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              &previous, nullptr, &descriptor) != ERROR_SUCCESS)
      return false;
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = rights;
    access.grfAccessMode        = mode;
    access.grfInheritance       = inheritance;
    access.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType  = TRUSTEE_IS_UNKNOWN;
    access.Trustee.ptstrName    = reinterpret_cast<LPWSTR>(sid);
    DWORD status = SetEntriesInAclW(1, &access, previous, &replacement);
    if (status == ERROR_SUCCESS)
      status = SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
                                     DACL_SECURITY_INFORMATION, nullptr,
                                     nullptr, replacement, nullptr);
    if (replacement) LocalFree(replacement);
    LocalFree(descriptor);
    return status == ERROR_SUCCESS;
  }

  bool grant(const QString &path, DWORD rights, DWORD inheritance = 0) {
    if (!changeAccess(path, rights, inheritance, GRANT_ACCESS)) return false;
    grants.push_back(path);
    return true;
  }

  bool stageRuntime(const QString &executable, QString &error,
                    const std::atomic_bool *cancelled,
                    const QElapsedTimer &elapsed) {
    auto stopped = [&] {
      if (cancelled && cancelled->load(std::memory_order_relaxed)) return true;
      if (elapsed.elapsed() <= 120000) return false;
      error =
          "Brush import exceeded the 120-second limit during runtime staging";
      return true;
    };
    if (stopped()) return false;
    const QFileInfo requested(executable);
    const QString base      = requested.fileName().toLower();
    const bool test         = base == "rasterbrushworker_test.exe";
    const bool directHelper = base == "rasterbrushimport-helper.exe";
    const QDir runtime =
        (test || directHelper)
            ? requested.dir()
            : QDir(requested.dir().filePath("brush-import-runtime"));
    const QString name =
        test ? "rasterbrushworker_test.exe" : "rasterbrushimport-helper.exe";
    if (!requested.isExecutable() || !codeDirectory.isValid()) {
      error = "Cannot locate trusted brush helper runtime";
      return false;
    }
    const auto manifest =
        readBoundedRegularFile(runtime.filePath("runtime.files"), 65536, error);
    if (!error.isEmpty()) return false;
    // CMake file(APPEND) writes native CRLF on Windows. The bounded reader
    // deliberately returns binary bytes, so normalize only record delimiters;
    // stray carriage returns, whitespace and all path/hash checks stay strict.
    const auto entries = QByteArray(manifest).replace("\r\n", "\n").split('\n');
    if (entries.size() > 257) {
      error = "Brush helper runtime has too many files";
      return false;
    }
    static const QRegularExpression entryPattern(
        "^([a-f0-9]{64}) ([A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+){0,3})$");
    std::set<QString> names;
    qint64 total = 0;
    for (const auto &entry : entries) {
      if (entry.isEmpty()) continue;
      if (stopped()) return false;
      const auto match       = entryPattern.match(QString::fromLatin1(entry));
      const QString relative = match.captured(2);
      const auto parts       = relative.split('/');
      if (!match.hasMatch() || parts.contains(".") || parts.contains("..") ||
          !names.insert(relative.toLower()).second) {
        error = "Invalid trusted brush helper manifest";
        return false;
      }
      const qint64 remaining = 512LL * 1024 * 1024 - total;
      const QFileInfo source(runtime.filePath(relative));
      Detail::RegularResponseDirectory sourceDirectory;
      Detail::RegularResponseFile sourceFile;
      if (!sourceDirectory.open(source.absolutePath(), error) ||
          !sourceFile.open(sourceDirectory, source.fileName(),
                           std::min(remaining, qint64(128) * 1024 * 1024),
                           error))
        return false;
      const QString destination = codeDirectory.filePath(relative);
      if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        error = "Cannot create private brush helper runtime";
        return false;
      }
      QFile copy(destination);
      if (!copy.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        error = "Cannot stage private brush helper runtime";
        return false;
      }
      QCryptographicHash hash(QCryptographicHash::Sha256);
      qint64 copied = 0;
      while (copied < sourceFile.size()) {
        if (stopped()) return false;
        const auto bytes = sourceFile.read(
            std::min(qint64(65536), sourceFile.size() - copied), error);
        if (!error.isEmpty()) return false;
        if (copy.write(bytes) != bytes.size()) {
          error = "Cannot stage private brush helper runtime";
          return false;
        }
        hash.addData(bytes);
        copied += bytes.size();
#ifdef RASTERBRUSH_SANDBOX_TESTING
        notifyImportCancellationObserver("staging", {}, 0);
#endif
      }
      if (stopped() || !sourceFile.finish(error)) return false;
      if (hash.result().toHex() != match.captured(1).toLatin1()) {
        error = "Brush helper runtime changed or is incomplete";
        return false;
      }
      if (!copy.flush()) {
        error = "Cannot flush private brush helper runtime";
        return false;
      }
      total += copied;
    }
    if (!names.count(name)) {
      error = "Trusted brush runtime lacks its helper executable";
      return false;
    }
    program = codeDirectory.filePath(name);
    return true;
  }

public:
  QString input;
  QString program;
  QString launchError;
  WindowsImportSandbox()                                        = default;
  WindowsImportSandbox(const WindowsImportSandbox &)            = delete;
  WindowsImportSandbox &operator=(const WindowsImportSandbox &) = delete;
  ~WindowsImportSandbox() { close(); }

  bool close() {
    bool cleaned = true;
    if (startup.lpAttributeList)
      DeleteProcThreadAttributeList(startup.lpAttributeList);
    startup.lpAttributeList = nullptr;
    // Grants refer only to this import's private temporary directories/files.
    // Installed application files and their ACLs are never modified.
    for (auto it = grants.crbegin(); it != grants.crend(); ++it)
      if (!changeAccess(*it, 0, 0, REVOKE_ACCESS)) {
        qWarning("Cannot revoke expired brush AppContainer file permission");
        cleaned = false;
      }
    grants.clear();
    if (sid) {
      const auto name = profile.toStdWString();
      if (FAILED(DeleteAppContainerProfile(name.c_str()))) {
        qWarning("Cannot delete expired brush AppContainer profile");
        cleaned = false;
      }
      FreeSid(sid);
      sid = nullptr;
    }
    return cleaned;
  }

  bool prepare(const QString &executable, const QString &source,
               const QString &output, QString &error,
               const std::atomic_bool *cancelled = nullptr,
               const QElapsedTimer *elapsed      = nullptr) {
    QElapsedTimer localElapsed;
    localElapsed.start();
    if (!elapsed) elapsed = &localElapsed;
    // CreateProcess needs the host LocalAppData location to establish and
    // redirect the AppContainer profile environment. A stripped environment
    // without it fails with ERROR_ENVVAR_NOT_FOUND before the worker starts.
    // Resolve an OS known folder rather than importing arbitrary environment.
    PWSTR knownFolder = nullptr;
    const HRESULT folderResult = SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &knownFolder);
    if (SUCCEEDED(folderResult) && knownFolder)
      localAppData = QString::fromWCharArray(knownFolder);
    CoTaskMemFree(knownFolder);
    if (FAILED(folderResult) || localAppData.isEmpty()) {
      error = "Cannot resolve Windows LocalAppData for brush AppContainer";
      return false;
    }
    if (!stageRuntime(executable, error, cancelled, *elapsed)) return false;
    if (!inputDirectory.isValid()) {
      error = "Cannot create private brush input staging";
      return false;
    }
    // Copy bytes, never decode them in the privileged process. The shared
    // reader rejects reparse points, special files and oversized input before
    // a decoder or a blocking device read can run.
    const QByteArray bytes =
        readBoundedRegularFile(source, 64LL * 1024 * 1024, error);
    if (!error.isEmpty()) return false;
    input = inputDirectory.filePath(QFileInfo(source).fileName());
    QFile staged(input);
    if (!staged.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        staged.write(bytes) != bytes.size() || !staged.flush()) {
      error = "Cannot stage selected brush input";
      return false;
    }
    staged.close();
    profile = "Groundzero.Brush." + QUuid::createUuid().toString(QUuid::Id128);
    const auto name = profile.toStdWString();
    if (FAILED(CreateAppContainerProfile(name.c_str(), L"Brush import",
                                         L"Temporary isolated brush decoder",
                                         nullptr, 0, &sid))) {
      error = "Cannot create Windows brush AppContainer";
      return false;
    }
    // No capabilities: no network, webcam, shared user library, or registry
    // capability. Ordinary AppContainer OS-public resources remain available.
    capabilities.AppContainerSid = sid;
    if (!grant(inputDirectory.path(), FILE_GENERIC_READ | FILE_TRAVERSE) ||
        !grant(input, FILE_GENERIC_READ) ||
        !grant(output,
               FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_DELETE_CHILD,
               SUB_CONTAINERS_AND_OBJECTS_INHERIT)) {
      error = "Cannot grant private brush staging access to AppContainer";
      return false;
    }
    // AppContainers have low integrity. Only the disposable output directory
    // receives a low integrity label; the source and code remain read-only.
    PSECURITY_DESCRIPTOR label = nullptr;
    PACL sacl                  = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    auto outputNative = QDir::toNativeSeparators(output).toStdWString();
    bool labelled =
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"S:(ML;OICI;NW;;;LW)", SDDL_REVISION_1, &label, nullptr) &&
        GetSecurityDescriptorSacl(label, &present, &sacl, &defaulted) &&
        present &&
        SetNamedSecurityInfoW(outputNative.data(), SE_FILE_OBJECT,
                              LABEL_SECURITY_INFORMATION, nullptr, nullptr,
                              nullptr, sacl) == ERROR_SUCCESS;
    if (label) LocalFree(label);
    if (!labelled) {
      error = "Cannot establish brush staging integrity label";
      return false;
    }
    if (!grant(codeDirectory.path(), FILE_GENERIC_READ | FILE_GENERIC_EXECUTE,
               SUB_CONTAINERS_AND_OBJECTS_INHERIT)) {
      error = "Cannot grant read-only access to private brush helper runtime";
      return false;
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
    if (!size) {
      error = "Windows does not support isolated brush process attributes";
      return false;
    }
    attributeStorage.resize(size);
    auto attributes =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 2, 0, &size)) {
      error = "Cannot initialize isolated brush process attributes";
      return false;
    }
    startup.lpAttributeList = attributes;
    if (!UpdateProcThreadAttribute(
            attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
            &capabilities, sizeof(capabilities), nullptr, nullptr)) {
      error = "Cannot establish brush AppContainer process attributes";
      return false;
    }
    return true;
  }

  void configure(QProcess &process) {
    QProcessEnvironment environment;
    const auto inheritedEnvironment = QProcessEnvironment::systemEnvironment();
    // Do not pass credentials or application secrets through environment
    // variables. Qt and Windows loader paths are trusted parent configuration.
    for (const QString &key : {QString("SystemRoot"), QString("WINDIR")})
      if (inheritedEnvironment.contains(key))
        environment.insert(key, inheritedEnvironment.value(key));
    environment.insert("PATH",
                       codeDirectory.path() + ";" +
                           QDir(inheritedEnvironment.value("SystemRoot"))
                               .filePath("System32"));
    environment.insert("LOCALAPPDATA", localAppData);
    environment.insert("QT_PLUGIN_PATH", codeDirectory.path());
    process.setProcessEnvironment(environment);
    process.setCreateProcessArgumentsModifier(
        [this](QProcess::CreateProcessArguments *args) {
          startup.StartupInfo    = *args->startupInfo;
          startup.StartupInfo.cb = sizeof(startup);
          inherited[0]           = startup.StartupInfo.hStdInput;
          inherited[1]           = startup.StartupInfo.hStdOutput;
          inherited[2]           = startup.StartupInfo.hStdError;
          // Qt supplies only the standard pipe/NUL handles. Do not inherit
          // unrelated handles held by the drawing application.
          if (!UpdateProcThreadAttribute(
                  startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                  inherited, sizeof(inherited), nullptr, nullptr)) {
            launchError = "Cannot restrict brush worker inherited handles";
            startup.StartupInfo.cb = 0;  // CreateProcess must fail closed.
          }
          args->startupInfo = &startup.StartupInfo;
          args->flags |= EXTENDED_STARTUPINFO_PRESENT;
          args->inheritHandles = true;
        });
  }
};

bool isWindowsImportContainer() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  DWORD container = 0, returned = 0;
  const bool ok = GetTokenInformation(token, TokenIsAppContainer, &container,
                                      sizeof(container), &returned) &&
                  container;
  CloseHandle(token);
  return ok;
}
}  // namespace
}  // namespace RasterBrush
#endif
