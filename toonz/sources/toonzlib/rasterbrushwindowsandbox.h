#pragma once
// Private Windows launch boundary. This is deliberately independent of the
// worker's Job Object, which limits resources but does not restrict privileges.
#ifdef _WIN32
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
#include <tlhelp32.h>
#include <set>
#include <vector>

namespace RasterBrush {
namespace {
class WindowsImportSandbox final {
  HANDLE importMutex   = nullptr;
  bool ownsImportMutex = false;
  PSID sid             = nullptr;

  bool acquireImportMutex(QString &error, const std::atomic_bool *cancelled) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    // All cooperating app instances, including different logon sessions/users,
    // serialize mutations of potentially shared executable/DLL DACLs. Grant
    // only wait/release rights; this handle is never inherited by the decoder.
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;0x00100001;;;AU)(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor,
            nullptr)) {
      error = "Cannot establish brush ACL serialization";
      return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    importMutex =
        CreateMutexExW(&attributes, L"Global\\Groundzero.BrushImportAcl.v1", 0,
                       SYNCHRONIZE | MUTEX_MODIFY_STATE);
    LocalFree(descriptor);
    if (!importMutex) {
      error = "Cannot open brush ACL serialization mutex";
      return false;
    }
    QElapsedTimer waiting;
    waiting.start();
    for (;;) {
      if (cancelled && cancelled->load(std::memory_order_relaxed)) return false;
      const DWORD result = WaitForSingleObject(importMutex, 50);
      if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
        ownsImportMutex = true;
        return true;
      }
      if (result != WAIT_TIMEOUT || waiting.elapsed() > 120000) {
        error = "Cannot acquire brush ACL serialization within 120 seconds";
        return false;
      }
    }
  }
  QString profile;
  QStringList grants;
  QTemporaryDir inputDirectory;
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

  // System and installed program files may already grant AppContainers RX.
  // Do not require WRITE_DAC on those files or broaden their existing ACLs.
  bool publiclyExecutable(const QString &path) {
    auto native = QDir::toNativeSeparators(path).toStdWString();
    PACL acl    = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl,
                              nullptr, &descriptor) != ERROR_SUCCESS)
      return false;
    BYTE storage[SECURITY_MAX_SID_SIZE];
    DWORD size   = sizeof(storage);
    bool allowed = false;
    if (acl &&
        CreateWellKnownSid(WinBuiltinAnyPackageSid, nullptr, storage, &size)) {
      TRUSTEE_W trustee{};
      BuildTrusteeWithSidW(&trustee, storage);
      ACCESS_MASK rights = 0;
      allowed =
          GetEffectiveRightsFromAclW(acl, &trustee, &rights) == ERROR_SUCCESS &&
          (rights & (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE)) ==
              (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
    }
    LocalFree(descriptor);
    return allowed;
  }

public:
  QString input;
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
    // The global mutex remains held through revocation. Remove only this
    // import's unique SID ACEs from the current DACL. External ACL editors do
    // not participate in our mutex and must not run concurrently with imports.
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
    if (importMutex) {
      if (ownsImportMutex && !ReleaseMutex(importMutex)) cleaned = false;
      CloseHandle(importMutex);
      importMutex     = nullptr;
      ownsImportMutex = false;
    }
    return cleaned;
  }

  bool prepare(const QString &executable, const QString &source,
               const QString &output, QString &error,
               const std::atomic_bool *cancelled = nullptr) {
    if (!acquireImportMutex(error, cancelled)) return false;
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
    std::set<QString> code{QFileInfo(executable).absoluteFilePath()};
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
      error = "Cannot enumerate trusted brush worker libraries";
      return false;
    }
    MODULEENTRY32W module{};
    module.dwSize         = sizeof(module);
    BOOL found            = Module32FirstW(snapshot, &module);
    const bool enumerated = found;
    while (found) {
      code.insert(QFileInfo(QString::fromWCharArray(module.szExePath))
                      .absoluteFilePath());
      found = Module32NextW(snapshot, &module);
    }
    CloseHandle(snapshot);
    if (!enumerated) {
      error = "Cannot enumerate trusted brush worker libraries";
      return false;
    }
    for (const QString &file : code)
      if (!publiclyExecutable(file) &&
          !grant(file, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE)) {
        error =
            "Cannot grant read-only access to a trusted brush worker library";
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
    for (const QString &key : {QString("SystemRoot"), QString("WINDIR"),
                               QString("PATH"), QString("QT_PLUGIN_PATH")})
      if (inheritedEnvironment.contains(key))
        environment.insert(key, inheritedEnvironment.value(key));
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
