#include "cadmiuminstaller.h"

#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStorageInfo>
#include <QSslSocket>
#include <QProcess>
#include <QTimer>
#include <QUrl>

CadmiumInstaller::CadmiumInstaller(QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)) {}

CadmiumInstaller::~CadmiumInstaller() { clearTransfer(); }

QVector<CadmiumInstaller::Model> CadmiumInstaller::models(bool accelerated) {
  QVector<Model> result = {
      {QStringLiteral("ant_v2_fp32.onnx"), 1388610539,
       QByteArrayLiteral("6c8d95cbfa62408f3cbfbbc29204fac7df5a587466dbb751e7e1b0bb530ff12b")},
      {QStringLiteral("gap_closer_fp32.onnx"), 497500380,
       QByteArrayLiteral("3a9a792f72fd3451c36145a0657328e333d6e5e982f0760b5e5a6e7e9934d327")}};
  if (accelerated) {
    result.append({QStringLiteral("ant_v2_fp32_tiledscatter.onnx"), 1389055751,
       QByteArrayLiteral("64a89282e0f0a18e182257718341dfc0838481ec63d1bb5fb45bad82b25814d8")});
    result.append({QStringLiteral("gap_closer_fp16.onnx"), 248798392,
       QByteArrayLiteral("1976b93409591384b2177fab1a5b84ee9866ad1c2cfabdc1154e7835fead33d6")});
  }
  return result;
}

qint64 CadmiumInstaller::downloadSize(bool accelerated) {
  qint64 bytes = 0;
  for (const Model &model : models(accelerated)) bytes += model.bytes;
  return bytes;
}

void CadmiumInstaller::install(const QString &directory, bool accelerated) {
  if (m_busy) return;
  if (directory.isEmpty() || !QDir().mkpath(directory)) {
    emit failed(tr("Choose a writable model folder."));
    return;
  }
  m_lock.reset(new QLockFile(QDir(directory).filePath(QStringLiteral(".cadmium-install.lock"))));
  if (!m_lock->tryLock(0)) {
    m_lock.reset();
    emit failed(tr("Another model installation is already using this folder."));
    return;
  }
  m_models = models(accelerated);
  m_directory = QDir(directory).absolutePath();
  m_total = downloadSize(accelerated);
  qint64 needed = 1024LL * 1024 * 1024;
  for (const Model &model : m_models) {
    const QFileInfo file(QDir(m_directory).filePath(model.name));
    if (!file.exists() || file.size() != model.bytes) needed += model.bytes;
  }
  const QStorageInfo storage(m_directory);
  if (!storage.isValid() || storage.isReadOnly() ||
      storage.bytesAvailable() < needed) {
    m_lock.reset();
    emit failed(tr("The model folder needs more free space or is not writable."));
    return;
  }
  m_busy = true;
  ++m_generation;
  m_index = 0;
  m_completed = 0;
  next();
}

void CadmiumInstaller::next() {
  if (!m_busy) return;
  clearTransfer();
  ++m_generation;
  if (m_index >= m_models.size()) {
    m_busy = false;
    m_lock.reset();
    emit finished(m_directory);
    return;
  }
  const Model &model = m_models[m_index];
  emit progress(m_completed, m_total, model.name);
  const QString path = QDir(m_directory).filePath(model.name);
  m_existing.reset(new QFile(path));
  if (m_existing->size() == model.bytes && m_existing->open(QIODevice::ReadOnly)) {
    m_hash.reset(new QCryptographicHash(QCryptographicHash::Sha256));
    verifyExisting();
  } else {
    m_existing.reset();
    download();
  }
}

void CadmiumInstaller::verifyExisting() {
  if (!m_busy || !m_existing) return;
  const QByteArray chunk = m_existing->read(4 * 1024 * 1024);
  if (!chunk.isEmpty()) {
    m_hash->addData(chunk);
    const quint64 generation = m_generation;
    QTimer::singleShot(0, this, [this, generation] {
      if (generation == m_generation) verifyExisting();
    });
    return;
  }
  const bool verified = m_existing->error() == QFile::NoError &&
      m_hash->result().toHex() == m_models[m_index].digest;
  m_existing.reset();
  if (verified) {
    m_completed += m_models[m_index].bytes;
    ++m_index;
    next();
  } else {
    download();
  }
}

