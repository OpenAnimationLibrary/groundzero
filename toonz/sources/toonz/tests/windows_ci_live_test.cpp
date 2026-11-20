#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStringList>
#include <cstdio>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
namespace {

using Handle = std::unique_ptr<void, decltype(&::CloseHandle)>;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QByteArray tokenData(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
  DWORD length = 0;
  GetTokenInformation(token, kind, nullptr, 0, &length);
  require(GetLastError() == ERROR_INSUFFICIENT_BUFFER && length > 0 &&
              length <= 65536,
          "Cannot size the probe security token");
  QByteArray bytes(int(length), '\0');
  require(GetTokenInformation(token, kind, bytes.data(), length, &length),
          "Cannot inspect the probe security token");
  return bytes;
}

QString digest(const QByteArray& bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QJsonObject identity() {
  HANDLE handle = nullptr;
  require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &handle),
          "Cannot open the probe process token");
  const Handle token(handle, &::CloseHandle);
  const QByteArray user = tokenData(token.get(), TokenUser);
  const auto sid =
      reinterpret_cast<const TOKEN_USER*>(user.constData())->User.Sid;
  require(IsValidSid(sid), "The probe user SID is invalid");
  const QByteArray statistics = tokenData(token.get(), TokenStatistics);
  const auto authentication =
      reinterpret_cast<const TOKEN_STATISTICS*>(statistics.constData())
          ->AuthenticationId;
  const QByteArray elevation = tokenData(token.get(), TokenElevation);
  const bool elevated =
      reinterpret_cast<const TOKEN_ELEVATION*>(elevation.constData())
          ->TokenIsElevated != 0;
  const QByteArray logon = QByteArray::number(authentication.HighPart) + ":" +
                           QByteArray::number(authentication.LowPart);
  return {{"user_sha256", digest(QByteArray(static_cast<const char*>(sid),
                                            int(GetLengthSid(sid))))},
          {"logon_sha256", digest(logon)},
          {"elevated", elevated},
          {"pid", double(GetCurrentProcessId())}};
}

void requireHostedRunner() {
  require(qEnvironmentVariable("GITHUB_ACTIONS") == "true" &&
              qEnvironmentVariable("RUNNER_ENVIRONMENT") == "github-hosted" &&
              qEnvironmentVariable("RUNNER_OS") == "Windows",
          "Identity probes are restricted to Windows GitHub-hosted Actions "
          "runners");
}

QJsonObject arguments(const QStringList& values) {
  require(values.size() >= 3 && values.size() % 2 == 1,
          "Probe arguments must be named pairs");
  const QStringList accepted{"--mode",       "--output",  "--endpoint",
                             "--executable", "--session", "--owner-user",
                             "--owner-logon"};
  QJsonObject result;
  for (int index = 1; index < values.size(); index += 2) {
    require(accepted.contains(values[index]) && !result.contains(values[index]),
            "Unknown or repeated identity probe option");
    result.insert(values[index], values[index + 1]);
  }
  require(result.contains("--mode") && result.contains("--output"),
          "Identity probe requires mode and output");
  return result;
}

QString required(const QJsonObject& options, const QString& name) {
  const QString value = options.value(name).toString();
  require(!value.isEmpty() && !value.contains(QChar('\0')),
          "A required probe argument is invalid");
  return value;
}

QString localFile(const QString& value) {
  const QFileInfo file(value);
  require(file.isAbsolute() && file.isFile() && !file.isSymLink(),
          "The probe requires an existing absolute regular file");
  return file.canonicalFilePath();
}

QString outputPath(const QJsonObject& options) {
  const QString value = required(options, "--output");
  const QFileInfo file(value);
  const QString parent = file.dir().canonicalPath();
  const QString temporary =
      QDir(qEnvironmentVariable("RUNNER_TEMP")).canonicalPath();
  require(!temporary.isEmpty() && !parent.isEmpty() && file.isAbsolute() &&
              !file.exists(),
          "The probe output must be a new file beneath the hosted temporary "
          "directory");
  require(parent.startsWith(temporary + '/', Qt::CaseInsensitive),
          "The probe output is outside the hosted temporary directory");
  return QDir(parent).filePath(file.fileName());
}

