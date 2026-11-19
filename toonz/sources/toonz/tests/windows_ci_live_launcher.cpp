#ifdef _WIN32
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <lm.h>
#include <sddl.h>
#include <userenv.h>
#include <winioctl.h>

#include "windows_ci_live_broker.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QStringList>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr auto protocol                 = "groundzero-windows-live-ci-v1";
constexpr DWORD transactionMilliseconds = 240000;
using Handle      = std::unique_ptr<void, decltype(&::CloseHandle)>;
using LocalMemory = std::unique_ptr<void, decltype(&::LocalFree)>;
using NetMemory   = std::unique_ptr<void, decltype(&::NetApiBufferFree)>;

struct Failure final : std::runtime_error {
  Failure(const char* operation, DWORD status)
      : std::runtime_error(operation), code(status) {}
  DWORD code;
};

void require(bool condition, const char* operation) {
  if (!condition) throw Failure(operation, ERROR_INVALID_DATA);
}

void checked(BOOL result, const char* operation) {
  if (!result) throw Failure(operation, GetLastError());
}

LPCWSTR wide(const QString& value) {
  return reinterpret_cast<LPCWSTR>(value.utf16());
}

Handle handle(HANDLE value, const char* operation) {
  if (!value || value == INVALID_HANDLE_VALUE)
    throw Failure(operation, GetLastError());
  return Handle(value, &::CloseHandle);
}

QString randomHex(int bytes) {
  QByteArray data(bytes, '\0');
  const NTSTATUS status =
      BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(data.data()),
                      ULONG(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status < 0) throw Failure("system_random", DWORD(status));
  return QString::fromLatin1(data.toHex());
}

QString hash(const QByteArray& value) {
  return QString::fromLatin1(
      QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex());
}

QByteArray tokenData(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
  DWORD size = 0;
  GetTokenInformation(token, kind, nullptr, 0, &size);
  require(GetLastError() == ERROR_INSUFFICIENT_BUFFER && size <= 65536 && size,
          "token_size");
  QByteArray result(int(size), '\0');
  checked(GetTokenInformation(token, kind, result.data(), size, &size),
          "token_read");
  return result;
}

QString sidText(PSID sid) {
  require(IsValidSid(sid), "invalid_sid");
  LPWSTR result = nullptr;
  checked(ConvertSidToStringSidW(sid, &result), "sid_encode");
  const LocalMemory memory(result, &::LocalFree);
  return QString::fromWCharArray(result);
}

struct Identity {
  QString sid;
  QString userHash;
  QString logonHash;
  bool elevated;
};

Identity identity(HANDLE token) {
  const auto user = tokenData(token, TokenUser);
  PSID sid = reinterpret_cast<const TOKEN_USER*>(user.constData())->User.Sid;
  const auto statistics = tokenData(token, TokenStatistics);
  const auto logon =
      reinterpret_cast<const TOKEN_STATISTICS*>(statistics.constData())
          ->AuthenticationId;
  const auto elevation = tokenData(token, TokenElevation);
  return {
      sidText(sid),
      hash(QByteArray(static_cast<const char*>(sid), int(GetLengthSid(sid)))),
      hash(QByteArray::number(qlonglong(logon.HighPart)) + ':' +
           QByteArray::number(qulonglong(logon.LowPart))),
      reinterpret_cast<const TOKEN_ELEVATION*>(elevation.constData())
              ->TokenIsElevated != 0};
}

Handle currentToken() {
  HANDLE token = nullptr;
  checked(OpenProcessToken(GetCurrentProcess(),
                           TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token),
          "parent_token");
  return handle(token, "parent_token");
}

void enablePrivilege(HANDLE token, LPCWSTR name) {
  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount           = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  checked(LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid),
          "privilege_lookup");
  SetLastError(ERROR_SUCCESS);
  checked(AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr),
          "privilege_enable");
  if (GetLastError() != ERROR_SUCCESS)
    throw Failure("privilege_unavailable", GetLastError());
}

void hostedGuard() {
  require(qEnvironmentVariable("GITHUB_ACTIONS") == "true" &&
              qEnvironmentVariable("RUNNER_ENVIRONMENT") == "github-hosted" &&
              qEnvironmentVariable("RUNNER_OS") == "Windows",
          "hosted_windows_required");
  for (const auto* name :
       {"GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_JOB"}) {
    const auto value = qEnvironmentVariable(name);
    require(!value.isEmpty() && value.size() <= 128, "missing_run_identity");
  }
  const auto token = currentToken();
  require(identity(token.get()).elevated, "elevated_parent_required");
  for (const auto* name :
       {SE_IMPERSONATE_NAME, SE_BACKUP_NAME, SE_RESTORE_NAME})
    enablePrivilege(token.get(), name);
}

QString canonical(const QString& value) {
  const QString path = QDir::fromNativeSeparators(value);
  require(QRegularExpression("^[A-Za-z]:/").match(path).hasMatch() &&
              QDir::cleanPath(path) == path && !path.contains(QChar('\0')),
          "local_absolute_path_required");
  require(GetDriveTypeW(wide(path.left(3))) == DRIVE_FIXED,
          "fixed_local_drive_required");
  QString current = path.left(3);
  for (const auto& part : path.mid(3).split('/', Qt::SkipEmptyParts)) {
    require(!part.contains(':') && !part.endsWith('.') && !part.endsWith(' '),
            "aliased_path_component");
    current           = QDir(current).filePath(part);
    const auto opened = handle(
        CreateFileW(wide(current), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                    nullptr),
        "path_open");
    BY_HANDLE_FILE_INFORMATION info{};
    checked(GetFileInformationByHandle(opened.get(), &info), "path_identity");
    require(!(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "reparse_path_rejected");
  }
  const QString result = QFileInfo(path).canonicalFilePath();
  require(!result.isEmpty(), "canonical_path_required");
  return QDir::fromNativeSeparators(result);
}

bool below(const QString& path, const QString& root) {
  return path.startsWith(root + '/', Qt::CaseInsensitive);
}

QString stagedPath(const QString& value, const QString& root) {
  const auto result = canonical(value);
  require(below(result, root), "path_outside_stage");
  return result;
}

QJsonObject readObject(const QString& path, qint64 maximum = 16384) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly) && file.size() > 0 &&
              file.size() <= maximum,
          "bounded_json_open");
  QJsonParseError error;
  const auto bytes = file.read(maximum + 1);
  require(bytes.size() <= maximum && file.atEnd(), "bounded_json_read_limit");
  require(file.error() == QFile::NoError &&
              QString::fromUtf8(bytes).toUtf8() == bytes,
          "bounded_json_read");
  const auto value = QJsonDocument::fromJson(bytes, &error);
  require(error.error == QJsonParseError::NoError && value.isObject(),
          "json_object_required");
  return value.object();
}

LocalMemory descriptor(const QString& sddl) {
  PSECURITY_DESCRIPTOR value = nullptr;
  checked(ConvertStringSecurityDescriptorToSecurityDescriptorW(
              wide(sddl), SDDL_REVISION_1, &value, nullptr),
          "security_descriptor");
  return LocalMemory(value, &::LocalFree);
}

QString privateAcl(const QString& launcher) {
  return "O:" + launcher + "D:P(A;;FA;;;SY)(A;;FA;;;" + launcher + ")";
}

BY_HANDLE_FILE_INFORMATION fileInformation(HANDLE file) {
  BY_HANDLE_FILE_INFORMATION value{};
  checked(GetFileInformationByHandle(file, &value), "file_identity");
  return value;
}

QJsonObject fileIdentity(HANDLE file) {
  const auto value = fileInformation(file);
  return {{"volume", double(value.dwVolumeSerialNumber)},
          {"index_high", double(value.nFileIndexHigh)},
          {"index_low", double(value.nFileIndexLow)}};
}

QJsonObject pathIdentity(const QString& path) {
  const auto file = handle(
      CreateFileW(wide(path), FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                  nullptr),
      "identity_open");
  return fileIdentity(file.get());
}

void setOwner(const QString& path, const QString& owner) {
  PSID raw = nullptr;
  checked(ConvertStringSidToSidW(wide(owner), &raw), "owner_sid_decode");
  const LocalMemory sid(raw, &::LocalFree);
  const auto status = SetNamedSecurityInfoW(
      const_cast<LPWSTR>(wide(path)), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION, raw, nullptr, nullptr, nullptr);
  if (status != ERROR_SUCCESS) throw Failure("set_fixture_owner", status);
}

