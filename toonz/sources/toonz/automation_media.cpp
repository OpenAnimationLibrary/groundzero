#include "automation_media.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <utility>

namespace GroundzeroAutomation {
namespace {

constexpr int MaximumProbeBytes = 1024 * 1024;
constexpr qint64 HashChunkBytes = 1024 * 1024;

bool near(double actual, double expected) {
  return std::isfinite(actual) && actual > 0 &&
         std::abs(actual - expected) <= std::max(0.000001, expected * 0.000006);
}

double rate(const QString& text) {
  const QStringList parts = text.split('/');
  if (parts.size() != 2) return 0;
  bool validNumerator = false, validDenominator = false;
  const double numerator   = parts[0].toDouble(&validNumerator);
  const double denominator = parts[1].toDouble(&validDenominator);
  if (!validNumerator || !validDenominator || denominator <= 0) return 0;
  return numerator / denominator;
}

bool validStream(const QJsonObject& stream,
                 const MovieVerificationConfig& config) {
  return stream.value("codec_type").toString() == "video" &&
         stream.value("pix_fmt").toString() == "yuv420p" &&
         !stream.value("codec_name").toString().isEmpty() &&
         stream.value("width").toInt() == config.width &&
         stream.value("height").toInt() == config.height;
}

bool validDuration(double duration, double expected) {
  // MP4 movie timescales can round container duration to one millisecond.
  return std::isfinite(duration) && duration > 0 &&
         std::abs(duration - expected) <= std::max(0.0011, expected * 0.000006);
}

QJsonObject movieTiming(const QJsonObject& stream, const QJsonObject& format,
                        const MovieVerificationConfig& config) {
  bool validCount = false, durationParsed = false;
  const int count =
      stream.value("nb_read_frames").toString().toInt(&validCount);
  const double fps = rate(stream.value("avg_frame_rate").toString());
  if (!validCount || count != config.frameCount || !near(fps, config.frameRate))
    return {};
  const double duration =
      format.value("duration").toString().toDouble(&durationParsed);
  if (!durationParsed || !validDuration(duration, double(count) / fps))
    return {};
  return {{"frame_count", count},
          {"frame_rate", fps},
          {"duration_seconds", duration}};
}

bool validContainer(const QJsonObject& format) {
  const QStringList brands{"isom", "iso2", "iso3", "iso4", "iso5",
                           "iso6", "mp41", "mp42", "avc1"};
  return format.value("format_name").toString().split(',').contains("mp4") &&
         brands.contains(
             format.value("tags").toObject().value("major_brand").toString());
}

QJsonObject movieMetadata(const QByteArray& bytes,
                          const MovieVerificationConfig& config) {
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    return {};
  const QJsonObject object = document.object();
  const QJsonArray streams = object.value("streams").toArray();
  if (streams.size() != 1) return {};
  const QJsonObject stream = streams[0].toObject();
  if (!validStream(stream, config)) return {};
  const QJsonObject format = object.value("format").toObject();
  if (!validContainer(format)) return {};
  QJsonObject metadata = movieTiming(stream, format, config);
  if (metadata.isEmpty()) return {};
  metadata.insert("width", config.width);
  metadata.insert("height", config.height);
  metadata.insert("codec", stream.value("codec_name"));
  metadata.insert("pixel_format", "yuv420p");
  metadata.insert("audio", false);
  metadata.insert("alpha", "opaque");
  metadata.insert("matte", "black");
  metadata.insert("container", "mp4");
  metadata.insert("major_brand",
                  format.value("tags").toObject().value("major_brand"));
  return metadata;
}

class Verification final : public MovieVerification,
                           public std::enable_shared_from_this<Verification> {
  MovieVerificationConfig m_config;
  MovieVerificationCompletion m_completion;
  QProcess m_process;
  QTimer m_deadline;
  QFile m_file;
  QCryptographicHash m_hash{QCryptographicHash::Sha256};
  QByteArray m_stdout, m_stderr, m_initialHash;
  QJsonObject m_metadata;
  QString m_failure;
  qint64 m_size     = 0;
  qint64 m_hashed   = 0;
  bool m_secondHash = false;
  bool m_done       = false;

public:
  Verification(MovieVerificationConfig config,
               MovieVerificationCompletion completion)
      : m_config(std::move(config)), m_completion(std::move(completion)) {}

  void start() {
    const auto weak = weak_from_this();
    QObject::connect(&m_deadline, &QTimer::timeout, &m_process, [weak]() {
      if (auto self = weak.lock()) self->abort("Movie verification timed out.");
    });
    QObject::connect(&m_process, &QProcess::readyReadStandardOutput, &m_process,
                     [weak]() {
                       if (auto self = weak.lock()) self->collect();
                     });
    QObject::connect(&m_process, &QProcess::readyReadStandardError, &m_process,
                     [weak]() {
                       if (auto self = weak.lock()) self->collect();
                     });
    QObject::connect(
        &m_process, &QProcess::errorOccurred, &m_process,
        [weak](QProcess::ProcessError error) {
          if (error == QProcess::FailedToStart)
            if (auto self = weak.lock())
              self->finish("Cannot start the configured ffprobe executable.");
        });
    QObject::connect(&m_process,
                     qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     &m_process, [weak](int code, QProcess::ExitStatus status) {
                       if (auto self = weak.lock()) self->probed(code, status);
                     });
    m_deadline.setSingleShot(true);
    m_deadline.start(60000);
    beginHash();
  }

  void cancel() override { abort("Movie verification canceled."); }

private:
  bool validFile() const {
    const QFileInfo info(m_config.moviePath);
    return info.isFile() && !info.isSymLink() && info.size() > 0 &&
           info.size() <= m_config.maximumBytes &&
           info.canonicalFilePath() == QDir::cleanPath(m_config.moviePath);
  }

  void beginHash() {
    if (m_done) return;
    if (!validFile())
      return finish("The movie is missing or outside its owned file bounds.");
    const qint64 size = QFileInfo(m_config.moviePath).size();
    if (m_secondHash && size != m_size)
      return finish("The movie changed during verification.");
    m_size   = size;
    m_hashed = 0;
    m_hash.reset();
    m_file.setFileName(m_config.moviePath);
    if (!m_file.open(QIODevice::ReadOnly))
      return finish("Cannot read the rendered movie.");
    scheduleHash();
  }

  void scheduleHash() {
    const auto weak = weak_from_this();
    QTimer::singleShot(0, &m_process, [weak]() {
      if (auto self = weak.lock()) self->hashChunk();
    });
  }

  void hashChunk() {
    if (m_done) return;
    const QByteArray bytes = m_file.read(HashChunkBytes);
    m_hashed += bytes.size();
    if (m_file.error() != QFileDevice::NoError || m_hashed > m_size)
      return finish("Cannot read a stable complete movie file.");
    m_hash.addData(bytes);
    if (!m_file.atEnd()) return scheduleHash();
    m_file.close();
    if (m_hashed != m_size || !validFile())
      return finish("The movie changed while its hash was calculated.");
    if (m_secondHash) return checked();
    m_initialHash = m_hash.result().toHex();
    probe();
  }

  void probe() {
#ifdef _WIN32
    const QString executable = "ffprobe.exe";
#else
    const QString executable = "ffprobe";
#endif
    const QString program =
        QDir(m_config.mediaToolsDirectory).filePath(executable);
    const QStringList arguments{
        "-v",
        "error",
        "-count_frames",
        "-show_entries",
        "stream=codec_type,codec_name,pix_fmt,width,height,nb_read_frames,avg_"
        "frame_rate:format=duration",
        "-of",
        "json",
        m_config.moviePath};
    m_process.setProgram(program);
    m_process.setArguments(arguments);
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    m_process.setStandardInputFile(QProcess::nullDevice());
    m_process.start();
  }

  void collect() {
    m_process.setReadChannel(QProcess::StandardOutput);
    m_stdout += m_process.read(
        std::max<qint64>(0, MaximumProbeBytes + 1 - m_stdout.size()));
    m_process.setReadChannel(QProcess::StandardError);
    m_stderr += m_process.read(
        std::max<qint64>(0, MaximumProbeBytes + 1 - m_stderr.size()));
    if (m_stdout.size() > MaximumProbeBytes ||
        m_stderr.size() > MaximumProbeBytes)
      abort("The movie probe exceeded its diagnostic output bound.");
  }

  void probed(int code, QProcess::ExitStatus status) {
    if (m_done) return;
    collect();
    if (!m_failure.isEmpty()) return finish(m_failure);
    if (status != QProcess::NormalExit || code != 0)
      return finish("ffprobe could not decode the complete rendered movie.");
    m_metadata = movieMetadata(m_stdout, m_config);
    if (m_metadata.isEmpty())
      return finish(
          "Decoded movie streams, dimensions, frame count, timing, or format "
          "differ.");
    m_secondHash = true;
    beginHash();
  }

  void checked() {
    if (m_hash.result().toHex() != m_initialHash)
      return finish("The rendered movie changed during the independent probe.");
    m_metadata.insert("sha256", QString::fromLatin1(m_initialHash));
    m_metadata.insert("size_bytes", double(m_size));
    finish({});
  }

  void abort(const QString& error) {
    if (m_done) return;
    m_failure = error;
    if (m_process.state() != QProcess::NotRunning)
      m_process.kill();
    else
      finish(error);
  }

  void finish(const QString& error) {
    if (m_done) return;
    m_done = true;
    m_deadline.stop();
    m_file.close();
    const auto self = shared_from_this();
    QTimer::singleShot(0, &m_process, [self, error]() {
      auto completion = std::move(self->m_completion);
      completion(error.isEmpty() ? self->m_metadata : QJsonObject{}, error);
    });
  }
};

}  // namespace

std::shared_ptr<MovieVerification> startMovieVerification(
    MovieVerificationConfig config, MovieVerificationCompletion completion) {
  auto verification =
      std::make_shared<Verification>(std::move(config), std::move(completion));
  verification->start();
  return verification;
}

}  // namespace GroundzeroAutomation
