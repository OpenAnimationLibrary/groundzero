#include "toonz/neuralstyleexecution.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace NeuralStyle {
namespace {
constexpr qint64 FileLimit     = 64 * 1024 * 1024;
constexpr qint64 ManifestLimit = 16 * 1024;

bool fail(QString& error, const QString& message) {
  error = message;
  return false;
}

bool safeName(const QString& name) {
  if (name.isEmpty() || name == "." || name == "..") return false;
  for (const QChar forbidden : QString::fromLatin1("\0/\\:", 4))
    if (name.contains(forbidden)) return false;
  return name.size() <= 128;
}

bool expired(WorkerExecution::Clock::time_point deadline, QString& error) {
  if (WorkerExecution::Clock::now() < deadline) return false;
  error = "Neural preview exceeded its total work budget.";
  return true;
}

bool trustedDirectory(const QString& directory, QString& error) {
  const QFileInfo root(directory);
  if (!root.isDir() || root.isSymLink() ||
      root.canonicalFilePath() != directory)
    return fail(
        error,
        "Neural component directory must be canonical and not a symlink.");
  return true;
}

bool manifestMetadata(const QFileInfo& info, QString& error) {
  if (info.isSymLink() || !info.isFile() || info.size() <= 0 ||
      info.size() > ManifestLimit)
    return fail(error,
                "Neural component manifest is missing, unsafe or oversized.");
  return true;
}

bool manifestBytes(const QString& directory, QByteArray& bytes,
                   QString& error) {
  if (!trustedDirectory(directory, error)) return false;
  const QFileInfo info(QDir(directory).filePath("package.json"));
  if (!manifestMetadata(info, error)) return false;
  QFile file(info.filePath());
  if (!file.open(QIODevice::ReadOnly))
    return fail(error, "Cannot read neural component manifest.");
  bytes = file.read(ManifestLimit);
  if (file.error() != QFile::NoError || bytes.size() != info.size() ||
      !file.atEnd())
    return fail(error, "Neural component manifest changed while reading.");
  return true;
}

bool declaration(const QJsonObject& record, QString& error) {
  const double size = record.value("bytes").toDouble(-1);
  if (!std::isfinite(size) || size <= 0 || size > FileLimit ||
      std::floor(size) != size)
    return fail(error, "Neural component size declaration is invalid.");
  const QByteArray hash = record.value("sha256").toString().toLatin1();
  if (!safeName(record.value("file").toString()) || hash.size() != 64 ||
      QByteArray::fromHex(hash).toHex() != hash)
    return fail(error,
                "Neural component filename or hash declaration is invalid.");
  return true;
}

bool digestFile(QFile& file, qint64 size, const WorkerExecution& execution,
                QByteArray& digest, QString& error) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  qint64 consumed = 0;
  while (!file.atEnd()) {
    if (expired(execution.deadline, error)) return false;
    const QByteArray block = file.read(65536);
    consumed += block.size();
    if (block.isEmpty() || consumed > size)
      return fail(error, "Neural component file changed while reading.");
    hash.addData(block);
  }
  if (file.error() != QFile::NoError || consumed != size)
    return fail(error, "Neural component file changed while reading.");
  digest = hash.result().toHex();
  return true;
}

bool contents(const WorkerExecution& execution, const QJsonObject& record,
              QString& error) {
  if (!declaration(record, error)) return false;
  const QFileInfo info(
      QDir(execution.directory).filePath(record.value("file").toString()));
  const qint64 size = qint64(record.value("bytes").toDouble());
  if (info.isSymLink() || !info.isFile() || info.size() != size)
    return fail(error,
                "Neural component file is missing or changed; reinstall the "
                "component.");
  QFile file(info.filePath());
  if (!file.open(QIODevice::ReadOnly))
    return fail(error, "Cannot read neural component file.");
  QByteArray hash;
  if (!digestFile(file, size, execution, hash, error)) return false;
  if (hash != record.value("sha256").toString().toLatin1())
    return fail(error,
                "Neural component checksum changed; reinstall the component.");
  return true;
}