void requireOwnedStageObject(const QString& path) {
  const auto token        = currentToken();
  const auto defaultOwner = tokenData(token.get(), TokenOwner);
  const auto allowed      = sidText(
      reinterpret_cast<const TOKEN_OWNER*>(defaultOwner.constData())->Owner);
  PSID owner               = nullptr;
  PSECURITY_DESCRIPTOR raw = nullptr;
  const auto status        = GetNamedSecurityInfoW(
      const_cast<LPWSTR>(wide(path)), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &raw);
  if (status != ERROR_SUCCESS) throw Failure("staged_owner_read", status);
  const LocalMemory security(raw, &::LocalFree);
  const auto observed = sidText(owner);
  require(observed == allowed || observed == identity(token.get()).sid,
          "staged_owner_mismatch");
  const auto opened = handle(
      CreateFileW(wide(path), FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                  nullptr),
      "staged_file_open");
  BY_HANDLE_FILE_INFORMATION info{};
  checked(GetFileInformationByHandle(opened.get(), &info),
          "staged_file_identity");
  require(!(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
          "staged_reparse_rejected");
  require((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
              info.nNumberOfLinks == 1,
          "staged_hardlink_rejected");
}

void setAcl(const QString& path, const QString& sddl) {
  const auto security = descriptor(sddl);
  PACL acl            = nullptr;
  BOOL present        = FALSE;
  BOOL defaulted      = FALSE;
  checked(GetSecurityDescriptorDacl(security.get(), &present, &acl, &defaulted),
          "read_dacl");
  require(present && acl, "explicit_dacl_required");
  const DWORD result = SetNamedSecurityInfoW(
      const_cast<LPWSTR>(wide(path)), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
      nullptr, acl, nullptr);
  if (result != ERROR_SUCCESS) throw Failure("set_private_dacl", result);
}

void writeBytes(HANDLE file, const QByteArray& bytes) {
  int offset = 0;
  while (offset < bytes.size()) {
    DWORD written = 0;
    checked(WriteFile(file, bytes.constData() + offset,
                      DWORD(bytes.size() - offset), &written, nullptr),
            "journal_write");
    require(written != 0, "journal_short_write");
    offset += int(written);
  }
  checked(FlushFileBuffers(file), "journal_flush");
}

void writeObject(const QString& path, const QJsonObject& value,
                 const QString& acl) {
  const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
  require(bytes.size() <= 1024 * 1024, "report_size");
  const auto security = descriptor(acl);
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.get(),
                                 FALSE};
  const auto temporary = path + ".new-" + randomHex(8);
  auto file = handle(CreateFileW(wide(temporary), GENERIC_WRITE, 0, &attributes,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr),
                     "journal_create");
  try {
    writeBytes(file.get(), bytes);
    file.reset();
    checked(MoveFileExW(wide(temporary), wide(path),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
            "journal_publish");
  } catch (...) {
    file.reset();
    DeleteFileW(wide(temporary));
    throw;
  }
}

QString accountSid(const QString& name) {
  LPBYTE data       = nullptr;
  const auto status = NetUserGetInfo(nullptr, wide(name), 4, &data);
  if (status != NERR_Success) throw Failure("account_identity", status);
  const NetMemory memory(data, &::NetApiBufferFree);
  return sidText(reinterpret_cast<const USER_INFO_4*>(data)->usri4_user_sid);
}

struct Secret final {
  std::array<wchar_t, 37> value{};
  Secret() {
    std::array<unsigned char, 32> bytes{};
    const auto status =
        BCryptGenRandom(nullptr, bytes.data(), ULONG(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) throw Failure("password_random", DWORD(status));
    constexpr wchar_t alphabet[] =
        L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::copy_n(L"Aa1!", 4, value.begin());
    for (size_t index = 0; index < bytes.size(); ++index)
      value[index + 4] = alphabet[bytes[index] & 63];
    SecureZeroMemory(bytes.data(), bytes.size());
  }
  ~Secret() { SecureZeroMemory(value.data(), sizeof(value)); }
  Secret(const Secret&)            = delete;
  Secret& operator=(const Secret&) = delete;
};

struct Account {
  explicit Account(QString role)
      : role(std::move(role)), name("gzl" + randomHex(8)) {}
  QString role;
  QString name;
  QString sid;
  Secret password;
  Handle token{nullptr, &::CloseHandle};
  HANDLE profile = nullptr;
  bool created   = false;
};

QJsonObject accountRecord(const Account& account) {
  return {{"role", account.role},
          {"name", account.name},
          {"sid", account.sid},
          {"created", account.created},
          {"profile_loaded", account.profile != nullptr}};
}

Handle logon(const Account& account) {
  HANDLE token = nullptr;
  checked(
      LogonUserW(wide(account.name), L".", account.password.value.data(),
                 LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &token),
      "standard_interactive_logon");
  auto result         = handle(token, "standard_interactive_logon");
  const auto observed = identity(result.get());
  require(!observed.elevated && observed.sid == account.sid,
          "standard_token_identity");
  return result;
}

void loadProfile(Account& account) {
  PROFILEINFOW profile{};
  profile.dwSize     = sizeof(profile);
  profile.dwFlags    = PI_NOUI;
  profile.lpUserName = const_cast<LPWSTR>(wide(account.name));
  checked(LoadUserProfileW(account.token.get(), &profile),
          "load_created_profile");
  account.profile = profile.hProfile;
}

class Impersonation final {
public:
  explicit Impersonation(HANDLE token) {
    checked(ImpersonateLoggedOnUser(token), "impersonate_created_owner");
  }
  ~Impersonation() {
    if (!RevertToSelf())
      TerminateProcess(GetCurrentProcess(), ERROR_CANNOT_IMPERSONATE);
  }
};

struct Desktop final {
  HWINSTA station  = nullptr;
  HWINSTA previous = nullptr;
  HDESK desktop    = nullptr;
  QString name;
  ~Desktop() {
    if (desktop) CloseDesktop(desktop);
    if (station) CloseWindowStation(station);
  }
  void create(const QString& acl, const QString& nonce) {
    const auto security = descriptor(acl);
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.get(),
                                   FALSE};
    const auto stationName = "gz-live-" + nonce;
    const auto desktopName = "desk-" + nonce;
    previous               = GetProcessWindowStation();
    checked(previous != nullptr, "read_parent_station");
    station = CreateWindowStationW(wide(stationName), CWF_CREATE_ONLY,
                                   WINSTA_ALL_ACCESS, &attributes);
    checked(station != nullptr, "create_private_station");
    checked(SetProcessWindowStation(station), "select_private_station");
    desktop           = CreateDesktopW(wide(desktopName), nullptr, nullptr, 0,
                                       GENERIC_ALL, &attributes);
    const auto status = GetLastError();
    checked(SetProcessWindowStation(previous), "restore_parent_station");
    if (!desktop) throw Failure("create_private_desktop", status);
    name = stationName + '\\' + desktopName;
  }
};

void exactKeys(const QJsonObject& object, QStringList keys) {
  keys.sort();
  require(object.keys() == keys, "unexpected_json_fields");
}

QString required(const QJsonObject& object, const QString& key) {
  const auto value = object.value(key);
  require(value.isString() && !value.toString().isEmpty() &&
              !value.toString().contains(QChar('\0')),
          "required_string");
  return value.toString();
}

QString fileHash(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "artifact_hash_open");
  QCryptographicHash digest(QCryptographicHash::Sha256);
  require(digest.addData(&file), "artifact_hash_read");
  return QString::fromLatin1(digest.result().toHex());
}

struct Configuration {
  QString root;
  QString launcherSid;
  QString output;
  QJsonObject files;
  QString stuff;
  QString mesa;
};

QJsonObject preparedProfile(const QString& file) {
  const auto value = readObject(file, 1024 * 1024);
  exactKeys(value, {"status", "qualification_executed",
                    "identity_mutations_enabled", "protocol", "files", "sha256",
                    "stuff", "windows_mesa", "packages"});
  require(value.value("protocol") == protocol &&
              value.value("status") == "prepared" &&
              value.value("qualification_executed") == false &&
              value.value("identity_mutations_enabled") == false,
          "prepared_profile_required");
  return value;
}

Configuration configuration(const QString& path, const QString& output) {
  const auto file      = canonical(path);
  const auto root      = QFileInfo(file).path();
  const auto temporary = canonical(qEnvironmentVariable("RUNNER_TEMP"));
  require(
      below(root, temporary) && QFileInfo(file).fileName() == "prepared.json",
      "prepared_stage_required");
  require(
      QDir::fromNativeSeparators(output) == root + "/identity-report.json" &&
          !QFileInfo::exists(output),
      "new_identity_report_required");
  for (const auto* name : {"transaction.json", "temp", "evidence", "control"})
    require(!QFileInfo::exists(root + '/' + name),
            "unused_staging_root_required");
  requireOwnedStageObject(root);
  const auto value  = preparedProfile(file);
  auto files        = value.value("files").toObject();
  const auto hashes = value.value("sha256").toObject();
  const QStringList names{"adapter", "client", "executable", "helper",
                          "node",    "python", "suite"};
  exactKeys(files, names);
  exactKeys(hashes, names);
  for (const auto& name : names) {
    const auto artifact = stagedPath(required(files, name), root);
    require(QFileInfo(artifact).isFile() &&
                fileHash(artifact) == required(hashes, name),
            "staged_artifact_hash");
    files.insert(name, artifact);
  }
  require(
      QFileInfo(files.value("suite").toString()).fileName() == "mcp_live.py" &&
          QFileInfo(files.value("helper").toString()).fileName() ==
              "windows_ci_live_test.exe",
      "fixed_test_entries_required");
  return {root,
          identity(currentToken().get()).sid,
          output,
          files,
          stagedPath(required(value, "stuff"), root),
          stagedPath(required(value, "windows_mesa"), root)};
}

class Journal final {
public:
  explicit Journal(const Configuration& config)
      : path(config.root + "/transaction.json")
      , acl(privateAcl(config.launcherSid)) {
    require(!QFileInfo::exists(path), "new_transaction_required");
    value = {{"protocol", protocol},
             {"run_id", qEnvironmentVariable("GITHUB_RUN_ID")},
             {"attempt", qEnvironmentVariable("GITHUB_RUN_ATTEMPT")},
             {"job", qEnvironmentVariable("GITHUB_JOB")},
             {"launcher_sid", config.launcherSid},
             {"stage_root", config.root},
             {"stage_identity", pathIdentity(config.root)},
             {"nonce", randomHex(16)},
             {"accounts", QJsonArray{}},
             {"state", "prepared"}};
    save();
  }
  void save() { writeObject(path, value, acl); }
  void accounts(const Account& first, const Account& second) {
    value.insert("accounts",
                 QJsonArray{accountRecord(first), accountRecord(second)});
    save();
  }
  QString path;
  QString acl;
  QJsonObject value;
};

void createAccount(Account& account, const std::function<void()>& save) {
  save();
  USER_INFO_1 user{};
  user.usri1_name     = const_cast<LPWSTR>(wide(account.name));
  user.usri1_password = account.password.value.data();
  user.usri1_priv     = USER_PRIV_USER;
  user.usri1_flags    = UF_SCRIPT | UF_NORMAL_ACCOUNT | UF_DONT_EXPIRE_PASSWD;
  DWORD parameter     = 0;
  const auto status =
      NetUserAdd(nullptr, 1, reinterpret_cast<LPBYTE>(&user), &parameter);
  if (status != NERR_Success) throw Failure("create_standard_account", status);
  account.created = true;
  save();
  account.sid = accountSid(account.name);
  save();
  account.token = logon(account);
  loadProfile(account);
  save();
}

bool ownerWritable(const QString& root, const QString& path) {
  for (const auto* name : {"control", "temp", "evidence"}) {
    const auto directory = root + '/' + name;
    if (path == directory || below(path, directory)) return true;
  }
  return false;
}

QString probeRights(const Configuration& config, const QString& path) {
  const auto control = config.root + "/control";
  const auto helper  = config.files.value("helper").toString();
  const QStringList ancestors{config.root, control, control + "/probes",
                              QFileInfo(helper).path()};
  if (ancestors.contains(path)) return "0x20";
  const auto output = control + "/probes/wrong-user";
  if (path == output || below(path, output)) return "FA";
  if (path == helper) return "GRGX";
  if (QFileInfo(path).suffix().compare("dll", Qt::CaseInsensitive) != 0)
    return {};
  return QFileInfo(path).path() == QFileInfo(helper).path() ? "GRGX"
                                                            : QString{};
}

QString roleAcl(const Configuration& config, const Account& owner,
                const Account& other, const QString& path) {
  QString acl          = privateAcl(config.launcherSid);
  const QString rights = ownerWritable(config.root, path) ? "FA" : "GRGX";
  acl += "(A;;" + rights + ";;;" + owner.sid + ")";
  const auto otherRights = probeRights(config, path);
  if (!otherRights.isEmpty())
    acl += "(A;;" + otherRights + ";;;" + other.sid + ")";
  return acl;
}

void prepareDirectories(const Configuration& config, const Account& owner,
                        const Account& other, bool& temporaryCreated,
                        ULONGLONG deadline) {
  for (const auto* name :
       {"temp", "evidence", "control", "control/probes",
        "control/probes/wrong-user", "control/probes/wrong-logon"}) {
    const auto path = config.root + '/' + name;
    require(!QFileInfo::exists(path) && QDir().mkdir(path),
            "new_private_directory");
    if (QString::fromLatin1(name) == "temp") temporaryCreated = true;
  }
  QDirIterator iterator(
      config.root,
      QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
      QDirIterator::Subdirectories);
  int entries = 0;
  while (iterator.hasNext()) {
    require(GetTickCount64() < deadline, "staging_deadline");
    require(++entries <= 100000, "staging_entry_limit");
    const auto path = canonical(iterator.next());
    require(below(path, config.root), "staging_escape");
    if (path == config.root + "/transaction.json") continue;
    requireOwnedStageObject(path);
    setAcl(path, roleAcl(config, owner, other, path));
  }
  setAcl(config.root, roleAcl(config, owner, other, config.root));
  setOwner(config.root + "/temp", owner.sid);
}

std::vector<wchar_t> environment(HANDLE token, const Configuration& config) {
  void* raw = nullptr;
  checked(CreateEnvironmentBlock(&raw, token, FALSE), "owner_environment");
  const std::unique_ptr<void, decltype(&::DestroyEnvironmentBlock)> block(
      raw, &::DestroyEnvironmentBlock);
  QMap<QString, QString> values;
  for (auto* entry = static_cast<const wchar_t*>(raw); *entry;
       entry += wcslen(entry) + 1) {
    const auto text = QString::fromWCharArray(entry);
    const int split = text.indexOf('=', 1);
    if (split > 0)
      values.insert(text.left(split).toUpper(), text.mid(split + 1));
  }
  for (const auto* name :
       {"GITHUB_ACTIONS", "RUNNER_ENVIRONMENT", "RUNNER_OS", "RUNNER_TEMP",
        "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_JOB"})
    values.insert(name, qEnvironmentVariable(name));
  values.insert("TEMP", QDir::toNativeSeparators(config.root + "/temp"));
  values.insert("TMP", QDir::toNativeSeparators(config.root + "/temp"));
  values.insert("GROUNDZERO_WINDOWS_LIVE_CONTROL", config.root + "/control");
  values.insert("QT_QPA_PLATFORM", "windows");
  std::wstring result;
  for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
    result += (it.key() + '=' + it.value()).toStdWString();
    result.push_back(L'\0');
  }
  result.push_back(L'\0');
  return {result.begin(), result.end()};
}

