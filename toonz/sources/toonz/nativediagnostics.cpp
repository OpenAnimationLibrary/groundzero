#include "nativediagnostics.h"
#include "toonzqt/tmessageviewer.h"
#include "tlogger.h"

#include <QCoreApplication>
#include <QDir>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <cstdio>

namespace {
QMutex handlerMutex;
std::weak_ptr<NativeDiagnostic::Writer> handlerWriter;
QtMessageHandler previousHandler = nullptr;
QStringList handlerPrivatePaths;
thread_local bool inDiagnosticHandler = false;

QString redactPaths(QString text, const QStringList &paths) {
  // Preserve useful diagnostic text while avoiding the user's home prefix.
  // Arbitrary message contents are not treated as safe for public submission.
  for (const QString &entry : paths) {
    const QString path = QDir::cleanPath(QDir::fromNativeSeparators(entry));
    // A root home directory (a service account or a container) is no private
    // prefix: replacing it would replace every separator of every path.
    if (entry.isEmpty() || QDir(path).isRoot()) continue;
    text.replace(path, "[local]", Qt::CaseInsensitive);
    text.replace(QDir::toNativeSeparators(path), "[local]", Qt::CaseInsensitive);
  }
  return text;
}

NativeDiagnostic::Severity qtSeverity(QtMsgType type) {
  switch (type) {
  case QtDebugMsg: return NativeDiagnostic::Severity::Debug;
  case QtInfoMsg: return NativeDiagnostic::Severity::Info;
  case QtWarningMsg: return NativeDiagnostic::Severity::Warning;
  case QtCriticalMsg:
  case QtFatalMsg: return NativeDiagnostic::Severity::Error;
  }
  return NativeDiagnostic::Severity::Info;
}

void diagnosticMessageHandler(QtMsgType type, const QMessageLogContext &context,
                              const QString &message) {
  if (inDiagnosticHandler) {
    // No logging calls here: a previous handler or a failing sink may itself
    // issue a Qt diagnostic. Preserve stderr without recursing into adapters.
    const QByteArray bytes = message.toLocal8Bit();
    std::fprintf(stderr, "%s\n", bytes.constData());
    return;
  }
  inDiagnosticHandler = true;
  std::shared_ptr<NativeDiagnostic::Writer> writer;
  QtMessageHandler previous;
  QStringList paths;
  {
    QMutexLocker guard(&handlerMutex);
    writer = handlerWriter.lock();
    previous = previousHandler;
    paths = handlerPrivatePaths;
  }
  try {
    if (writer) {
      NativeDiagnostic::Record record;
      record.severity = qtSeverity(type);
      record.source = QStringLiteral("Qt");
      record.text = redactPaths(message, paths);
      writer->append(record);
    }
  } catch (...) {
    // A diagnostic must never throw into the caller or affect normal startup.
  }
  if (previous && previous != diagnosticMessageHandler) {
    previous(type, context, message);
  } else {
    const QByteArray bytes = qFormatLogMessage(type, context, message).toLocal8Bit();
    std::fprintf(stderr, "%s\n", bytes.constData());
  }
  inDiagnosticHandler = false;
}
}  // namespace

NativeDiagnostics *NativeDiagnostics::instance() {
  static NativeDiagnostics *service = new NativeDiagnostics(qApp);
  return service;
}

NativeDiagnostics::NativeDiagnostics(QObject *parent)
    : QObject(parent), m_writer(std::make_shared<NativeDiagnostic::Writer>()),
      m_statusTimer(new QTimer(this)) {
  m_statusTimer->setInterval(500);
  connect(m_statusTimer, &QTimer::timeout, this, &NativeDiagnostics::statusChanged);
  if (qApp) connect(qApp, &QCoreApplication::aboutToQuit, this,
                    &NativeDiagnostics::shutdown);
}

NativeDiagnostics::~NativeDiagnostics() { shutdown(); }

