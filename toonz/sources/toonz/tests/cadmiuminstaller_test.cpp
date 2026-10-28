#include "cadmiuminstaller.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>

namespace {
struct Artifact { const char *name; qint64 bytes; const char *hash; };
const Artifact Artifacts[] = {
    {"ant_v2_fp32.onnx", 1388610539LL,
     "6c8d95cbfa62408f3cbfbbc29204fac7df5a587466dbb751e7e1b0bb530ff12b"},
    {"gap_closer_fp32.onnx", 497500380LL,
     "3a9a792f72fd3451c36145a0657328e333d6e5e982f0760b5e5a6e7e9934d327"}};

bool verify(const QString &directory) {
  for (const Artifact &artifact : Artifacts) {
    QFile file(QDir(directory).filePath(QString::fromLatin1(artifact.name)));
    if (file.size() != artifact.bytes || !file.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || hash.result().toHex() != artifact.hash) return false;
  }
  return true;
}

bool install(const QString &directory) {
  CadmiumInstaller installer;
  QEventLoop loop;
  QTimer deadline;
  deadline.setSingleShot(true);
  QObject context;
  bool terminal = false, success = false;
  QObject::connect(&installer, &CadmiumInstaller::finished, &context,
                   [&](const QString &) { terminal = success = true; loop.quit(); });
  QObject::connect(&installer, &CadmiumInstaller::failed, &context,
                   [&](const QString &) {
    std::cerr << "Installer reported a failure\n";
    terminal = true;
    loop.quit();
  });
  QObject::connect(&installer, &CadmiumInstaller::cancelled, &context,
                   [&] { terminal = true; loop.quit(); });
  QObject::connect(&deadline, &QTimer::timeout, &context, [&] {
    std::cerr << "Installer fixture timed out\n";
    installer.cancel();
    terminal = true;
    loop.quit();
  });
  deadline.start(65 * 60 * 1000);
  installer.install(directory, false);
  if (!terminal) loop.exec();
  return success && !installer.busy();
}

bool cancelAndLock(const QString &directory) {
  QObject context;
  CadmiumInstaller installer;
  bool cancelled = false, failed = false;
  QObject::connect(&installer, &CadmiumInstaller::cancelled, &context,
                   [&] { cancelled = true; });
  QObject::connect(&installer, &CadmiumInstaller::failed, &context,
                   [&](const QString &) { failed = true; });
  installer.install(directory, false);
  if (!installer.busy()) return false;
  // Cancellation during asynchronous existing-file verification must neither
  // start a replacement download nor alter the installed model files.
  installer.cancel();
  if (!cancelled || failed || installer.busy()) return false;
  QCoreApplication::processEvents();
  if (!verify(directory)) return false;
  QLockFile lock(QDir(directory).filePath(".cadmium-install.lock"));
  if (!lock.tryLock(0)) return false;
  installer.install(directory, false);
  if (!failed || installer.busy()) return false;
  lock.unlock();
  return verify(directory);
}
}

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  const QStringList arguments = application.arguments();
  if (arguments.size() < 2) {
    std::cout << "Provide a verified model folder or --download and an empty model folder\n";
    return 77;
  }
  const bool download = arguments[1] == "--download";
  const bool explicitCached = arguments[1] == "--cached";
  const int pathIndex = download || explicitCached ? 2 : 1;
  QTemporaryDir temporary;
  const QString directory = arguments.size() > pathIndex
      ? arguments[pathIndex] : temporary.path();
  if (directory.isEmpty() || (!download && arguments.size() <= pathIndex)) return 77;
  if (download) {
    for (const Artifact &artifact : Artifacts) {
      if (QFileInfo::exists(QDir(directory).filePath(QString::fromLatin1(artifact.name)))) {
        std::cerr << "Download fixture requires an empty destination\n";
        return 1;
      }
    }
    std::cout << "Qt TLS support: " << (QSslSocket::supportsSsl() ? "available" : "unavailable") << '\n';
    if (!install(directory) || !verify(directory)) {
      std::cerr << "Verified model download fixture failed\n";
      return 1;
    }
    std::cout << "Actual secure model download and verification passed\n";
  } else if (!verify(directory) || !install(directory)) {
    std::cerr << "Cached model verification fixture failed\n";
    return 1;
  }
  if (!cancelAndLock(directory)) {
    std::cerr << "Model cancellation or installation locking failed\n";
    return 1;
  }
  std::cout << "Installer verification, cancellation, and directory locking passed\n";
  return 0;
}