QString quote(const QString& value) {
  require(!value.contains(QChar('\0')) && !value.contains('"') &&
              !value.endsWith('\\'),
          "fixed_argument_encoding");
  return '"' + value + '"';
}

struct Process final {
  Handle process{nullptr, &::CloseHandle};
  Handle thread{nullptr, &::CloseHandle};
};

class Job final {
public:
  Job(const QString& name, const QString& acl) {
    const auto security = descriptor(acl);
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.get(),
                                   FALSE};
    SetLastError(ERROR_SUCCESS);
    object = handle(CreateJobObjectW(&attributes, wide(name)),
                    "create_containment_job");
    require(GetLastError() != ERROR_ALREADY_EXISTS, "job_collision");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    checked(
        SetInformationJobObject(object.get(), JobObjectExtendedLimitInformation,
                                &limits, sizeof(limits)),
        "job_kill_on_close");
  }
  DWORD active() const {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION value{};
    checked(QueryInformationJobObject(object.get(),
                                      JobObjectBasicAccountingInformation,
                                      &value, sizeof(value), nullptr),
            "job_process_count");
    return value.ActiveProcesses;
  }
  void terminate() {
    checked(TerminateJobObject(object.get(), ERROR_PROCESS_ABORTED),
            "terminate_owned_job");
  }
  void stopUnassigned() {
    if (!unassigned) return;
    if (WaitForSingleObject(unassigned.get(), 0) != WAIT_OBJECT_0)
      checked(TerminateProcess(unassigned.get(), ERROR_PROCESS_ABORTED),
              "terminate_unassigned_child");
    require(WaitForSingleObject(unassigned.get(), 5000) == WAIT_OBJECT_0,
            "unassigned_child_remains");
    unassigned.reset();
  }
  void waitEmpty() const {
    const auto deadline = GetTickCount64() + 5000;
    while (active() && GetTickCount64() < deadline) Sleep(20);
    require(active() == 0, "owned_descendants_remain");
  }
  Handle object{nullptr, &::CloseHandle};
  Handle unassigned{nullptr, &::CloseHandle};
};