void NativeDiagnostics::initialize(const QString &settingsFile,
                                   const QString &logDirectory) {
  if (m_initialized || m_stopped) return;
  m_initialized = true;
  m_settings.reset(new QSettings(settingsFile, QSettings::IniFormat));
  const QString localData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  m_directory = logDirectory.isEmpty()
      ? (localData.isEmpty() ? QString() : localData + "/diagnostics")
      : logDirectory;
  m_privatePaths = QStringList{QDir::homePath()};
  m_historyLimit = std::max(100, std::min(10000,
      m_settings->value("Diagnostics/HistoryLimit", 1000).toInt()));
  m_minimumSeverity = std::max(0, std::min(3,
      m_settings->value("Diagnostics/MinimumSeverity", 1).toInt()));
  m_writer->setMinimumSeverity(NativeDiagnostic::Severity(m_minimumSeverity));
  auto *repository = TMessageRepository::instance();
  repository->setHistoryLimit(m_historyLimit);
  connect(repository, &TMessageRepository::recordAdded, this,
          [this](quint64, const QDateTime &timestamp, int severity, const QString &text) {
    if (!m_enabled || m_stopped) return;
    NativeDiagnostic::Record record;
    record.timestamp = timestamp;
    record.severity = severity == DVGui::CRITICAL ? NativeDiagnostic::Severity::Error
        : severity == DVGui::WARNING ? NativeDiagnostic::Severity::Warning
                                    : NativeDiagnostic::Severity::Info;
    record.source = QStringLiteral("Application");
    record.text = redactPaths(text, m_privatePaths);
    m_writer->append(record);
  });
  const std::weak_ptr<NativeDiagnostic::Writer> writer = m_writer;
  const QStringList privatePaths = m_privatePaths;
  TLogger::instance()->setDiagnosticSink([writer, privatePaths](const TLogger::Message &message) {
    if (const auto target = writer.lock()) {
      NativeDiagnostic::Record record;
      record.severity = NativeDiagnostic::Severity(int(message.getType()) - 1);
      record.source = QStringLiteral("Native");
      record.text = redactPaths(QString::fromStdString(message.getText()), privatePaths);
      target->append(record);
    }
  });
  setEnabled(m_settings->value("Diagnostics/Enabled", false).toBool());
  m_statusTimer->start();
}

void NativeDiagnostics::saveSettings() {
  if (!m_settings) return;
  m_settings->setValue("Diagnostics/Enabled", m_enabled);
  m_settings->setValue("Diagnostics/MinimumSeverity", m_minimumSeverity);
  m_settings->setValue("Diagnostics/HistoryLimit", m_historyLimit);
  m_settings->sync();
}

void NativeDiagnostics::setEnabled(bool enabled) {
  if (!m_initialized || m_stopped) return;
  m_enabled = enabled;
  updateQtHandler(enabled);
  if (enabled) m_writer->enable(m_directory);
  else m_writer->disable();
  saveSettings();
  emit settingsChanged();
  emit statusChanged();
}

void NativeDiagnostics::setMinimumSeverity(int severity) {
  m_minimumSeverity = std::max(0, std::min(3, severity));
  m_writer->setMinimumSeverity(NativeDiagnostic::Severity(m_minimumSeverity));
  saveSettings();
  emit settingsChanged();
}

void NativeDiagnostics::setHistoryLimit(int limit) {
  m_historyLimit = std::max(100, std::min(10000, limit));
  TMessageRepository::instance()->setHistoryLimit(m_historyLimit);
  saveSettings();
  emit settingsChanged();
}

void NativeDiagnostics::deleteSavedLogs() {
  if (!m_stopped && m_initialized) m_writer->removeSavedLogs(m_directory);
}

NativeDiagnostic::Status NativeDiagnostics::status() const { return m_writer->status(); }

void NativeDiagnostics::updateQtHandler(bool install) {
  if (install == m_handlerInstalled) return;
  QMutexLocker guard(&handlerMutex);
  if (install) {
    handlerWriter = m_writer;
    handlerPrivatePaths = m_privatePaths;
    previousHandler = qInstallMessageHandler(diagnosticMessageHandler);
  } else {
    // Do not overwrite a handler installed after ours by another subsystem.
    QtMessageHandler current = qInstallMessageHandler(previousHandler);
    if (current != diagnosticMessageHandler) qInstallMessageHandler(current);
    handlerWriter.reset();
    handlerPrivatePaths.clear();
  }
  m_handlerInstalled = install;
}

void NativeDiagnostics::shutdown() {
  if (m_stopped) return;
  m_stopped = true;
  m_statusTimer->stop();
  updateQtHandler(false);
  if (m_initialized) TLogger::instance()->setDiagnosticSink({});
  m_writer->shutdown();
}
