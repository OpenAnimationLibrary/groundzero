#include "cadmiumruntime.h"
#include "cadmiumcodec.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPointer>
#include <QProcessEnvironment>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QTcpServer>
#include <QThread>
#include <QUrl>
#include <functional>
#include <memory>
#include <cmath>

namespace {
constexpr qint64 MaxBody = 256 * 1024 * 1024;
constexpr qint64 MaxResponse = 128 * 1024 * 1024;
struct Model { const char *name; qint64 size; const char *hash; bool optional; };
struct VerifiedFile {
  QString path;
  qint64 size;
  QByteArray hash;
  bool runtime;
};

bool runtimeFiles(const QFileInfo &executable, QVector<VerifiedFile> &files) {
  const QDir directory(executable.absolutePath());
  QFile manifest(directory.filePath(QStringLiteral("runtime-manifest.json")));
#ifndef Q_OS_WIN
  // Unpackaged local workers remain available for development on other hosts.
  if (!manifest.exists()) return true;
#endif
  if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > 65536) return false;
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(manifest.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) return false;
  const QJsonObject object = document.object();
  if (object.value("format").toDouble() != 1 ||
      object.value("source").toString() != QStringLiteral("d817cae470a1499df4985be460e0fdc721233987") ||
      object.value("platform").toString() != QStringLiteral("windows-x64") ||
      executable.fileName() != QStringLiteral("cadmium-sidecar.exe")) return false;
#ifndef Q_OS_WIN
  return false;
#else
  QSet<QString> expected{QStringLiteral("cadmium-sidecar.exe"),
      QStringLiteral("onnxruntime.dll"), QStringLiteral("onnxruntime_providers_shared.dll"),
      QStringLiteral("DirectML.dll")};
  const QJsonArray records = object.value("files").toArray();
  if (records.size() != expected.size()) return false;
  for (const auto &value : records) {
    if (!value.isObject()) return false;
    const auto record = value.toObject();
    const QString name = record.value("file").toString();
    const double bytes = record.value("bytes").toDouble(-1);
    const QByteArray hash = record.value("sha256").toString().toLatin1();
    if (!expected.remove(name) || !std::isfinite(bytes) || bytes <= 0 ||
        bytes > 1024.0 * 1024 * 1024 || std::floor(bytes) != bytes ||
        hash.size() != 64 || QByteArray::fromHex(hash).toHex() != hash) return false;
    const QString path = directory.filePath(name);
    const QFileInfo file(path);
    if (!file.isFile() || file.isSymLink() || file.size() != qint64(bytes)) return false;
    files.append({path, qint64(bytes), hash, true});
  }
  return expected.isEmpty();
#endif
}
// Pinned model manifest. Each artifact is checked before the worker sees it.
const Model Models[] = {
 {"ant_v2_fp32.onnx", 1388610539LL, "6c8d95cbfa62408f3cbfbbc29204fac7df5a587466dbb751e7e1b0bb530ff12b", false},
 {"gap_closer_fp32.onnx", 497500380LL, "3a9a792f72fd3451c36145a0657328e333d6e5e982f0760b5e5a6e7e9934d327", false},
 {"ant_v2_fp32_tiledscatter.onnx", 1389055751LL, "64a89282e0f0a18e182257718341dfc0838481ec63d1bb5fb45bad82b25814d8", true},
 {"gap_closer_fp16.onnx", 248798392LL, "1976b93409591384b2177fab1a5b84ee9866ad1c2cfabdc1154e7835fead33d6", true}
};
QSettings preferences() { return QSettings(QStringLiteral("OpenToonz"), QStringLiteral("Cadmium")); }
QString pngUri(const QImage &image) {
  QByteArray png;
  QBuffer buffer(&png);
  buffer.open(QIODevice::WriteOnly);
  if (!image.save(&buffer, "PNG")) return {};
  return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}