struct Launch {
  HANDLE token;
  const Configuration& config;
  const Desktop& desktop;
  Job& job;
  Journal& journal;
};

Process spawn(const Launch& launch, const QString& executable,
              const QStringList& arguments) {
  QStringList quoted{quote(executable)};
  for (const auto& argument : arguments) quoted.append(quote(argument));
  std::wstring command = quoted.join(' ').toStdWString();
  require(command.size() < 1024, "fixed_command_line_limit");
  auto variables = environment(launch.token, launch.config);
  STARTUPINFOW startup{};
  startup.cb        = sizeof(startup);
  startup.lpDesktop = const_cast<LPWSTR>(wide(launch.desktop.name));
  PROCESS_INFORMATION process{};
  launch.journal.value.insert("spawn_pending", true);
  launch.journal.save();
  checked(CreateProcessWithTokenW(
              launch.token, 0, wide(executable), command.data(),
              CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
              variables.data(), wide(launch.config.root), &startup, &process),
          "launch_fixed_test_child");
  Process result{handle(process.hProcess, "child_process"),
                 handle(process.hThread, "child_thread")};
  if (!AssignProcessToJobObject(launch.job.object.get(),
                                result.process.get())) {
    const auto status = GetLastError();
    require(!launch.job.unassigned, "unassigned_child_already_retained");
    launch.job.unassigned = std::move(result.process);
    launch.job.stopUnassigned();
    throw Failure("assign_owned_job", status);
  }
  launch.journal.value.insert("spawn_pending", false);
  launch.journal.save();
  if (ResumeThread(result.thread.get()) == DWORD(-1)) {
    const auto status = GetLastError();
    launch.job.terminate();
    launch.job.waitEmpty();
    throw Failure("resume_owned_child", status);
  }
  return result;
}

DWORD waitProcess(const Process& process, DWORD milliseconds) {
  const auto wait = WaitForSingleObject(process.process.get(), milliseconds);
  if (wait != WAIT_OBJECT_0) throw Failure("fixed_child_deadline", wait);
  DWORD code = 0;
  checked(GetExitCodeProcess(process.process.get(), &code), "fixed_child_exit");
  return code;
}

void requireSuccessfulExit(const Process& process, DWORD milliseconds,
                           const char* operation) {
  const auto status = waitProcess(process, milliseconds);
  if (status != 0) throw Failure(operation, status);
}

QJsonObject safeFailure(const Failure& error) {
  return {{"operation", QString::fromLatin1(error.what()).left(128)},
          {"win32_status", double(error.code)}};
}

void removeTree(const QString& path, int& entries, int depth = 0) {
  require(++entries <= 100000, "temporary_cleanup_entry_limit");
  require(depth <= 128, "temporary_cleanup_depth_limit");
  const auto attributes = GetFileAttributesW(wide(path));
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    require(GetLastError() == ERROR_FILE_NOT_FOUND ||
                GetLastError() == ERROR_PATH_NOT_FOUND,
            "temporary_cleanup_attributes");
    return;
  }
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    checked(DeleteFileW(wide(path)), "temporary_cleanup_file");
    return;
  }
  if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    const auto children = QDir(path).entryList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const auto& child : children)
      removeTree(QDir(path).filePath(child), entries, depth + 1);
  }
  checked(RemoveDirectoryW(wide(path)), "temporary_cleanup_directory");
}

void verifyHelperIdentity(const QJsonObject& report, const Identity& expected) {
  require(report.value("status") == "passed", "identity_helper_failure");
  const auto value = report.value("identity").toObject();
  require(value.value("user_sha256") == expected.userHash &&
              value.value("logon_sha256") == expected.logonHash &&
              value.value("elevated") == false,
          "identity_helper_token_mismatch");
}

void deleteOwnedProfile(const QString& sid) {
  if (DeleteProfileW(wide(sid), nullptr, nullptr)) return;
  const auto status = GetLastError();
  if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND)
    throw Failure("delete_created_profile", status);
}

void requireAccountAbsent(const QString& name) {
  LPBYTE raw        = nullptr;
  const auto status = NetUserGetInfo(nullptr, wide(name), 0, &raw);
  const NetMemory information(raw, &::NetApiBufferFree);
  if (status == NERR_UserNotFound) return;
  if (status == NERR_Success)
    throw Failure("cleanup_account_creation_uncertain", ERROR_INVALID_DATA);
  throw Failure("cleanup_account_presence", status);
}

void deleteOwnedAccount(Account& account) {
  if (!account.created) {
    requireAccountAbsent(account.name);
    return;
  }
  require(!account.sid.isEmpty() && accountSid(account.name) == account.sid,
          "cleanup_account_identity_changed");
  if (account.profile) {
    checked(UnloadUserProfile(account.token.get(), account.profile),
            "unload_created_profile");
    account.profile = nullptr;
  }
  account.token.reset();
  deleteOwnedProfile(account.sid);
  const auto status = NetUserDel(nullptr, wide(account.name));
  if (status != NERR_Success) throw Failure("delete_created_account", status);
  account.created = false;
}

QJsonObject cleanupBoundaries(const Configuration& config, const Account& owner,
                              const Account& other, Journal& journal);

class Transaction final {
public:
  explicit Transaction(Configuration config)
      : m_config(std::move(config))
      , m_owner("owner")
      , m_other("other")
      , m_journal(m_config) {}

  int run() {
    bool passed = false;
    try {
      prepare();
      qualify();
      passed = true;
    } catch (const Failure& error) {
      m_report.insert("failure", safeFailure(error));
    } catch (...) {
      m_report.insert(
          "failure", QJsonObject{{"operation", "unexpected_launcher_failure"}});
    }
    const auto errors = cleanup();
    m_report.insert("cleanup_errors", errors);
    m_report.insert("cleanup_succeeded", errors.isEmpty());
    m_report.insert("qualification_executed", m_executed);
    m_report.insert("identity_mutations_enabled", true);
    m_report.insert("status", passed && errors.isEmpty() ? "passed" : "failed");
    writeObject(m_config.output, m_report, privateAcl(m_config.launcherSid));
    return passed && errors.isEmpty() ? 0 : 1;
  }

private:
  void saveAccounts() { m_journal.accounts(m_owner, m_other); }

  void prepare() {
    const auto nonce   = m_journal.value.value("nonce").toString();
    const auto jobName = "Local\\groundzero-live-ci-" + nonce;
    m_journal.value.insert("job_name", jobName);
    m_journal.save();
    m_job = std::make_unique<Job>(jobName, privateAcl(m_config.launcherSid));
    createAccount(m_owner, [this] { saveAccounts(); });
    createAccount(m_other, [this] { saveAccounts(); });
    m_ownerIdentity = identity(m_owner.token.get());
    m_otherIdentity = identity(m_other.token.get());
    m_otherLogon    = logon(m_owner);
    m_logonIdentity = identity(m_otherLogon.get());
    require(m_ownerIdentity.sid != m_otherIdentity.sid &&
                m_ownerIdentity.sid == m_logonIdentity.sid &&
                m_ownerIdentity.logonHash != m_logonIdentity.logonHash,
            "independent_test_identities_required");
    prepareDirectories(m_config, m_owner, m_other, m_tempCreated,
                       m_started + transactionMilliseconds);
    m_journal.value.insert("temp_created", m_tempCreated);
    m_journal.value.insert("temp_identity",
                           pathIdentity(m_config.root + "/temp"));
    m_journal.save();
    m_report.insert("cleanup_boundaries",
                    cleanupBoundaries(m_config, m_owner, m_other, m_journal));
    const auto desktopAcl = "D:P(A;;GA;;;SY)(A;;GA;;;" + m_config.launcherSid +
                            ")(A;;GA;;;" + m_owner.sid + ")(A;;GA;;;" +
                            m_other.sid + ")";
    m_desktop.create(desktopAcl, nonce);
    m_journal.value.insert("state", "running");
    m_journal.save();
    establishOwner();
  }

  Launch launch(HANDLE token) {
    return {token, m_config, m_desktop, *m_job, m_journal};
  }

  QString artifact(const QString& name) const {
    return m_config.files.value(name).toString();
  }

  QString control(const QString& name) const {
    return m_config.root + "/control/" + name;
  }

  QJsonObject ownerRead(const QString& path, qint64 maximum = 16384) const {
    const Impersonation owner(m_owner.token.get());
    return readObject(stagedPath(path, m_config.root), maximum);
  }

