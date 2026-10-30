#include "toonz/neuralstyle.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace NeuralStyle {
namespace {
constexpr quint32 Magic = 0x31535a47;
constexpr int ContentCount = 384 * 384 * 3;
constexpr int MaxReply = 16 + ContentCount * 4;
constexpr int TimeoutMs = 30000;
bool cancelled(const std::atomic_bool *flag) { return flag && flag->load(); }
void appendWord(QByteArray &bytes, quint32 value) {
  const quint32 little = qToLittleEndian(value);
  bytes.append(reinterpret_cast<const char *>(&little), 4);
}
quint32 word(const char *bytes) {
  return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes));
}
QByteArray pixels(const QImage &input) {
  const auto image = input.convertToFormat(QImage::Format_RGB32);
  QByteArray result;
  result.reserve(image.width() * image.height() * 12);
  for (int y = 0; y < image.height(); ++y) {
    const auto row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      const int channels[] = {qRed(row[x]), qGreen(row[x]), qBlue(row[x])};
      for (int channel : channels) {
        const float value = channel / 255.f;
        quint32 bits;
        std::memcpy(&bits, &value, 4);
        appendWord(result, bits);
      }
    }
  }
  return result;
}
}  // namespace

struct Session::Impl {
  QString directory;
  QThread *thread = QThread::currentThread();
  QProcess process;
  QByteArray reference;
  quint32 sequence = 0;
  explicit Impl(const QString &path) : directory(QDir(path).absolutePath()) {}
  ~Impl() { stop(); }
  void stop() {
    if (process.state() != QProcess::NotRunning) {
      process.kill();
      process.waitForFinished(3000);
    }
    process.readAllStandardOutput();
    process.readAllStandardError();
    reference.clear();
    sequence = 0;
  }
  bool fail(QString &error, const QString &message) {
    error = message;
    stop();
    return false;
  }
  bool verify(const QJsonObject &manifest, const QString &role,
              QString &path, QString &error, const std::atomic_bool *cancel) {
    const auto entry = manifest.value(role).toObject();
    const QString name = entry.value("file").toString();
    const auto expected = entry.value("sha256").toString().toLatin1();
    const double declared = entry.value("bytes").toDouble(-1);
    if (!std::isfinite(declared) || declared <= 0 || declared > 64 * 1024 * 1024 ||
        std::floor(declared) != declared)
      return fail(error, "Invalid neural component size.");
    const qint64 count = qint64(declared);
    if (name.isEmpty() || name == "." || name == ".." || name.contains('/') ||
        name.contains('\\') || name.contains(':') || expected.size() != 64 ||
        count <= 0 || count > 64 * 1024 * 1024)
      return fail(error, "Invalid neural component manifest.");
    if ((role == "prediction" && expected != "af6ad4b2e7aeba0675f32636082ab915ced5375229a3f8aff7e714c6213f5ed2") ||
        (role == "transfer" && expected != "7a1550643cf034a4d813c0aa276976cd15da4141b4f1ec3631db1d0d9c8e2cd1"))
      return fail(error, "Unsupported neural model revision.");
    path = QDir(directory).filePath(name);
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() != count)
      return fail(error, "Neural component is missing or has changed. Reinstall the component.");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, "Cannot read neural component.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QElapsedTimer timer;
    timer.start();
    qint64 consumed = 0;
    while (!file.atEnd()) {
      if (cancelled(cancel)) return fail(error, "Neural styling cancelled.");
      if (timer.elapsed() > TimeoutMs) return fail(error, "Neural component verification timed out.");
      const auto block = file.read(65536);
      if (block.isEmpty()) return fail(error, "Cannot read complete neural component.");
      consumed += block.size();
      if (consumed > count) return fail(error, "Neural component changed during verification.");
      hash.addData(block);
    }
    if (consumed != count || hash.result().toHex() != expected)
      return fail(error, "Neural component checksum mismatch. Reinstall the component.");
    return true;
  }
  bool receive(quint32 command, quint32 request, int count, QByteArray &payload,
               QString &error, const std::atomic_bool *cancel, QElapsedTimer &timer) {
    QByteArray reply;
    const int expected = 16 + count * 4;
    for (;;) {
      if (cancelled(cancel)) return fail(error, "Neural styling cancelled.");
      if (timer.elapsed() > TimeoutMs) return fail(error, "Neural worker timed out.");
      if (process.bytesAvailable() > MaxReply - reply.size())
        return fail(error, "Neural worker exceeded the response limit.");
      reply += process.readAllStandardOutput();
      // Drain diagnostics without retaining artwork-independent vendor chatter.
      process.readAllStandardError();
      if (reply.size() > expected) return fail(error, "Unexpected neural response length.");
      if (reply.size() >= 16 &&
          (word(reply.constData()) != Magic || word(reply.constData()+4) != command ||
           word(reply.constData()+8) != request || word(reply.constData()+12) != quint32(count)))
        return fail(error, "Neural worker protocol mismatch.");
      if (reply.size() == expected) {
        payload = reply.mid(16);
        for (int i = 0; i < count; ++i) {
          const quint32 bits = word(payload.constData() + i * 4);
          float value;
          std::memcpy(&value, &bits, 4);
          if (!std::isfinite(value)) return fail(error, "Neural worker returned nonfinite pixels.");
        }
        return true;
      }
      if (process.state() == QProcess::NotRunning)
        return fail(error, "Neural worker exited before completing the image.");
      process.waitForReadyRead(20);
    }
  }
  bool start(QString &error, const std::atomic_bool *cancel) {
    if (process.state() == QProcess::Running) return true;
    stop();
    QFile file(QDir(directory).filePath("package.json"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16384)
      return fail(error, "Neural style component is not installed.");
    const auto manifest = QJsonDocument::fromJson(file.readAll()).object();
    if (manifest.value("version").toInt() != 1 || manifest.value("protocol").toInt() != 1 ||
        manifest.value("runtimeVersion").toString() != "2.3.0")
      return fail(error, "Unsupported neural style component version.");
    QString worker, runtime, prediction, transfer;
    if (!verify(manifest, "worker", worker, error, cancel) ||
        !verify(manifest, "runtime", runtime, error, cancel) ||
        !verify(manifest, "prediction", prediction, error, cancel) ||
        !verify(manifest, "transfer", transfer, error, cancel)) return false;
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto &name : environment.keys())
      if (name.startsWith("PYTHON") || name.startsWith("DYLD_") || name.startsWith("LD_"))
        environment.remove(name);
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setStandardErrorFile(QProcess::nullDevice());
    process.setProgram(worker);
    process.setArguments({runtime, prediction, transfer});
    QElapsedTimer timer;
    timer.start();
    process.start();
    while (process.state() == QProcess::Starting) {
      if (cancelled(cancel)) return fail(error, "Neural styling cancelled.");
      if (timer.elapsed() > TimeoutMs) return fail(error, "Neural worker launch timed out.");
      process.waitForStarted(20);
    }
    QByteArray ready;
    return receive(0, 0, 0, ready, error, cancel, timer);
  }
  bool request(quint32 command, const QByteArray &input, int count,
               QByteArray &output, QString &error, const std::atomic_bool *cancel) {
    QByteArray message;
    appendWord(message, Magic);
    appendWord(message, command);
    appendWord(message, ++sequence);
    appendWord(message, quint32(input.size() / 4));
    message += input;
    QElapsedTimer timer;
    timer.start();
    if (process.write(message) != message.size()) return fail(error, "Cannot submit neural request.");
    while (process.bytesToWrite()) {
      if (cancelled(cancel)) return fail(error, "Neural styling cancelled.");
      if (timer.elapsed() > TimeoutMs) return fail(error, "Neural request timed out.");
      process.waitForBytesWritten(20);
      if (process.state() == QProcess::NotRunning) return fail(error, "Neural worker closed its input.");
    }
    return receive(command, sequence, count, output, error, cancel, timer);
  }
};