QJsonObject warmupColorize() {
  QImage lines(128, 128, QImage::Format_ARGB32);
  lines.fill(qRgba(255, 255, 255, 0));
  QPainter painter(&lines);
  painter.setPen(QPen(Qt::black, 3));
  painter.drawRect(24, 24, 80, 80);
  painter.end();
  QImage labels(128, 128, QImage::Format_Grayscale8);
  labels.fill(0);
  for (int y = 26; y < 103; ++y)
    for (int x = 26; x < 103; ++x) labels.scanLine(y)[x] = 1;
  QJsonArray colors;
  colors.append(QJsonArray{0, 0, 0, 0});
  colors.append(QJsonArray{180, 80, 40, 255});
  return {{"target_line_image_uri", Cadmium::lineImageUri(lines)},
          {"target_seg_map_uri", pngUri(labels)},
          {"references", QJsonArray{QJsonObject{
            {"line_image_uri", Cadmium::lineImageUri(lines)},
            {"seg_map_uri", pngUri(labels)}, {"colors_rgba", colors}}}},
          {"return_colorized", false}, {"verbose", false}};
}
}

CadmiumRuntime *CadmiumRuntime::instance() {
  static CadmiumRuntime *runtime = new CadmiumRuntime(QCoreApplication::instance());
  return runtime;
}

CadmiumRuntime::CadmiumRuntime(QObject *parent)
    : QObject(parent), m_process(new QProcess(this)),
      m_network(new QNetworkAccessManager(this)), m_status(tr("Stopped")) {
  m_network->setProxy(QNetworkProxy::NoProxy);
  m_process->setProcessChannelMode(QProcess::SeparateChannels);
  m_healthTimer.setInterval(250);
  m_deadline.setSingleShot(true);
  m_idleTimer.setSingleShot(true);
  connect(&m_healthTimer, &QTimer::timeout, this, [this] {
    if (m_state == Starting && m_listening && !m_reply)
      checkHealth(m_generation);
  });
  connect(&m_deadline, &QTimer::timeout, this, [this] {
    fail(tr("Cadmium timed out. Unload the models and try again."));
  });
  connect(&m_idleTimer, &QTimer::timeout, this, &CadmiumRuntime::stop);
  // Consume output continuously, but never expose host reports or local paths.
  auto readOutput = [this](const QByteArray &bytes) {
    m_output += bytes;
    if (m_state == Starting && m_output.contains("cadmium-sidecar listening")) {
      m_listening = true;
      checkHealth(m_generation);
    }
    if (m_output.size() > 16384) m_output = m_output.right(8192);
  };
  connect(m_process, &QProcess::readyReadStandardOutput, this,
          [this, readOutput] { readOutput(m_process->readAllStandardOutput()); });
  connect(m_process, &QProcess::readyReadStandardError, this,
          [this, readOutput] { readOutput(m_process->readAllStandardError()); });
  connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
    if (m_state != Stopped && m_state != Stopping && m_state != Error)
      fail(tr("The Cadmium worker could not run. Check its installation."));
  });
  connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
          this, [this](int, QProcess::ExitStatus) {
    if (m_state != Stopped && m_state != Stopping && m_state != Error)
      fail(tr("The Cadmium worker stopped. Pending work was discarded."));
  });
  connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
          this, &CadmiumRuntime::stop);
}

CadmiumRuntime::~CadmiumRuntime() {
  stop();
  if (m_verifier) m_verifier->wait();
  if (m_process->state() != QProcess::NotRunning) {
    m_process->kill();
    m_process->waitForFinished(1000);
  }
}

int CadmiumRuntime::startupMode() const {
  int mode = preferences().value("startupMode", OnDemand).toInt();
  return mode >= Off && mode <= Preload ? mode : OnDemand;
}
void CadmiumRuntime::setStartupMode(int mode) {
  if (mode < Off || mode > Preload) return;
  preferences().setValue("startupMode", mode);
  if (mode == Off) stop();
  else if (mode == Preload) start();
  emit statusChanged();
}
QString CadmiumRuntime::executablePath() const {
#ifdef Q_OS_WIN
  const QString name = QStringLiteral("cadmium-sidecar.exe");
#else
  const QString name = QStringLiteral("cadmium-sidecar");
#endif
  return preferences().value("executable", QDir(QCoreApplication::applicationDirPath()).filePath(name)).toString();
}
void CadmiumRuntime::setExecutablePath(const QString &path) {
  if (path == executablePath()) return;
  stop(); preferences().setValue("executable", path); emit statusChanged();
}
QString CadmiumRuntime::modelsDirectory() const {
  return preferences().value("modelsDirectory",
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("cadmium-models")).toString();
}
void CadmiumRuntime::setModelsDirectory(const QString &path) {
  if (path == modelsDirectory()) return;
  stop(); preferences().setValue("modelsDirectory", path); emit statusChanged();
}
int CadmiumRuntime::idleUnloadMinutes() const {
  return qBound(0, preferences().value("idleUnloadMinutes", 0).toInt(), 1440);
}
void CadmiumRuntime::setIdleUnloadMinutes(int minutes) {
  preferences().setValue("idleUnloadMinutes", qBound(0, minutes, 1440));
  armIdleTimer(); emit statusChanged();
}
bool CadmiumRuntime::isReady() const { return m_state == Ready; }
bool CadmiumRuntime::busy() const {
  return m_state == Verifying || m_state == Starting || m_state == Warming ||
         m_reply || !m_queue.isEmpty();
}
QString CadmiumRuntime::statusText() const { return m_status; }
void CadmiumRuntime::setState(State state, const QString &text) {
  m_state = state; m_status = text; emit statusChanged();
}

