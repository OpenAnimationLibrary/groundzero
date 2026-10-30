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
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <deque>

namespace NeuralStyle {
namespace {
constexpr quint32 Magic = 0x31535a47;
constexpr int ContentCount = 384 * 384 * 3;
constexpr int MaxReply = 16 + ContentCount * 4;
constexpr int TimeoutMs = 30000;
std::mutex workerMutex;
std::condition_variable workerAvailable;
bool workerBusy = false;
quint64 nextTicket = 0;
std::deque<quint64> waiting;
bool cancelled(Cancellation flag) { return flag(); }
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
  bool ownsLease = false;
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
    if (ownsLease) {
      { std::lock_guard<std::mutex> lock(workerMutex); workerBusy = false; ownsLease = false; }
      workerAvailable.notify_all();
    }
  }
  bool fail(QString &error, const QString &message) {
    error = message;
    stop();
    return false;
  }
  bool verify(const QJsonObject &manifest, const QString &role,
              QString &path, QString &error, Cancellation cancel) {
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
               QString &error, Cancellation cancel, QElapsedTimer &timer) {
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
  bool start(QString &error, Cancellation cancel) {
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
    {
      std::unique_lock<std::mutex> lock(workerMutex);
      // At most eight admitted waiters; further callers receive cancellable
      // backpressure. Queue time is not an inference timeout: valid active
      // work may take a full shot, and must not make another request fail.
      while (waiting.size() >= 8) {
        if (cancelled(cancel)) return fail(error, "Neural styling cancelled.");
        workerAvailable.wait_for(lock, std::chrono::milliseconds(20));
      }
      const quint64 ticket = ++nextTicket;
      waiting.push_back(ticket);
      for (;;) {
        if (cancelled(cancel)) {
          waiting.erase(std::find(waiting.begin(), waiting.end(), ticket));
          workerAvailable.notify_all();
          return fail(error, "Neural styling cancelled.");
        }
        if (!workerBusy && waiting.front() == ticket) {
          waiting.pop_front(); workerBusy = true; ownsLease = true;
          workerAvailable.notify_all(); break;
        }
        workerAvailable.wait_for(lock, std::chrono::milliseconds(20));
      }
    }
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
               QByteArray &output, QString &error, Cancellation cancel) {
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
                    QString &error, Cancellation cancel, Progress progress) {
  std::vector<QImage> images;
  if (!styleBatch(reference, {content}, images, error, cancel, progress)) return false;
  result = images.front();
  return true;
}
bool Session::styleBatch(const QImage &reference, const std::vector<QImage> &contents,
                         std::vector<QImage> &results, QString &error, Cancellation cancel, Progress progress) {
  error.clear();
  if (QThread::currentThread() != m_impl->thread) {
    error = "Neural session must remain on its owning thread.";
    return false;
  }
  struct Release { Impl *impl; ~Release() { impl->stop(); } } release{m_impl.get()};
  if (reference.size() != QSize(256, 256) || contents.empty() || contents.size() > 120) {
    error = "Neural inputs require a 256-square reference and one to 120 frames.";
    return false;
  }
  std::vector<QImage> validation{reference};
  validation.insert(validation.end(), contents.begin(), contents.end());
  for (size_t i=0; i<validation.size(); ++i) {
    const auto &image = validation[i];
    if (i && image.size() != QSize(384,384)) {
      error = "Neural frames must be 384 square."; return false;
    }
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
  if (!m_impl->request(1, pixels(reference), 100, response, error, cancel)) return false;
  std::vector<QImage> complete;
  complete.reserve(contents.size());
  for (size_t i=0; i<contents.size(); ++i) {
    if (cancelled(cancel)) return m_impl->fail(error, "Neural styling cancelled.");
    if (progress) progress(int(i),int(contents.size()));
    if (i && contents[i] == contents[i-1]) { complete.push_back(complete.back()); continue; }
    if (!m_impl->request(2, pixels(contents[i]), ContentCount, response, error, cancel)) return false;
    QImage image(384, 384, QImage::Format_RGB32);
    for (int y = 0; y < 384; ++y) {
      if (cancelled(cancel)) return m_impl->fail(error, "Neural styling cancelled.");
      auto row = reinterpret_cast<QRgb *>(image.scanLine(y));
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
    complete.push_back(image);
  }
  results = std::move(complete);
  return true;
}
}  // namespace NeuralStyle
