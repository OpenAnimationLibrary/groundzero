#include "automation.h"
#include "automation_media.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace GroundzeroAutomation {
namespace {

void require(bool condition, const QString& message) {
  if (!condition) throw std::runtime_error(message.toStdString());
}

QString executableName(const QString& name) {
#ifdef _WIN32
  return name + ".exe";
#else
  return name;
#endif
}

class ProbeMode final {
  QByteArray m_previous = qgetenv("GROUNDZERO_TEST_PROBE_MODE");
  bool m_present = qEnvironmentVariableIsSet("GROUNDZERO_TEST_PROBE_MODE");

public:
  explicit ProbeMode(const QString& mode) {
    require(qputenv("GROUNDZERO_TEST_PROBE_MODE", mode.toUtf8()),
            "Cannot select the fixed native probe fixture mode");
  }
  ~ProbeMode() {
    if (m_present)
      qputenv("GROUNDZERO_TEST_PROBE_MODE", m_previous);
    else
      qunsetenv("GROUNDZERO_TEST_PROBE_MODE");
  }
};

struct Fixture {
  QString directory;
  MovieVerificationConfig config;
  QByteArray original = QByteArray(64, 'x');
};

Fixture prepareFixture(Context& context, const QString& name) {
  QTemporaryDir directory(
      QDir(context.workspace()).filePath("media-" + name + "-XXXXXX"));
  require(directory.isValid(),
          "Cannot create private verifier fixture storage");
  directory.setAutoRemove(false);
  Fixture fixture;
  fixture.directory = QFileInfo(directory.path()).canonicalFilePath();
  fixture.config.mediaToolsDirectory = fixture.directory;
  fixture.config.moviePath    = QDir(fixture.directory).filePath("owned.mp4");
  fixture.config.width        = 32;
  fixture.config.height       = 32;
  fixture.config.frameCount   = 3;
  fixture.config.frameRate    = 24;
  fixture.config.maximumBytes = 1024;
  const QString source = QDir(QCoreApplication::applicationDirPath())
                             .filePath(executableName("mcp_ffprobe_fixture"));
  const QString target =
      QDir(fixture.directory).filePath(executableName("ffprobe"));
  require(QFile::copy(source, target),
          "Cannot copy the native probe fixture: " + source);
  require(QFile::setPermissions(target, QFile::permissions(target) |
                                            QFile::ExeOwner | QFile::ExeGroup |
                                            QFile::ExeOther),
          "Cannot grant execution to the private probe fixture");
  QFile movie(fixture.config.moviePath);
  require(movie.open(QIODevice::WriteOnly), "Cannot create owned probe input");
  require(movie.write(fixture.original) == fixture.original.size(),
          "Cannot write complete owned probe input");
  movie.close();
  return fixture;
}

enum class Cancellation { None, BeforeProbe, RunningProbe };

struct Outcome {
  QJsonObject metadata;
  QString error;
  QPointer<QEventLoop> loop;
  int callbacks      = 0;
  bool forcedStop    = false;
  bool observedProbe = false;
  qint64 elapsedMs   = 0;
};

std::shared_ptr<Outcome> verify(const MovieVerificationConfig& config,
                                Cancellation cancellation) {
  auto outcome = std::make_shared<Outcome>();
  QEventLoop loop;
  QElapsedTimer elapsed;
  outcome->loop = &loop;
  elapsed.start();
  auto verification = startMovieVerification(
      config, [outcome](QJsonObject metadata, QString error) {
        ++outcome->callbacks;
        outcome->metadata = std::move(metadata);
        outcome->error    = std::move(error);
        if (outcome->loop) outcome->loop->quit();
      });
  QTimer deadline, probeStarted;
  deadline.setSingleShot(true);
  QObject::connect(&deadline, &QTimer::timeout, &loop,
                   [verification, outcome, &loop]() {
                     outcome->forcedStop = true;
                     verification->cancel();
                     QTimer::singleShot(5000, &loop, &QEventLoop::quit);
                   });
  QObject::connect(
      &probeStarted, &QTimer::timeout, &loop,
      [verification, outcome, config, &probeStarted]() {
        if (!QFileInfo::exists(config.moviePath + ".probe-started")) return;
        outcome->observedProbe = true;
        probeStarted.stop();
        verification->cancel();
        verification->cancel();
      });
  if (cancellation == Cancellation::BeforeProbe) verification->cancel();
  if (cancellation == Cancellation::RunningProbe) probeStarted.start(5);
  deadline.start(65000);
  loop.exec(QEventLoop::ExcludeUserInputEvents);
  deadline.stop();
  probeStarted.stop();
  outcome->elapsedMs = elapsed.elapsed();
  verification->cancel();
  QEventLoop settle;
  QTimer::singleShot(10, &settle, &QEventLoop::quit);
  settle.exec(QEventLoop::ExcludeUserInputEvents);
  require(!outcome->forcedStop,
          "Verifier exceeded its production deadline and grace period");
  require(outcome->callbacks == 1,
          "Verifier completion must occur exactly once");
  return outcome;
}

void requireFailure(const Outcome& result, const QString& reason) {
  require(result.metadata.isEmpty(),
          "Rejected verification published movie metadata");
  require(result.error.contains(reason),
          "Wrong verifier failure: " + result.error);
}

void checkEncodingMetadata(const QJsonObject& result) {
  require(result.value("codec").toString() == "mpeg4",
          "Verifier must report the actual decoded codec");
  require(result.value("pixel_format").toString() == "yuv420p",
          "Verifier changed the validated pixel format");
  require(result.value("audio").isBool() && !result.value("audio").toBool(),
          "Verifier must report that the movie has no audio");
  require(result.value("alpha").toString() == "opaque" &&
              result.value("matte").toString() == "black",
          "Verifier lost the native opaque black-matte profile");
}

void checkSuccess(Context& context) {
  const Fixture fixture = prepareFixture(context, "valid");
  const ProbeMode mode("valid");
  const auto outcome = verify(fixture.config, Cancellation::None);
  require(outcome->error.isEmpty(), "Valid probe failed: " + outcome->error);
  const QJsonObject result = outcome->metadata;
  checkEncodingMetadata(result);
  require(result.value("width").toInt() == 32 &&
              result.value("height").toInt() == 32,
          "Verifier changed decoded movie dimensions");
  require(result.value("frame_count").toInt() == 3 &&
              result.value("frame_rate").toDouble() == 24,
          "Verifier changed decoded movie frame count or rate");
  require(
      std::abs(result.value("duration_seconds").toDouble() - 0.125) < 0.000001,
      "Verifier changed decoded movie duration");
  require(result.value("container").toString() == "mp4" &&
              result.value("major_brand").toString() == "isom",
          "Verifier lost the validated MP4 container identity");
  const QByteArray hash =
      QCryptographicHash::hash(fixture.original, QCryptographicHash::Sha256);
  require(
      result.value("sha256").toString() == QString::fromLatin1(hash.toHex()) &&
          result.value("size_bytes").toDouble() == fixture.original.size(),
      "Verifier hash and size must identify the unchanged owned file");
}

void checkRejectedProbe(Context& context, const QString& name,
                        const QString& reason) {
  const Fixture fixture = prepareFixture(context, name);
  const ProbeMode mode(name);
  const auto outcome = verify(fixture.config, Cancellation::None);
  requireFailure(*outcome, reason);
  require(QFileInfo::exists(fixture.config.moviePath + ".probe-started"),
          "Probe rejection must execute the real subprocess boundary");
}

void checkDeadline(Context& context) {
  const Fixture fixture = prepareFixture(context, "deadline");
  const ProbeMode mode("delay");
  const auto outcome = verify(fixture.config, Cancellation::None);
  requireFailure(*outcome, "timed out");
  require(outcome->elapsedMs >= 55000 && outcome->elapsedMs < 65000,
          "Verifier must enforce the real sixty-second deadline");
  require(QFileInfo::exists(fixture.config.moviePath + ".probe-started"),
          "Timeout must interrupt an actual started probe");
}

void checkCancellation(Context& context, Cancellation cancellation) {
  const Fixture fixture = prepareFixture(context, "cancel");
  const ProbeMode mode("delay");
  const auto outcome = verify(fixture.config, cancellation);
  requireFailure(*outcome, "canceled");
  require(outcome->elapsedMs < 10000,
          "Canceled verification waited for its normal deadline");
  require(
      outcome->observedProbe == (cancellation == Cancellation::RunningProbe),
      "Cancellation did not exercise the intended subprocess lifetime");
}

void checkFileRejection(Context& context, const QString& defect) {
  Fixture fixture = prepareFixture(context, defect);
  const ProbeMode mode("valid");
  if (defect == "missing")
    require(QFile::remove(fixture.config.moviePath),
            "Cannot remove owned fixture file");
  if (defect == "empty") {
    QFile file(fixture.config.moviePath);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "Cannot empty fixture file");
  }
  if (defect == "oversized")
    fixture.config.maximumBytes = fixture.original.size() - 1;
  if (defect == "directory") fixture.config.moviePath = fixture.directory;
  const auto outcome = verify(fixture.config, Cancellation::None);
  requireFailure(*outcome, "owned file bounds");
  require(!QFileInfo::exists(fixture.config.moviePath + ".probe-started"),
          "Invalid owned file must fail before starting the probe");
}