  DWORD remainingMilliseconds(DWORD maximum) const {
    const auto elapsed = GetTickCount64() - m_started;
    require(elapsed < transactionMilliseconds, "qualification_deadline");
    return DWORD(
        std::min<ULONGLONG>(maximum, transactionMilliseconds - elapsed));
  }

  void establishOwner() {
    const auto path = control("owner.json");
    auto process    = spawn(launch(m_owner.token.get()), artifact("helper"),
                            {"--mode", "identity", "--output", path});
    requireSuccessfulExit(process, remainingMilliseconds(10000),
                          "owner_identity_helper_exit");
    const auto report = ownerRead(path);
    verifyHelperIdentity(report, m_ownerIdentity);
    m_report.insert("owner", report.value("identity"));
    writeObject(
        control("context.json"),
        {{"protocol", protocol},
         {"launcher_sid", m_config.launcherSid},
         {"owner_user_sha256", m_ownerIdentity.userHash},
         {"owner_logon_sha256", m_ownerIdentity.logonHash}},
        privateAcl(m_config.launcherSid) + "(A;;GR;;;" + m_owner.sid + ")");
  }

  QStringList suiteArguments() const {
    return {
        artifact("suite"), "--executable",     artifact("executable"),
        "--stuff",         m_config.stuff,     "--node",
        artifact("node"),  "--adapter",        artifact("adapter"),
        "--client",        artifact("client"), "--windows-mesa",
        m_config.mesa,     "--output",         m_config.root + "/evidence/gui"};
  }

  void qualify() {
    remainingMilliseconds(transactionMilliseconds);
    auto process = spawn(launch(m_owner.token.get()), artifact("python"),
                         suiteArguments());
    m_executed   = true;
    while (WaitForSingleObject(process.process.get(), 20) == WAIT_TIMEOUT) {
      remainingMilliseconds(transactionMilliseconds);
      serviceRequest("peer");
      serviceRequest("reparse");
    }
    requireSuccessfulExit(process, 0, "native_sdk_journey_exit");
    m_job->waitEmpty();
    require(m_peerDone && m_reparseDone, "identity_boundaries_not_exercised");
    const auto report = ownerRead(m_config.root + "/evidence/gui/report.json",
                                  16 * 1024 * 1024);
    require(report.value("status") == "passed" &&
                report.value("native_exit_code") == 0 &&
                report.value("process_tree_stopped") == true,
            "native_sdk_evidence_failed");
    m_report.insert("native_and_sdk_passed", true);
    m_report.insert("descendants_stopped_naturally", true);
    m_report.insert("duration_ms", double(GetTickCount64() - m_started));
  }