void CadmiumInstaller::download() {
  if (!m_busy) return;
  const Model &model = m_models[m_index];
  // Reserve enough space for an atomic replacement, including corrupt files.
  if (QStorageInfo(m_directory).bytesAvailable() < model.bytes + 1024LL * 1024 * 1024) {
    fail(tr("There is insufficient free space to safely install this model."));
    return;
  }
  m_output.reset(new QSaveFile(QDir(m_directory).filePath(model.name)));
  m_output->setDirectWriteFallback(false);
  if (!m_output->open(QIODevice::WriteOnly)) {
    fail(tr("The model file cannot be created. Check the folder permissions."));
    return;
  }
  m_hash.reset(new QCryptographicHash(QCryptographicHash::Sha256));
  m_received = 0;
  QUrl url;
  url.setScheme(QStringLiteral("https"));
  url.setHost(QStringLiteral("github.com"));
  url.setPath(QStringLiteral("/latentspacelabs/cadmium-oss/releases/download/models-v2/") + model.name);
#ifdef Q_OS_WIN
  // The system HTTPS client uses the Windows trust store and needs no
  // additional OpenSSL libraries when the Qt distribution has no TLS runtime.
  if (!QSslSocket::supportsSsl()) {
    const QString systemRoot = qEnvironmentVariable("SystemRoot");
    const QString client = QDir(systemRoot).filePath("System32/curl.exe");
    if (systemRoot.isEmpty() || !QFileInfo::exists(client)) {
      fail(tr("Secure downloading is unavailable. Choose a verified offline model folder."));
      return;
    }
    m_nativeSucceeded = false;
    m_nativeDownload = new QProcess(this);
    m_nativeDownload->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *arguments) {
      arguments->flags |= 0x08000000UL;  // CREATE_NO_WINDOW
    });
    connect(m_nativeDownload, &QProcess::readyReadStandardOutput,
            this, &CadmiumInstaller::readDownload);
    connect(m_nativeDownload, &QProcess::readyReadStandardError, this,
            [this] { if (m_nativeDownload) m_nativeDownload->readAllStandardError(); });
    connect(m_nativeDownload, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) {
      m_nativeSucceeded = code == 0 && status == QProcess::NormalExit;
      completeDownload();
    });
    connect(m_nativeDownload, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError) {
      fail(tr("The system download client could not complete the secure download."));
    });
    m_nativeDownload->setProgram(client);
    m_nativeDownload->setArguments({QStringLiteral("--fail"), QStringLiteral("--location"),
        QStringLiteral("--proto"), QStringLiteral("=https"),
        QStringLiteral("--proto-redir"), QStringLiteral("=https"),
        QStringLiteral("--silent"), QStringLiteral("--show-error"),
        QStringLiteral("--connect-timeout"), QStringLiteral("60"),
        QStringLiteral("--max-time"), QStringLiteral("1800"),
        QStringLiteral("--url"), url.toString(QUrl::FullyEncoded)});
    m_nativeDownload->start(QIODevice::ReadOnly);
    return;
  }
#endif
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  m_reply = m_network->get(request);
  m_reply->setReadBufferSize(4 * 1024 * 1024);
  connect(m_reply, &QNetworkReply::readyRead, this, &CadmiumInstaller::readDownload);
  connect(m_reply, &QNetworkReply::finished, this, &CadmiumInstaller::completeDownload);
  const quint64 generation = m_generation;
  QTimer::singleShot(30 * 60 * 1000, this, [this, generation] {
    if (generation == m_generation && m_busy && m_reply)
      fail(tr("The model download timed out. You can retry or choose an offline model folder."));
  });
}

void CadmiumInstaller::readDownload() {
  if (!m_busy || (!m_reply && !m_nativeDownload) || !m_output) return;
  while (m_nativeDownload ? m_nativeDownload->bytesAvailable() > 0 : m_reply->bytesAvailable() > 0) {
    const QByteArray chunk = m_nativeDownload ? m_nativeDownload->read(1024 * 1024) : m_reply->read(1024 * 1024);
    if (chunk.isEmpty()) break;
    m_received += chunk.size();
    if (m_received > m_models[m_index].bytes ||
        m_output->write(chunk) != chunk.size()) {
      fail(tr("The model download could not be saved safely. Check free disk space."));
      return;
    }
    m_hash->addData(chunk);
    emit progress(m_completed + m_received, m_total, m_models[m_index].name);
  }
}

void CadmiumInstaller::completeDownload() {
  if (!m_busy || (!m_reply && !m_nativeDownload)) return;
  readDownload();
  if (!m_busy || (!m_reply && !m_nativeDownload)) return;
  const int status = m_reply ? m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 200;
  if ((m_reply && m_reply->error() != QNetworkReply::NoError) ||
      (m_nativeDownload && !m_nativeSucceeded) || status != 200) {
    fail(tr("The model download failed. Check connectivity and retry, or use offline files."));
    return;
  }
  const Model &model = m_models[m_index];
  if (m_received != model.bytes || m_hash->result().toHex() != model.digest) {
    fail(tr("The downloaded model failed verification. Existing installed files were preserved."));
    return;
  }
  if (!m_output->commit()) {
    fail(tr("The verified model could not be installed. Check disk space and permissions."));
    return;
  }
  m_output.reset();
  m_completed += model.bytes;
  ++m_index;
  next();
}

void CadmiumInstaller::clearTransfer() {
  if (m_nativeDownload) {
    disconnect(m_nativeDownload, nullptr, this, nullptr);
    m_nativeDownload->kill();
    m_nativeDownload->deleteLater();
    m_nativeDownload = nullptr;
  }
  if (m_reply) {
    disconnect(m_reply, nullptr, this, nullptr);
    m_reply->abort();
    m_reply->deleteLater();
    m_reply = nullptr;
  }
  if (m_output) m_output->cancelWriting();
  m_output.reset();
  m_existing.reset();
  m_hash.reset();
}

void CadmiumInstaller::fail(const QString &message) {
  ++m_generation;
  clearTransfer();
  m_busy = false;
  m_lock.reset();
  emit failed(message);
}

void CadmiumInstaller::cancel() {
  if (!m_busy) return;
  ++m_generation;
  clearTransfer();
  m_busy = false;
  m_lock.reset();
  emit cancelled();
}
