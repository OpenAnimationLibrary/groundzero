#include "automation_build_identity.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include <cstdio>
#include <future>
#include <mutex>
#include <string>

#include "automation.h"
// TEnv computes its version from these constants on first use, and that first
// use reads system-variable files and may print to stdout, which the live
// connector reserves for protocol frames. The same constants are read here.
#include "tversion.h"

namespace GroundzeroAutomation {
namespace {

QString applicationVersion() {
  TVER::ToonzVersion version;
  std::string text = version.getAppVersionString();
  if (version.getAppRevision() != 0)
    text += "." + version.getAppRevisionString();
  return QString::fromStdString(text).left(64);
}

// The libraries this project builds and ships beside its executable; anything
// else there, and everything elsewhere, is not part of the identity.
const char* const projectLibraries[] = {
    "colorfx", "image",    "sound",    "tfarm",    "tnzbase", "tnzcore",
    "tnzext",  "tnzstdfx", "tnztools", "toonzlib", "toonzqt"};

QString libraryFileName(const char* name) {
#if defined(Q_OS_WIN)
  return QString("%1.dll").arg(name);
#elif defined(Q_OS_MACOS)
  return QString("lib%1.dylib").arg(name);
#else
  return QString("lib%1.so").arg(name);
#endif
}

QByteArray fileDigest(const QString& path) {
  QFile file(path);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file))
    throw Error("connector_io",
                "Cannot read this application's files to identify its build. "
                "Reinstall the application.");
  return hash.result().toHex();
}

// One line per covered file, the executable first and then each project
// library present beside it in name order: "<name>\t<sha256>\n".
QJsonObject computeIdentity() {
  const QFileInfo executable(QCoreApplication::applicationFilePath());
  QByteArray manifest =
      "executable\t" + fileDigest(executable.filePath()) + "\n";
  QStringList names;
  for (const char* name : projectLibraries) names.append(libraryFileName(name));
  names.sort();
  for (const QString& name : names) {
    const QString path = executable.dir().filePath(name);
    if (QFileInfo(path).isFile())
      manifest += name.toUtf8() + "\t" + fileDigest(path) + "\n";
  }
  return {{"application", "groundzero"},
          {"version", applicationVersion()},
          {"executable_sha256",
           QString::fromLatin1(
               QCryptographicHash::hash(manifest, QCryptographicHash::Sha256)
                   .toHex())}};
}

std::shared_future<QJsonObject> identity(std::launch policy) {
  static std::once_flag started;
  static std::shared_future<QJsonObject> result;
  std::call_once(started, [policy]() {
    result = std::async(policy, computeIdentity).share();
  });
  return result;
}

}  // namespace

QJsonObject buildIdentity() { return identity(std::launch::deferred).get(); }

void prefetchBuildIdentity() { identity(std::launch::async); }

bool validBuildIdentity(const QJsonValue& value) {
  if (!value.isObject()) return false;
  const QJsonObject object = value.toObject();
  const QStringList keys{"application", "executable_sha256", "version"};
  if (object.keys() != keys) return false;
  const QString version = object.value("version").toString();
  const QRegularExpression hex(
      QRegularExpression::anchoredPattern("[0-9a-f]{64}"));
  return object.value("application") == "groundzero" &&
         object.value("version").isString() && !version.isEmpty() &&
         version.size() <= 64 && object.value("executable_sha256").isString() &&
         hex.match(object.value("executable_sha256").toString()).hasMatch();
}

}  // namespace GroundzeroAutomation