void checkMissingProgram(Context& context) {
  const Fixture fixture = prepareFixture(context, "missing-program");
  require(QFile::remove(
              QDir(fixture.directory).filePath(executableName("ffprobe"))),
          "Cannot remove private probe executable");
  const auto outcome = verify(fixture.config, Cancellation::None);
  requireFailure(*outcome, "Cannot start");
}

void recordCase(const QString& name, const std::function<void()>& action,
                QJsonArray& reports, QStringList& failures) {
  QJsonObject report{{"case", name}};
  try {
    action();
    report.insert("status", "passed");
    std::cout << "automation-media-test: " << name.toStdString() << " PASS"
              << std::endl;
  } catch (const std::exception& error) {
    const QString message = QString::fromUtf8(error.what());
    report.insert("status", "failed");
    report.insert("error", message);
    failures.append(name + ": " + message);
    std::cerr << "automation-media-test: " << name.toStdString()
              << " FAIL: " << error.what() << std::endl;
  }
  reports.append(report);
}

}  // namespace

void runAutomationMediaProcessTests(Context& context) {
  QJsonArray reports;
  QStringList failures;
  recordCase("valid", [&] { checkSuccess(context); }, reports, failures);
  const QStringList invalidMetadata{
      "wrong-container", "wrong-brand",    "wrong-count",  "wrong-dimensions",
      "wrong-rate",      "wrong-duration", "wrong-pixfmt", "audio",
      "extra-stream",    "missing-codec",  "malformed"};
  for (const QString& mode : invalidMetadata)
    recordCase(
        mode, [&] { checkRejectedProbe(context, mode, "Decoded movie"); },
        reports, failures);
  const QMap<QString, QString> failuresByMode{{"nonzero", "could not decode"},
                                              {"overflow", "output bound"},
                                              {"tamper", "changed"}};
  for (auto item = failuresByMode.cbegin(); item != failuresByMode.cend();
       ++item)
    recordCase(
        item.key(),
        [&] { checkRejectedProbe(context, item.key(), item.value()); }, reports,
        failures);
  for (const QString& defect :
       QStringList{"missing", "empty", "oversized", "directory"})
    recordCase(
        "file-" + defect, [&] { checkFileRejection(context, defect); }, reports,
        failures);
  recordCase(
      "missing-program", [&] { checkMissingProgram(context); }, reports,
      failures);
  recordCase(
      "cancel-before-probe",
      [&] { checkCancellation(context, Cancellation::BeforeProbe); }, reports,
      failures);
  recordCase(
      "cancel-running-probe",
      [&] { checkCancellation(context, Cancellation::RunningProbe); }, reports,
      failures);
  recordCase(
      "production-deadline", [&] { checkDeadline(context); }, reports,
      failures);
  QFile report(
      QDir(context.workspace()).filePath("media-verifier-report.json"));
  require(report.open(QIODevice::WriteOnly),
          "Cannot retain native verifier report");
  const QByteArray bytes =
      QJsonDocument(reports).toJson(QJsonDocument::Indented);
  require(report.write(bytes) == bytes.size(),
          "Cannot retain complete native verifier report");
  require(failures.isEmpty(), failures.join("; "));
}

}  // namespace GroundzeroAutomation