void CadmiumRuntime::start() {
  if (m_state == Ready || m_state == Starting || m_state == Warming || m_state == Verifying) return;
  if (startupMode() == Off) {
    emit failed(tr("Cadmium is off. Choose Load when needed or Preload in Cadmium settings."));
    return;
  }
  if (m_process->state() != QProcess::NotRunning || m_verifier) {
    fail(tr("Cadmium is stopping. Try again shortly."));
    return;
  }
  const QFileInfo executable(executablePath());
  if (!executable.isFile() || !executable.isExecutable()) {
    fail(tr("Setup required: choose the installed Cadmium worker.")); return;
  }
  QVector<VerifiedFile> files;
  if (!runtimeFiles(executable, files)) {
    fail(tr("The Cadmium runtime package is missing or incompatible. Install the complete supported runtime package."));
    return;
  }
  const QDir directory(modelsDirectory());
  for (const Model &model : Models) {
    QFileInfo file(directory.filePath(QString::fromLatin1(model.name)));
    if ((!model.optional || file.exists()) && (!file.isFile() || file.size() != model.size)) {
      fail(tr("Setup required: model files are missing or have an incorrect size.")); return;
    }
    if (file.exists()) files.append({file.absoluteFilePath(), model.size, QByteArray(model.hash), false});
  }
  const quint64 generation = ++m_generation;
  m_cancelVerification.store(false);
  setState(Verifying, tr("Verifying runtime and model files…"));
  m_verifier = QThread::create([this, files, generation] {
    bool valid = true;
    bool runtimeMismatch = false;
    for (const VerifiedFile &entry : files) {
      QFile file(entry.path);
      runtimeMismatch = entry.runtime;
      if (!file.open(QIODevice::ReadOnly) || file.size() != entry.size) { valid = false; break; }
      QCryptographicHash hash(QCryptographicHash::Sha256);
      while (!file.atEnd() && !m_cancelVerification.load()) {
        QByteArray block = file.read(1024 * 1024);
        if (block.isEmpty() && file.error() != QFile::NoError) { valid = false; break; }
        hash.addData(block);
      }
      if (m_cancelVerification.load()) return;
      if (!valid || hash.result().toHex() != entry.hash) { valid = false; break; }
    }
    QMetaObject::invokeMethod(this, [this, generation, valid, runtimeMismatch] {
      if (generation != m_generation || m_state != Verifying) return;
      if (!valid) fail(runtimeMismatch ?
        tr("Runtime verification failed. Reinstall the complete supported runtime package.") :
        tr("Model verification failed. Reinstall the verified model files."));
      else launch(generation);
    }, Qt::QueuedConnection);
  });
  QThread *thread = m_verifier;
  connect(thread, &QThread::finished, this, [this, thread] {
    if (m_verifier == thread) m_verifier = nullptr;
    thread->deleteLater();
  });
  thread->start();
}