  QJsonObject readRequest(const QString& kind) const {
    const auto request = ownerRead(control(kind + "-request.json"));
    const QStringList common{"protocol", "kind", "id"};
    auto fields = common;
    fields.append(kind == "peer" ? QStringList{"session_path", "endpoint"}
                                 : QStringList{"fixture_root"});
    exactKeys(request, fields);
    require(
        request.value("protocol") == protocol &&
            request.value("kind") == (kind == "peer" ? "identity" : "reparse"),
        "fixed_control_protocol");
    require(
        QRegularExpression(
            "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
            .match(required(request, "id"))
            .hasMatch(),
        "fixed_control_identity");
    return request;
  }

  void serviceRequest(const QString& kind) {
    bool& complete = kind == "peer" ? m_peerDone : m_reparseDone;
    if (complete) return;
    {
      const Impersonation owner(m_owner.token.get());
      if (!QFileInfo::exists(control(kind + "-request.json"))) return;
    }
    const auto request = readRequest(kind);
    QJsonObject result;
    try {
      result = kind == "peer" ? peerProbes(request) : reparseProbe(request);
    } catch (...) {
      writeObject(
          control(kind + "-response.json"),
          {{"id", request.value("id")}, {"status", "failed"}},
          privateAcl(m_config.launcherSid) + "(A;;GR;;;" + m_owner.sid + ")");
      throw;
    }
    result.insert("id", request.value("id"));
    result.insert("status", "passed");
    writeObject(
        control(kind + "-response.json"), result,
        privateAcl(m_config.launcherSid) + "(A;;GR;;;" + m_owner.sid + ")");
    complete = true;
  }

  QJsonObject runProbe(const QString& mode, const QStringList& extra,
                       HANDLE token, const Identity& expected) {
    const auto path = control("probes/" + mode + "/report.json");
    QStringList arguments{"--mode",        mode,
                          "--output",      path,
                          "--owner-user",  m_ownerIdentity.userHash,
                          "--owner-logon", m_ownerIdentity.logonHash};
    arguments.append(extra);
    auto process = spawn(launch(token), artifact("helper"), arguments);
    requireSuccessfulExit(process, remainingMilliseconds(10000),
                          "negative_identity_helper_exit");
    QJsonObject report;
    {
      const Impersonation account(token);
      report = readObject(stagedPath(path, m_config.root));
    }
    verifyHelperIdentity(report, expected);
    return report;
  }

  QJsonObject peerProbes(const QJsonObject& request) {
    QString session;
    {
      const Impersonation owner(m_owner.token.get());
      session = stagedPath(required(request, "session_path"),
                           m_config.root + "/temp");
    }
    const auto endpoint  = required(request, "endpoint");
    const QString prefix = "\\\\.\\pipe\\groundzero-live-";
    require(
        endpoint.startsWith(prefix) && QRegularExpression("^[0-9a-f]{32}$")
                                           .match(endpoint.mid(prefix.size()))
                                           .hasMatch(),
        "qualified_pipe_required");
    const auto wrongUser = runProbe("wrong-user", {"--endpoint", endpoint},
                                    m_other.token.get(), m_otherIdentity);
    const auto wrongLogon =
        runProbe("wrong-logon",
                 {"--executable", artifact("executable"), "--session", session},
                 m_otherLogon.get(), m_logonIdentity);
    require(wrongUser.value("probe").toObject().value("denial") ==
                    "ERROR_ACCESS_DENIED" &&
                wrongLogon.value("probe").toObject().value("denial") ==
                    "live_security",
            "negative_identity_oracles");
    m_report.insert("wrong_user", wrongUser);
    m_report.insert("wrong_logon", wrongLogon);
    return {{"wrong_user", wrongUser}, {"wrong_logon", wrongLogon}};
  }

  QJsonObject reparseProbe(const QJsonObject& request) {
    return GroundzeroWindowsLiveTest::createLiveReparseFixtures(
        m_config.root, required(request, "fixture_root"), m_owner.token.get());
  }

  void closeDesktop() {
    if (m_desktop.station && GetProcessWindowStation() == m_desktop.station)
      checked(SetProcessWindowStation(m_desktop.previous),
              "restore_parent_station");
    if (m_desktop.desktop) {
      checked(CloseDesktop(m_desktop.desktop), "close_private_desktop");
      m_desktop.desktop = nullptr;
    }
    if (m_desktop.station) {
      checked(CloseWindowStation(m_desktop.station), "close_private_station");
      m_desktop.station = nullptr;
    }
  }

  void retainEvidence() {
    if (!m_journal.value.contains("temp_identity")) return;
    const Impersonation owner(m_owner.token.get());
    const auto directory = m_config.root + "/evidence";
    QStringList paths{directory};
    QDirIterator files(
        directory,
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDirIterator::Subdirectories);
    while (files.hasNext()) {
      require(paths.size() < 10000, "evidence_entry_limit");
      paths.append(stagedPath(files.next(), directory));
    }
    std::sort(paths.begin(), paths.end(),
              [](const QString& first, const QString& second) {
                return first.size() > second.size();
              });
    for (const auto& path : paths)
      setAcl(path, privateAcl(m_config.launcherSid));
  }

  void retainReport(const QString& path, HANDLE token) {
    if (!token) return;
    const Impersonation account(token);
    if (!QFileInfo::exists(path)) return;
    setAcl(stagedPath(path, m_config.root), privateAcl(m_config.launcherSid));
  }

  void stopJob() {
    if (!m_job) return;
    m_job->stopUnassigned();
    if (m_job->active()) m_job->terminate();
    m_job->waitEmpty();
    m_job.reset();
    m_journal.value.insert("spawn_pending", false);
    m_journal.save();
  }

  void removePrivateTemporary() {
    if (!m_tempCreated) return;
    int entries = 0;
    if (m_journal.value.contains("temp_identity")) {
      const Impersonation owner(m_owner.token.get());
      removeTree(m_config.root + "/temp", entries);
    } else
      removeTree(m_config.root + "/temp", entries);
  }

  void unloadRetainedAccount(Account& account) {
    if (!account.profile) return;
    checked(UnloadUserProfile(account.token.get(), account.profile),
            "unload_retained_profile");
    account.profile = nullptr;
  }

  QJsonArray cleanup() {
    QJsonArray errors;
    const auto attempt = [&errors](const std::function<void()>& operation) {
      try {
        operation();
      } catch (const Failure& error) {
        errors.append(safeFailure(error));
      } catch (...) {
        errors.append(QJsonObject{{"operation", "unexpected_cleanup_failure"}});
      }
    };
    attempt([this] { stopJob(); });
    if (!errors.isEmpty()) return errors;
    attempt([this] { removePrivateTemporary(); });
    attempt([this] { retainEvidence(); });
    attempt(
        [this] { retainReport(control("owner.json"), m_owner.token.get()); });
    attempt([this] {
      retainReport(control("probes/wrong-user/report.json"),
                   m_other.token.get());
    });
    attempt([this] {
      retainReport(control("probes/wrong-logon/report.json"),
                   m_owner.token.get());
    });
    attempt([this] { closeDesktop(); });
    m_otherLogon.reset();
    if (errors.isEmpty()) {
      attempt([this] {
        deleteOwnedAccount(m_other);
        saveAccounts();
      });
      attempt([this] {
        deleteOwnedAccount(m_owner);
        saveAccounts();
      });
    } else {
      attempt([this] {
        unloadRetainedAccount(m_other);
        saveAccounts();
      });
      attempt([this] {
        unloadRetainedAccount(m_owner);
        saveAccounts();
      });
    }
    attempt([this, &errors] {
      m_journal.value.insert("state",
                             errors.isEmpty() ? "cleaned" : "cleanup_failed");
      m_journal.value.insert("cleanup_errors", errors);
      m_journal.save();
    });
    return errors;
  }

  Configuration m_config;
  Account m_owner;
  Account m_other;
  Journal m_journal;
  Handle m_otherLogon{nullptr, &::CloseHandle};
  Identity m_ownerIdentity{};
  Identity m_otherIdentity{};
  Identity m_logonIdentity{};
  Desktop m_desktop;
  std::unique_ptr<Job> m_job;
  QJsonObject m_report{{"protocol", protocol}, {"status", "failed"}};
  bool m_executed           = false;
  bool m_peerDone           = false;
  bool m_reparseDone        = false;
  bool m_tempCreated        = false;
  const ULONGLONG m_started = GetTickCount64();
};

void requirePrivateJournal(HANDLE file, const QString& launcher) {
  PSID owner               = nullptr;
  PACL acl                 = nullptr;
  PSECURITY_DESCRIPTOR raw = nullptr;
  const auto status =
      GetSecurityInfo(file, SE_FILE_OBJECT,
                      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                      &owner, nullptr, &acl, nullptr, &raw);
  if (status != ERROR_SUCCESS) throw Failure("journal_security_read", status);
  const LocalMemory security(raw, &::LocalFree);
  require(sidText(owner) == launcher && acl && acl->AceCount == 2,
          "journal_owner_or_acl");
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision                      = 0;
  checked(GetSecurityDescriptorControl(raw, &control, &revision),
          "journal_control_read");
  require(control & SE_DACL_PROTECTED, "journal_protected_acl_required");
  QStringList principals;
  for (DWORD index = 0; index < acl->AceCount; ++index) {
    void* rawAce = nullptr;
    checked(GetAce(acl, index, &rawAce), "journal_ace_read");
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
    require(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
                !ace->Header.AceFlags &&
                (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS,
            "journal_ace_profile");
    principals.append(sidText(const_cast<DWORD*>(&ace->SidStart)));
  }
  principals.sort();
  QStringList expected{launcher, "S-1-5-18"};
  expected.sort();
  require(principals == expected, "journal_private_principals");
}

std::vector<Handle> pinDirectories(const QString& path) {
  std::vector<Handle> result;
  QString current = path.left(3);
  QStringList prefixes{current};
  for (const auto& part : path.mid(3).split('/', Qt::SkipEmptyParts)) {
    current = QDir(current).filePath(part);
    prefixes.append(current);
  }
  for (const auto& prefix : prefixes) {
    auto file = handle(
        CreateFileW(wide(prefix), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                    nullptr),
        "cleanup_pin_parent");
    const auto value = fileInformation(file.get());
    require((value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(value.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "cleanup_parent_must_be_directory");
    result.push_back(std::move(file));
  }
  return result;
}

struct RecoveryScope {
  QJsonObject value;
  std::vector<Handle> parents;
  Handle journal{nullptr, &::CloseHandle};
};

RecoveryScope recoveryJournal(const QString& path) {
  const auto file = canonical(path);
  const auto root = QFileInfo(file).path();
  require(below(root, canonical(qEnvironmentVariable("RUNNER_TEMP"))) &&
              QFileInfo(file).fileName() == "transaction.json",
          "fixed_cleanup_journal");
  const auto launcher = identity(currentToken().get()).sid;
  RecoveryScope scope;
  scope.parents = pinDirectories(root);
  scope.journal = handle(CreateFileW(wide(file), GENERIC_READ | READ_CONTROL,
                                     FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT, nullptr),
                         "cleanup_pin_journal");
  const auto fileInfo = fileInformation(scope.journal.get());
  require(!(fileInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
              fileInfo.nNumberOfLinks == 1,
          "cleanup_journal_file_required");
  requirePrivateJournal(scope.journal.get(), launcher);
  const auto value = readObject(file);
  require(value.value("protocol") == protocol &&
              value.value("launcher_sid") == launcher &&
              value.value("stage_root") == root,
          "cleanup_journal_scope");
  for (const auto& pair : {std::pair{"run_id", "GITHUB_RUN_ID"},
                           std::pair{"attempt", "GITHUB_RUN_ATTEMPT"},
                           std::pair{"job", "GITHUB_JOB"}})
    require(value.value(pair.first) == qEnvironmentVariable(pair.second),
            "cleanup_run_identity");
  const auto nonce = required(value, "nonce");
  require(QRegularExpression("^[0-9a-f]{32}$").match(nonce).hasMatch(),
          "cleanup_nonce");
  require(value.value("job_name") == "Local\\groundzero-live-ci-" + nonce,
          "cleanup_job_identity");
  require(
      value.value("stage_identity") == fileIdentity(scope.parents.back().get()),
      "cleanup_stage_identity_changed");
  scope.value = value;
  return scope;
}

bool missingObject(const QString& path) {
  if (GetFileAttributesW(wide(path)) != INVALID_FILE_ATTRIBUTES) return false;
  const auto status = GetLastError();
  require(status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND,
          "cleanup_path_visibility");
  return true;
}

QString handleOwner(HANDLE file) {
  PSID owner               = nullptr;
  PSECURITY_DESCRIPTOR raw = nullptr;
  const auto status =
      GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                      nullptr, nullptr, nullptr, &raw);
  if (status != ERROR_SUCCESS) throw Failure("cleanup_owner_read", status);
  const LocalMemory security(raw, &::LocalFree);
  return sidText(owner);
}

struct CleanupBudget {
  int entries        = 0;
  ULONGLONG deadline = GetTickCount64() + 30000;
};

QString directoryLeaf(const FILE_ID_BOTH_DIR_INFO& value, size_t remaining) {
  const auto prefix = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
  require(remaining >= prefix && value.FileNameLength <= remaining - prefix &&
              value.FileNameLength % sizeof(wchar_t) == 0,
          "cleanup_directory_record");
  const auto name =
      QString::fromWCharArray(value.FileName, int(value.FileNameLength / 2));
  if (name == "." || name == "..") return {};
  require(!name.isEmpty() &&
              !name.contains(QRegularExpression("[\\\\/:\\x00]")) &&
              !name.endsWith('.') && !name.endsWith(' '),
          "cleanup_directory_leaf");
  return name;
}

QStringList directoryNames(HANDLE directory) {
  alignas(8) std::array<char, 65536> buffer{};
  QStringList names;
  while (GetFileInformationByHandleEx(directory, FileIdBothDirectoryInfo,
                                      buffer.data(), DWORD(buffer.size()))) {
    size_t offset = 0;
    for (;;) {
      require(offset <= buffer.size() - sizeof(FILE_ID_BOTH_DIR_INFO),
              "cleanup_directory_offset");
      const auto& entry = *reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(
          buffer.data() + offset);
      const auto name = directoryLeaf(entry, buffer.size() - offset);
      if (!name.isEmpty()) names.append(name);
      require(names.size() <= 100000, "cleanup_directory_entry_limit");
      if (!entry.NextEntryOffset) break;
      require(entry.NextEntryOffset % 8 == 0 &&
                  entry.NextEntryOffset >= sizeof(entry),
              "cleanup_directory_next");
      offset += entry.NextEntryOffset;
    }
  }
  require(GetLastError() == ERROR_NO_MORE_FILES,
          "cleanup_directory_enumeration");
  return names;
}

BY_HANDLE_FILE_INFORMATION cleanupObject(HANDLE file, const QString& owner,
                                         const QJsonObject& expected) {
  require(handleOwner(file) == owner, "cleanup_foreign_owner");
  const auto info = fileInformation(file);
  require((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
              info.nNumberOfLinks == 1,
          "cleanup_hardlink_rejected");
  if (!expected.isEmpty()) {
    require((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "cleanup_temp_must_be_directory");
    require(expected == fileIdentity(file), "cleanup_temp_identity_changed");
  }
  return info;
}

Handle openCleanupObject(const QString& path, bool allowMissing = false) {
  const auto raw = CreateFileW(
      wide(path), DELETE | READ_CONTROL | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (raw == INVALID_HANDLE_VALUE && allowMissing) {
    const auto status = GetLastError();
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
      return Handle(nullptr, &::CloseHandle);
    throw Failure("cleanup_open_owned_object", status);
  }
  return handle(raw, "cleanup_open_owned_object");
}

struct CleanupEntry {
  QString path;
  QString owner;
  Handle file;
  QJsonObject expected;
  int depth = 0;
};

void deleteOwnedObject(CleanupEntry entry, CleanupBudget& budget) {
  require(++budget.entries <= 100000 && entry.depth <= 128 &&
              GetTickCount64() < budget.deadline,
          "cleanup_tree_limit");
  const auto info =
      cleanupObject(entry.file.get(), entry.owner, entry.expected);
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
      !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    for (const auto& name : directoryNames(entry.file.get())) {
      const auto path = QDir(entry.path).filePath(name);
      deleteOwnedObject(
          {path, entry.owner, openCleanupObject(path), {}, entry.depth + 1},
          budget);
    }
  }
  FILE_DISPOSITION_INFO disposition{TRUE};
  checked(SetFileInformationByHandle(entry.file.get(), FileDispositionInfo,
                                     &disposition, sizeof(disposition)),
          "cleanup_delete_owned_handle");
  entry.file.reset();
  require(missingObject(entry.path), "cleanup_object_remains");
}

QString cleanupOwner(const QJsonObject& journal) {
  QString owner;
  for (const auto& value : journal.value("accounts").toArray()) {
    const auto account = value.toObject();
    if (account.value("role") != "owner") continue;
    require(owner.isEmpty() && account.value("created") == true,
            "cleanup_owner_record");
    owner = required(account, "sid");
    require(accountSid(required(account, "name")) == owner,
            "cleanup_owner_identity_changed");
  }
  require(owner.startsWith("S-1-5-21-"), "cleanup_local_owner_required");
  return owner;
}

void recoverTemporary(const RecoveryScope& scope) {
  const auto path = required(scope.value, "stage_root") + "/temp";
  auto file       = openCleanupObject(path, true);
  if (!file) return;
  require(scope.value.value("temp_created") == true,
          "cleanup_temp_not_created");
  const auto owner    = cleanupOwner(scope.value);
  const auto expected = scope.value.value("temp_identity").toObject();
  exactKeys(expected, {"volume", "index_high", "index_low"});
  CleanupBudget budget;
  deleteOwnedObject({path, owner, std::move(file), expected, 0}, budget);
}

void fixtureDirectory(const QString& path, const QString& owner) {
  const auto security = descriptor(privateAcl(owner));
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.get(),
                                 FALSE};
  checked(CreateDirectoryW(wide(path), &attributes),
          "cleanup_fixture_directory");
}

void fixtureFile(const QString& path, const QString& owner) {
  const auto security = descriptor(privateAcl(owner));
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.get(),
                                 FALSE};
  const auto file =
      handle(CreateFileW(wide(path), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr),
             "cleanup_fixture_file");
  writeBytes(file.get(), "fixed cleanup fixture\n");
}

class ExistingPrivilege final {
public:
  explicit ExistingPrivilege(LPCWSTR name) : m_token(currentToken()) {
    TOKEN_PRIVILEGES desired{};
    desired.PrivilegeCount           = 1;
    desired.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    checked(LookupPrivilegeValueW(nullptr, name, &desired.Privileges[0].Luid),
            "fixture_privilege_lookup");
    DWORD size = sizeof(m_previous);
    SetLastError(ERROR_SUCCESS);
    checked(AdjustTokenPrivileges(m_token.get(), FALSE, &desired, size,
                                  &m_previous, &size),
            "fixture_privilege_enable");
    if (GetLastError() != ERROR_SUCCESS)
      throw Failure("fixture_privilege_unavailable", GetLastError());
  }
  ~ExistingPrivilege() {
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(m_token.get(), FALSE, &m_previous, 0, nullptr,
                               nullptr) ||
        GetLastError() != ERROR_SUCCESS)
      std::terminate();
  }
  ExistingPrivilege(const ExistingPrivilege&)            = delete;
  ExistingPrivilege& operator=(const ExistingPrivilege&) = delete;

private:
  Handle m_token;
  TOKEN_PRIVILEGES m_previous{};
};

void fixtureJunction(const QString& path, const QString& target,
                     const QString& owner) {
  const ExistingPrivilege symbolicLink(SE_CREATE_SYMBOLIC_LINK_NAME);
  fixtureDirectory(path, owner);
  const auto substitute = "\\??\\" + QDir::toNativeSeparators(target);
  const auto printable  = QDir::toNativeSeparators(target);
  struct Header {
    DWORD tag;
    WORD length;
    WORD reserved;
    WORD substituteOffset;
    WORD substituteLength;
    WORD printOffset;
    WORD printLength;
  };
  static_assert(sizeof(Header) == 16);
  require(substitute.size() + printable.size() < 7000,
          "cleanup_junction_target_limit");
  const Header header{IO_REPARSE_TAG_MOUNT_POINT,
                      WORD(8 + (substitute.size() + printable.size() + 2) * 2),
                      0,
                      0,
                      WORD(substitute.size() * 2),
                      WORD((substitute.size() + 1) * 2),
                      WORD(printable.size() * 2)};
  QByteArray data(reinterpret_cast<const char*>(&header), sizeof(header));
  data.append(reinterpret_cast<const char*>(substitute.utf16()),
              substitute.size() * 2);
  data.append(2, '\0');
  data.append(reinterpret_cast<const char*>(printable.utf16()),
              printable.size() * 2);
  data.append(2, '\0');
  const auto file = handle(
      CreateFileW(wide(path), GENERIC_WRITE | READ_CONTROL,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                  nullptr),
      "cleanup_junction_open");
  DWORD returned = 0;
  checked(DeviceIoControl(file.get(), FSCTL_SET_REPARSE_POINT, data.data(),
                          DWORD(data.size()), nullptr, 0, &returned, nullptr),
          "cleanup_junction_create");
  require(handleOwner(file.get()) == owner, "cleanup_junction_owner");
}

void recoverAccount(QJsonObject& account);

void expectFixtureFailure(const std::function<void()>& operation,
                          const char* expected) {
  try {
    operation();
  } catch (const Failure& error) {
    require(QString::fromLatin1(error.what()) == expected,
            "unexpected_cleanup_fixture_failure");
    return;
  }
  throw Failure("cleanup_fixture_was_admitted", ERROR_INVALID_DATA);
}

class CleanupFixtures final {
public:
  CleanupFixtures(const Configuration& config, const Account& owner,
                  const Account& other, Journal& journal)
      : m_config(config), m_owner(owner), m_other(other), m_journal(journal) {}

  QJsonObject run() {
    rejectAccountIntent();
    fixtureFile(temp() + "/private.json", m_owner.sid);
    erase();
    resetTemporary();
    rejectForeign();
    rejectJunction();
    rejectJournalAcl();
    rejectJournalRun();
    erase();
    resetTemporary();
    return {{"status", "passed"},
            {"private_cleanup", true},
            {"foreign_owner_rejected", true},
            {"root_junction_rejected", true},
            {"nested_junction_target_preserved", true},
            {"journal_acl_rejected", true},
            {"journal_run_rejected", true},
            {"unconfirmed_account_rejected", true},
            {"unrecorded_account_sid_rejected", true}};
  }

private:
  QString temp() const { return m_config.root + "/temp"; }

  void resetTemporary() {
    require(missingObject(temp()), "cleanup_fixture_temp_remains");
    fixtureDirectory(temp(), m_owner.sid);
    m_journal.value.insert("temp_identity", pathIdentity(temp()));
    m_journal.save();
  }

  void erase() {
    const auto scope = recoveryJournal(m_journal.path);
    recoverTemporary(scope);
    require(missingObject(temp()), "cleanup_fixture_temp_remains");
  }

  void rejected(const char* operation) {
    expectFixtureFailure([this] { erase(); }, operation);
  }

  void rejectAccountIntent() {
    auto pending = accountRecord(m_owner);
    pending.insert("created", false);
    pending.insert("sid", "");
    pending.insert("profile_loaded", false);
    expectFixtureFailure([&pending] { recoverAccount(pending); },
                         "cleanup_account_creation_uncertain");
    Account unknown("owner");
    unknown.name = m_owner.name;
    expectFixtureFailure([&unknown] { deleteOwnedAccount(unknown); },
                         "cleanup_account_creation_uncertain");
    unknown.created = true;
    expectFixtureFailure([&unknown] { deleteOwnedAccount(unknown); },
                         "cleanup_account_identity_changed");
    require(accountSid(m_owner.name) == m_owner.sid,
            "cleanup_intent_fixture_changed_account");
    Account absent("owner");
    auto absentRecord = accountRecord(absent);
    recoverAccount(absentRecord);
    deleteOwnedAccount(absent);
  }

  void deleteFixedLeaf(const QString& path, const QString& owner) {
    const auto parents = pinDirectories(QFileInfo(path).path());
    CleanupBudget budget;
    deleteOwnedObject({path, owner, openCleanupObject(path), {}, 1}, budget);
  }

  void rejectForeign() {
    const auto path = temp() + "/foreign.json";
    fixtureFile(path, m_other.sid);
    rejected("cleanup_foreign_owner");
    {
      const Impersonation other(m_other.token.get());
      require(QFileInfo(path).isFile(), "foreign_cleanup_fixture_changed");
    }
    deleteFixedLeaf(path, m_other.sid);
    erase();
    resetTemporary();
  }

  void requireTarget(const QString& path, const QString& expected) const {
    const Impersonation owner(m_owner.token.get());
    require(fileHash(path) == expected, "cleanup_junction_target_changed");
  }

  void rejectJunction() {
    const auto target = m_config.root + "/evidence/cleanup-target";
    fixtureDirectory(target, m_owner.sid);
    fixtureFile(target + "/sentinel.json", m_owner.sid);
    QString digest;
    {
      const Impersonation owner(m_owner.token.get());
      digest = fileHash(target + "/sentinel.json");
    }
    erase();
    fixtureJunction(temp(), target, m_owner.sid);
    rejected("cleanup_temp_must_be_directory");
    requireTarget(target + "/sentinel.json", digest);
    deleteFixedLeaf(temp(), m_owner.sid);
    resetTemporary();
    fixtureJunction(temp() + "/owned-link", target, m_owner.sid);
    erase();
    requireTarget(target + "/sentinel.json", digest);
    resetTemporary();
  }

  void rejectJournalAcl() {
    setAcl(m_journal.path,
           privateAcl(m_config.launcherSid) + "(A;;GR;;;" + m_other.sid + ")");
    try {
      rejected("journal_owner_or_acl");
    } catch (...) {
      setAcl(m_journal.path, privateAcl(m_config.launcherSid));
      throw;
    }
    setAcl(m_journal.path, privateAcl(m_config.launcherSid));
    require(pathIdentity(temp()) ==
                m_journal.value.value("temp_identity").toObject(),
            "journal_tamper_deleted_temp");
  }

  void rejectJournalRun() {
    const auto saved = m_journal.value;
    m_journal.value.insert("run_id", "cleanup-fixture-other-run");
    m_journal.save();
    try {
      rejected("cleanup_run_identity");
    } catch (...) {
      m_journal.value = saved;
      m_journal.save();
      throw;
    }
    m_journal.value = saved;
    m_journal.save();
    require(pathIdentity(temp()) ==
                m_journal.value.value("temp_identity").toObject(),
            "journal_tamper_deleted_temp");
  }

  const Configuration& m_config;
  const Account& m_owner;
  const Account& m_other;
  Journal& m_journal;
};

QJsonObject cleanupBoundaries(const Configuration& config, const Account& owner,
                              const Account& other, Journal& journal) {
  CleanupFixtures fixtures(config, owner, other, journal);
  return fixtures.run();
}

void recoverJob(const QString& name) {
  const auto raw = OpenJobObjectW(JOB_OBJECT_QUERY | JOB_OBJECT_TERMINATE,
                                  FALSE, wide(name));
  if (!raw) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND) return;
    throw Failure("open_owned_cleanup_job", GetLastError());
  }
  const auto job = handle(raw, "open_owned_cleanup_job");
  checked(TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED),
          "terminate_cleanup_job");
  const auto deadline = GetTickCount64() + 5000;
  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION state{};
  do {
    checked(QueryInformationJobObject(job.get(),
                                      JobObjectBasicAccountingInformation,
                                      &state, sizeof(state), nullptr),
            "cleanup_job_count");
    if (!state.ActiveProcesses) return;
    Sleep(20);
  } while (GetTickCount64() < deadline);
  throw Failure("cleanup_descendants_remain", WAIT_TIMEOUT);
}

