#include "automation_build_identity.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QPair>
#include <QRegularExpression>
#include <QStringList>

#if defined(Q_OS_MACOS)
#include <mach-o/dyld.h>
#elif defined(Q_OS_LINUX)
#include <link.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

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

#ifndef AUTOMATION_IDENTITY_EXECUTABLE_ONLY
// The libraries this project builds. The identity covers each one where the
// dynamic loader loaded it from; third-party and system libraries are not part
// of it.
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

#if defined(Q_OS_LINUX)
int collectImage(dl_phdr_info* info, size_t, void* images) {
  if (info->dlpi_name && *info->dlpi_name)
    static_cast<QStringList*>(images)->append(
        QString::fromLocal8Bit(info->dlpi_name));
  return 0;
}
#endif

// The paths of every image the dynamic loader has loaded into this process.
QStringList loadedImages() {
  QStringList images;
#if defined(Q_OS_MACOS)
  for (uint32_t index = 0; index < _dyld_image_count(); ++index)
    images.append(QString::fromUtf8(_dyld_get_image_name(index)));
#elif defined(Q_OS_LINUX)
  dl_iterate_phdr(collectImage, &images);
#elif defined(Q_OS_WIN)
  HMODULE modules[1024];
  DWORD needed = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules),
                             &needed))
    throw Error("connector_io", "Cannot list this process's modules.");
  for (DWORD index = 0; index < needed / sizeof(HMODULE) && index < 1024;
       ++index) {
    wchar_t path[32768];
    const DWORD size = GetModuleFileNameW(modules[index], path, 32768);
    if (size) images.append(QString::fromWCharArray(path, int(size)));
  }
#endif
  return images;
}

// Each project library this process loaded, by file name, with its path.
QMap<QString, QString> loadedProjectLibraries() {
  QMap<QString, QString> found;
  for (const QString& image : loadedImages()) {
    const QString name = QFileInfo(image).fileName();
    for (const char* library : projectLibraries) {
      const QString file = libraryFileName(library);
      if (name == file || name.startsWith(file + "."))
        found.insert(file, image);
    }
  }
  return found;
}
#endif

QByteArray fileDigest(const QString& path) {
  QFile file(path);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file))
    throw Error("connector_io",
                "Cannot read this application's files to identify its build. "
                "Reinstall the application.");
  return hash.result().toHex();
}

struct Manifest final {
  QJsonArray files;
  QString digest;
};

// One line per covered file, the executable first and then each project
// library in name order: "<name>\t<sha256>\n". A project library that is not
// loaded is an error, except in the standalone bridge tests, which link none.
Manifest computeManifest() {
  QList<QPair<QString, QString>> files{
      {"executable", QCoreApplication::applicationFilePath()}};
#ifndef AUTOMATION_IDENTITY_EXECUTABLE_ONLY
  const QMap<QString, QString> loaded = loadedProjectLibraries();
  for (const char* library : projectLibraries) {
    const QString name = libraryFileName(library);
    if (!loaded.contains(name))
      throw Error("connector_io",
                  "A library of this application is not loaded, so its build "
                  "cannot be identified. Reinstall the application.",
                  {{"library", name}});
  }
  for (auto it = loaded.constBegin(); it != loaded.constEnd(); ++it)
    files.append({it.key(), it.value()});
#endif
  Manifest result;
  QByteArray lines;
  for (const auto& file : files) {
    const QByteArray digest = fileDigest(file.second);
    lines += file.first.toUtf8() + "\t" + digest + "\n";
    result.files.append(QJsonObject{{"name", file.first},
                                    {"path", file.second},
                                    {"sha256", QString::fromLatin1(digest)}});
  }
  result.digest = QString::fromLatin1(
      QCryptographicHash::hash(lines, QCryptographicHash::Sha256).toHex());
  return result;
}

std::shared_future<Manifest> manifest(std::launch policy) {
  static std::once_flag started;
  static std::shared_future<Manifest> result;
  std::call_once(started, [policy]() {
    result = std::async(policy, computeManifest).share();
  });
  return result;
}

}  // namespace

QJsonObject buildIdentity() {
  return {{"application", "groundzero"},
          {"version", applicationVersion()},
          {"build_digest", manifest(std::launch::deferred).get().digest}};
}

void prefetchBuildIdentity() { manifest(std::launch::async); }

int printBuildManifest(int argc, char* argv[]) {
  try {
    QCoreApplication application(argc, argv);
    const Manifest current = manifest(std::launch::deferred).get();
    const QByteArray text =
        QJsonDocument(QJsonObject{{"build_digest", current.digest},
                                  {"files", current.files}})
            .toJson(QJsonDocument::Compact);
    std::fwrite(text.constData(), 1, size_t(text.size()), stdout);
    std::fputs("\n", stdout);
    return std::fflush(stdout) == 0 ? 0 : 1;
  } catch (const Error& error) {
    std::fprintf(stderr, "Groundzero build manifest failed: %s\n",
                 error.message.toUtf8().constData());
  }
  return 1;
}

bool validBuildIdentity(const QJsonValue& value) {
  if (!value.isObject()) return false;
  const QJsonObject object = value.toObject();
  const QStringList keys{"application", "build_digest", "version"};
  if (object.keys() != keys) return false;
  const QString version = object.value("version").toString();
  const QRegularExpression hex(
      QRegularExpression::anchoredPattern("[0-9a-f]{64}"));
  return object.value("application") == "groundzero" &&
         object.value("version").isString() && !version.isEmpty() &&
         version.size() <= 64 && object.value("build_digest").isString() &&
         hex.match(object.value("build_digest").toString()).hasMatch();
}

}  // namespace GroundzeroAutomation