void CadmiumRuntime::launch(quint64 generation) {
  if (generation != m_generation) return;
  QTcpServer reservation;
  if (!reservation.listen(QHostAddress::LocalHost, 0)) {
    fail(tr("A local Cadmium connection could not be reserved.")); return;
  }
  m_port = reservation.serverPort();
  const QDir models(modelsDirectory());
  QStringList arguments{QStringLiteral("--host"), QHostAddress(QHostAddress::LocalHost).toString(),
    QStringLiteral("--port"), QString::number(m_port),
    QStringLiteral("--ant-model"), models.filePath("ant_v2_fp32.onnx"),
    QStringLiteral("--gap-model"), models.filePath("gap_closer_fp32.onnx"),
    QStringLiteral("--ep"), QStringLiteral("auto"), QStringLiteral("--exit-on-stdin-close")};
  m_tiled = m_gapAccelerated = false;
#ifdef Q_OS_WIN
  m_tiled = QFileInfo::exists(models.filePath("ant_v2_fp32_tiledscatter.onnx"));
  m_gapAccelerated = QFileInfo::exists(models.filePath("gap_closer_fp16.onnx"));
  if (m_tiled) arguments << "--ant-model-tiled" << models.filePath("ant_v2_fp32_tiledscatter.onnx");
  if (m_gapAccelerated) arguments << "--gap-model-bucket" << models.filePath("gap_closer_fp16.onnx");
#endif
  m_output.clear(); m_listening = false;
  m_process->setProgram(executablePath());
  m_process->setArguments(arguments);
  m_process->setWorkingDirectory(QFileInfo(executablePath()).absolutePath());
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("RUST_LOG"), QStringLiteral("info,ort::logging=warn"));
  environment.remove(QStringLiteral("CADMIUM_SIDECAR_WATCHDOG_DEBUG"));
  m_process->setProcessEnvironment(environment);
  reservation.close();
  setState(Starting, tr("Starting local Cadmium worker…"));
  m_deadline.start(60000);
  m_healthTimer.start();
  m_process->start();
}

void CadmiumRuntime::stop() {
  stopWorker(true);
}

void CadmiumRuntime::stopWorker(bool notify) {
  ++m_generation;
  m_cancelVerification.store(true);
  m_healthTimer.stop(); m_deadline.stop(); m_idleTimer.stop();
  m_queue.clear(); m_queuedBytes = 0; m_activeOperation.clear();
  if (m_reply) {
    QNetworkReply *reply = m_reply; m_reply = nullptr;
    reply->abort(); reply->deleteLater();
  }
  setState(Stopping, tr("Unloading models…"));
  if (m_process->state() != QProcess::NotRunning) {
    m_process->closeWriteChannel();
    m_process->terminate();
    QPointer<QProcess> process(m_process);
    const quint64 generation = m_generation;
    QTimer::singleShot(1500, this, [this, process, generation] {
      if (process && generation == m_generation && process->state() != QProcess::NotRunning)
        process->kill();
    });
  }
  m_listening = false;
  setState(Stopped, startupMode() == Off ? tr("Off") : tr("Stopped"));
  if (notify) emit workerStopped();
}

void CadmiumRuntime::fail(const QString &message) {
  stopWorker(false); setState(Error, message); emit failed(message);
}

void CadmiumRuntime::checkHealth(quint64 generation) {
  if (generation != m_generation || m_reply || !m_listening || m_process->state() != QProcess::Running) return;
  send(QStringLiteral("health"), {}, generation, m_state == Starting ? 1 : 4);
}

void CadmiumRuntime::warmModels(quint64 generation) {
  setState(Warming, tr("Loading and checking models…"));
  const auto warm = warmupColorize();
  send(QStringLiteral("segment"), {{"line_image_uri", warm.value("target_line_image_uri")},
       {"gap_closer_strength", 1.0}, {"return_colorized", false}}, generation, 2);
}

void CadmiumRuntime::colorize(const QJsonObject &body) { enqueue("colorize", body); }
void CadmiumRuntime::segment(const QJsonObject &body) { enqueue("segment", body); }
void CadmiumRuntime::preprocess(const QJsonObject &body) { enqueue("preprocess", body); }
void CadmiumRuntime::enqueue(const QString &operation, const QJsonObject &body) {
  if (startupMode() == Off) { start(); return; }
  const qint64 size = QJsonDocument(body).toJson(QJsonDocument::Compact).size();
  if (body.isEmpty() || size > MaxBody || m_queuedBytes + size > MaxBody || m_queue.size() >= 32) {
    emit failed(tr("The Cadmium request queue is full or the request is too large.")); return;
  }
  m_idleTimer.stop();
  m_queue.enqueue({operation, body, size}); m_queuedBytes += size;
  if (m_state != Ready) start();
  else next();
}