void recoverAccount(QJsonObject& account) {
  exactKeys(account, {"role", "name", "sid", "created", "profile_loaded"});
  require(account.value("role") == "owner" || account.value("role") == "other",
          "cleanup_account_role");
  const auto name = required(account, "name");
  require(QRegularExpression("^gzl[0-9a-f]{16}$").match(name).hasMatch(),
          "cleanup_created_name");
  require(account.value("created").isBool(), "cleanup_created_state");
  if (account.value("created") == false) {
    requireAccountAbsent(name);
    return;
  }
  const auto sid = required(account, "sid");
  require(sid.startsWith("S-1-5-21-"), "cleanup_created_identity");
  try {
    require(accountSid(name) == sid, "cleanup_sid_changed");
  } catch (const Failure& error) {
    if (error.code != NERR_UserNotFound) throw;
  }
  deleteOwnedProfile(sid);
  const auto status = NetUserDel(nullptr, wide(name));
  if (status != NERR_Success && status != NERR_UserNotFound)
    throw Failure("cleanup_created_account", status);
  account.insert("created", false);
  account.insert("profile_loaded", false);
}

int recover(const QString& path) {
  auto scope    = recoveryJournal(path);
  auto& journal = scope.value;
  QJsonArray errors;
  try {
    recoverJob(required(journal, "job_name"));
    require(journal.value("spawn_pending") != true,
            "cleanup_unassigned_spawn_uncertain");
    recoverTemporary(scope);
    scope.journal.reset();
    auto accounts = journal.value("accounts").toArray();
    require(accounts.size() == 2, "cleanup_account_count");
    for (int index = 0; index < accounts.size(); ++index) {
      auto account = accounts[index].toObject();
      recoverAccount(account);
      accounts[index] = account;
      journal.insert("accounts", accounts);
      writeObject(path, journal, privateAcl(required(journal, "launcher_sid")));
    }
  } catch (const Failure& error) {
    errors.append(safeFailure(error));
  }
  scope.journal.reset();
  journal.insert("state", errors.isEmpty() ? "cleaned" : "cleanup_failed");
  journal.insert("cleanup_errors", errors);
  writeObject(path, journal, privateAcl(required(journal, "launcher_sid")));
  return errors.isEmpty() ? 0 : 1;
}