Session::Session(const QString &directory) : m_impl(new Impl(directory)) {
  static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                "Neural protocol requires IEEE float32");
}
Session::~Session() = default;
void Session::stop() {
  Q_ASSERT(QThread::currentThread() == m_impl->thread);
  if (QThread::currentThread() == m_impl->thread) m_impl->stop();
}
bool Session::style(const QImage &reference, const QImage &content, QImage &result,
                    QString &error, const std::atomic_bool *cancel) {
  error.clear();
  if (QThread::currentThread() != m_impl->thread) {
    error = "Neural session must remain on its owning thread.";
    return false;
  }
  if (reference.size() != QSize(256, 256) || content.size() != QSize(384, 384)) {
    error = "Neural inputs must match the qualified model dimensions.";
    return false;
  }
  for (const auto &image : {reference, content}) {
    const auto rgba = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < rgba.height(); ++y) {
      if (cancelled(cancel)) return m_impl->fail(error, "Neural styling cancelled.");
      const auto row = reinterpret_cast<const QRgb *>(rgba.constScanLine(y));
      for (int x = 0; x < rgba.width(); ++x)
        if (qAlpha(row[x]) != 255) {
          error = "Composite neural inputs onto their neutral background before inference.";
          return false;
        }
    }
  }
  if (!m_impl->start(error, cancel)) return false;
  QByteArray response;
  const auto style = pixels(reference);
  if (m_impl->reference != style) {
    if (!m_impl->request(1, style, 100, response, error, cancel)) return false;
    m_impl->reference = style;
  }
  if (!m_impl->request(2, pixels(content), ContentCount, response, error, cancel)) return false;
  QImage complete(384, 384, QImage::Format_RGB32);
  for (int y = 0; y < 384; ++y) {
    if (cancelled(cancel)) return m_impl->fail(error, "Neural styling cancelled.");
    auto row = reinterpret_cast<QRgb *>(complete.scanLine(y));
    for (int x = 0; x < 384; ++x) {
      int channels[3];
      for (int c = 0; c < 3; ++c) {
        const quint32 bits = word(response.constData() + ((y * 384 + x) * 3 + c) * 4);
        float value;
        std::memcpy(&value, &bits, 4);
        channels[c] = int(std::max(0.f, std::min(1.f, value)) * 255 + .5f);
      }
      row[x] = qRgb(channels[0], channels[1], channels[2]);
    }
  }
  result = complete;
  return true;
}
}  // namespace NeuralStyle