void CadmiumRuntime::next() {
  if (m_state != Ready || m_reply) return;
  if (m_queue.isEmpty()) { m_activeOperation.clear(); armIdleTimer(); emit statusChanged(); return; }
  const Request request = m_queue.dequeue(); m_queuedBytes -= request.bytes;
  m_activeOperation = request.operation;
  send(request.operation, request.body, m_generation);
  emit statusChanged();
}

void CadmiumRuntime::armIdleTimer() {
  m_idleTimer.stop();
  if (m_state == Ready && !m_reply && m_queue.isEmpty() && idleUnloadMinutes() > 0)
    m_idleTimer.start(idleUnloadMinutes() * 60000);
}

void CadmiumRuntime::send(const QString &operation, const QJsonObject &body,
                          quint64 generation, int internalStage) {
  if (m_reply || generation != m_generation) return;
  QUrl endpoint;
  endpoint.setScheme(QStringLiteral("http"));
  endpoint.setHost(QHostAddress(QHostAddress::LocalHost).toString());
  endpoint.setPort(m_port);
  endpoint.setPath(QStringLiteral("/") + operation);
  QNetworkRequest request(endpoint);
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  QNetworkReply *reply = operation == "health" ? m_network->get(request) :
    m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
  m_reply = reply;
  m_deadline.start(internalStage == 1 ? 60000 : 600000);
  auto data = std::make_shared<QByteArray>();
  connect(reply, &QIODevice::readyRead, this, [this, reply, data, generation] {
    if (generation != m_generation) return;
    if (data->size() + reply->bytesAvailable() > MaxResponse) {
      fail(tr("The Cadmium response exceeded the allowed size.")); return;
    }
    data->append(reply->readAll());
  });
  connect(reply, &QNetworkReply::finished, this,
    [this, reply, data, operation, generation, internalStage] {
      reply->deleteLater();
      if (generation != m_generation || m_reply != reply) return;
      m_reply = nullptr; m_deadline.stop();
      if (data->size() + reply->bytesAvailable() > MaxResponse) {
        fail(tr("The Cadmium response exceeded the allowed size.")); return;
      }
      data->append(reply->readAll());
      const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      if (reply->error() != QNetworkReply::NoError || code != 200) {
        fail(tr("Cadmium could not process the request. Check the models and available memory.")); return;
      }
      QJsonParseError parseError;
      const QJsonDocument document = QJsonDocument::fromJson(*data, &parseError);
      if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        fail(tr("Cadmium returned an invalid response.")); return;
      }
      const QJsonObject result = document.object();
      if (internalStage == 1) {
        if (result.value("engine").toString() != QStringLiteral("sidecar-rust") ||
            result.value("status").toString() != QStringLiteral("ok") ||
            result.value("version").toString() != QStringLiteral("0.1.0")) {
          fail(tr("The installed Cadmium worker is incompatible.")); return;
        }
        m_healthTimer.stop(); warmModels(generation); return;
      }
      if (internalStage == 2) {
        if (!result.value("seg_map_uri").isString()) {
          fail(tr("The segmentation model readiness check failed.")); return;
        }
        send(QStringLiteral("colorize"), warmupColorize(), generation, 3); return;
      }
      if (internalStage == 3) {
        if (result.value("target_color_ids").toArray().size() != 2 ||
            result.value("target_colors_rgba").toArray().size() != 2) {
          fail(tr("The colorizer model readiness check failed.")); return;
        }
        checkHealth(generation); return;
      }
      if (internalStage == 4) {
        const auto acceleration = result.value("acceleration").toObject();
        const auto colorize = acceleration.value("colorize").toObject();
        const auto segment = acceleration.value("segment").toObject();
        const QString active = colorize.value("active").toString();
        const bool gpu = active == "dml";
        const bool gapGpu = segment.value("active").toString() == "dml";
        setState(Ready, gpu && gapGpu ? tr("Ready — GPU (DirectML)") :
          gpu ? tr("Ready — GPU colorizer, CPU gap closing") :
          gapGpu ? tr("Ready — CPU colorizer, GPU gap closing") : tr("Ready — CPU"));
        next(); return;
      }
      m_activeOperation.clear();
      if (operation == "colorize") emit resultReady(result);
      else if (operation == "segment") emit segmentReady(result);
      else emit preprocessReady(result);
      next();
    });
}