int run(const QStringList& arguments) {
  hostedGuard();
  require(arguments.last() == "--allow-hosted-identities",
          "explicit_identity_guard_required");
  if (arguments.size() == 3 && arguments[0] == "--cleanup")
    return recover(arguments[1]);
  require(arguments.size() == 5 && arguments[0] == "--configuration" &&
              arguments[2] == "--output",
          "fixed_launcher_arguments");
  const auto config = configuration(arguments[1], arguments[3]);
  setAcl(config.root, privateAcl(config.launcherSid));
  Transaction transaction(config);
  return transaction.run();
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  try {
    QStringList arguments;
    for (int index = 1; index < argc; ++index)
      arguments.append(QString::fromWCharArray(argv[index]));
    require(!arguments.isEmpty(), "fixed_launcher_arguments");
    return run(arguments);
  } catch (const Failure& error) {
    const auto message = QString("Windows live launcher failed: %1 (%2).\n")
                             .arg(QString::fromLatin1(error.what()))
                             .arg(error.code)
                             .toUtf8();
    DWORD written      = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message.constData(),
              DWORD(message.size()), &written, nullptr);
    return 1;
  } catch (...) {
    constexpr char message[] =
        "Windows live launcher failed: internal_error.\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message,
              DWORD(sizeof(message) - 1), &written, nullptr);
    return 1;
  }
}
#else
#include <cstdio>
int main() {
  std::fputs(
      "Windows live launcher requires a reviewed Windows hosted runner.\n",
      stderr);
  return 1;
}
#endif