bool modelPins(const QJsonObject& manifest, QString& error) {
  const QString prediction =
      manifest.value("prediction").toObject().value("sha256").toString();
  const QString transfer =
      manifest.value("transfer").toObject().value("sha256").toString();
  if (prediction !=
          "af6ad4b2e7aeba0675f32636082ab915ced5375229a3f8aff7e714c6213f5ed2" ||
      transfer !=
          "7a1550643cf034a4d813c0aa276976cd15da4141b4f1ec3631db1d0d9c8e2cd1")
    return fail(error, "Neural component uses an unsupported model revision.");
  return true;
}
bool parseManifest(const QByteArray& bytes, QJsonObject& manifest,
                   QString& error) {
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(bytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject())
    return fail(error, "Neural component manifest is invalid JSON.");
  manifest = document.object();
  if (manifest.value("version").toDouble(-1) != 1 ||
      manifest.value("protocol").toDouble(-1) != 1 ||
      manifest.value("runtimeVersion").toString() != "2.3.0")
    return fail(error, "Neural component version is unsupported.");
  return modelPins(manifest, error);
}
}  // namespace

WorkerExecution::WorkerExecution(QString packageDirectory, QString manifestHash,
                                 QJsonObject identities,
                                 Clock::time_point expires)
    : directory(std::move(packageDirectory))
    , manifestSha256(std::move(manifestHash))
    , files(std::move(identities))
    , cacheNamespace(QUuid::createUuid().toString())
    , deadline(expires) {}

bool WorkerExecution::expired() const { return Clock::now() >= deadline; }

bool WorkerExecution::verify(QString& error) const {
  error.clear();
  if (::NeuralStyle::expired(deadline, error)) return false;
  QByteArray bytes;
  if (!manifestBytes(directory, bytes, error)) return false;
  if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() !=
      manifestSha256.toLatin1())
    return fail(error,
                "Neural package identity changed after preview admission.");
  for (auto entry = files.begin(); entry != files.end(); ++entry)
    if (!contents(*this, entry.value().toObject(), error)) return false;
  return !::NeuralStyle::expired(deadline, error);
}

void WorkerExecution::helperStarting() const { ++m_helpers; }
void WorkerExecution::helperClosed() const { --m_helpers; }
void WorkerExecution::helperKilled() const { m_killed = true; }
bool WorkerExecution::helperWasKilled() const { return m_killed.load(); }
bool WorkerExecution::helpersClosed() const { return m_helpers.load() == 0; }
std::unique_lock<std::timed_mutex> WorkerExecution::lockCompute(
    const int* cancel) const {
  const auto interrupted = [&]() { return expired() || (cancel && *cancel); };
  std::unique_lock<std::timed_mutex> lock(m_compute, std::defer_lock);
  while (!interrupted()) {
    if (lock.try_lock_for(std::chrono::milliseconds(20))) {
      if (interrupted()) lock.unlock();
      return lock;
    }
  }
  return lock;
}
[[noreturn]] void WorkerExecution::closureFailed() const { std::_Exit(74); }

std::shared_ptr<const WorkerExecution> captureWorkerExecution(
    const QString& directory, WorkerExecution::Clock::time_point deadline,
    QString& error) {
  error.clear();
  if (expired(deadline, error)) return {};
  QByteArray bytes;
  if (!manifestBytes(directory, bytes, error)) return {};
  QJsonObject manifest;
  if (!parseManifest(bytes, manifest, error)) return {};
  QJsonObject files;
  for (const QString& role : {"worker", "runtime", "prediction", "transfer"})
    files.insert(role, manifest.value(role));
  auto execution = std::make_shared<WorkerExecution>(
      directory,
      QString::fromLatin1(
          QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()),
      files, deadline);
  if (!execution->verify(error)) return {};
  return execution;
}
}  // namespace NeuralStyle
