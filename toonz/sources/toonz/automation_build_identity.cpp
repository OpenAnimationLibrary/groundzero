#include "automation_build_identity.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QRegularExpression>

#include <cstdio>
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

QJsonObject computeIdentity() {
  QFile executable(QCoreApplication::applicationFilePath());
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!executable.open(QIODevice::ReadOnly) || !hash.addData(&executable))
    throw Error("connector_io",
                "Cannot read this executable to identify its build. Reinstall "
                "the application.");
  return {{"application", "groundzero"},
          {"version", applicationVersion()},
          {"executable_sha256", QString::fromLatin1(hash.result().toHex())}};
}

}  // namespace

QJsonObject buildIdentity() {
  static const QJsonObject identity = computeIdentity();
  return identity;
}

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