void requireOwner(const QJsonObject& observed, const QJsonObject& options,
                  bool sameUser) {
  require(!observed.value("elevated").toBool(),
          "The negative probe must be non-elevated");
  const bool matches = observed.value("user_sha256").toString() ==
                       required(options, "--owner-user");
  require(matches == sameUser,
          "The launched probe has the wrong test identity");
  require(observed.value("logon_sha256").toString() !=
              required(options, "--owner-logon"),
          "The negative probe unexpectedly shares the owner's logon");
}

QJsonObject pipeProbe(const QJsonObject& options, const QJsonObject& observed) {
  requireOwner(observed, options, false);
  const QString endpoint = required(options, "--endpoint");
  const QString prefix = "\\\\.\\pipe\\groundzero-live-";
  require(endpoint.startsWith(prefix) && endpoint.size() == prefix.size() + 32,
          "The probe requires an explicit qualified local live pipe");
  const HANDLE pipe = CreateFileW(reinterpret_cast<LPCWSTR>(endpoint.utf16()),
                                  GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  const DWORD error = GetLastError();
  if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
  require(pipe == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED,
          "The actual wrong-user pipe connection was not denied with "
          "ACCESS_DENIED");
  return {{"denial", "ERROR_ACCESS_DENIED"}, {"win32_error", double(error)}};
}

QJsonObject connectorProbe(const QJsonObject& options,
                           const QJsonObject& observed) {
  requireOwner(observed, options, true);
  const QString executable = localFile(required(options, "--executable"));
  const QString session = localFile(required(options, "--session"));
  QProcess connector;
  connector.start(executable, {"--automation-live-connect", session});
  require(connector.waitForStarted(3000),
          "The different-logon connector failed to start");
  connector.closeWriteChannel();
  if (!connector.waitForFinished(5000)) {
    connector.kill();
    require(connector.waitForFinished(3000),
            "The different-logon connector did not stop");
    throw std::runtime_error(
        "The different-logon connector exceeded its deadline");
  }
  require(connector.exitStatus() == QProcess::NormalExit &&
              connector.exitCode() == 1,
          "The different-logon connector was not rejected normally");
  require(connector.readAllStandardOutput().isEmpty(),
          "The different-logon connector exposed unauthenticated output");
  require(
      connector.readAllStandardError() ==
          "Groundzero live connector failed: live_security.\n",
      "The different-logon connector did not fail at the security boundary");
  return {{"denial", "live_security"},
          {"connector_exit_code", 1},
          {"connector_stdout_bytes", 0}};
}

void saveReport(const QString& path, const QJsonObject& report) {
  QSaveFile file(path);
  const QByteArray bytes =
      QJsonDocument(report).toJson(QJsonDocument::Indented);
  require(file.open(QIODevice::WriteOnly),
          "Cannot create the identity probe report");
  require(file.write(bytes) == bytes.size() && file.commit(),
          "Cannot commit the identity report");
}

int run(const QStringList& values) {
  requireHostedRunner();
  const QJsonObject options = arguments(values);
  const QString output = outputPath(options);
  QJsonObject report{{"status", "failed"}, {"identity", identity()}};
  try {
    const QString mode = required(options, "--mode");
    const auto observed = report.value("identity").toObject();
    if (mode == "wrong-user")
      report.insert("probe", pipeProbe(options, observed));
    else if (mode == "wrong-logon")
      report.insert("probe", connectorProbe(options, observed));
    else {
      require(mode == "identity", "Unknown identity probe mode");
      require(!observed.value("elevated").toBool(),
              "The owner must be non-elevated");
    }
    report.insert("status", "passed");
  } catch (const std::exception& error) {
    report.insert("error", QString::fromUtf8(error.what()));
  }
  saveReport(output, report);
  return report.value("status") == "passed" ? 0 : 1;
}

}  // namespace
#endif

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  try {
#ifdef _WIN32
    return run(application.arguments());
#else
    throw std::runtime_error("The identity probe requires Windows");
#endif
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Windows live identity probe failed: %s\n",
                 error.what());
    return 1;
  }
}
