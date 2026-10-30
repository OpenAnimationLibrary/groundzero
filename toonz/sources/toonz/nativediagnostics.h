#pragma once

#include "nativediagnosticswriter.h"
#include <QObject>
#include <QStringList>
#include <memory>

class QSettings;
class QTimer;

class NativeDiagnostics final : public QObject {
  Q_OBJECT
public:
  static NativeDiagnostics *instance();
  explicit NativeDiagnostics(QObject *parent = nullptr);
  ~NativeDiagnostics() override;
  void initialize(const QString &settingsFile, const QString &logDirectory = QString());
  void shutdown();
  bool isEnabled() const { return m_enabled; }
  int minimumSeverity() const { return m_minimumSeverity; }
  int historyLimit() const { return m_historyLimit; }
  QString directory() const { return m_directory; }
  NativeDiagnostic::Status status() const;
  void setEnabled(bool enabled);
  void setMinimumSeverity(int severity);
  void setHistoryLimit(int limit);
  void deleteSavedLogs();

signals:
  void settingsChanged();
  void statusChanged();

private:
  void saveSettings();
  void updateQtHandler(bool install);
  std::unique_ptr<QSettings> m_settings;
  std::shared_ptr<NativeDiagnostic::Writer> m_writer;
  QTimer *m_statusTimer;
  QString m_directory;
  QStringList m_privatePaths;
  bool m_initialized = false;
  bool m_enabled = false;
  bool m_handlerInstalled = false;
  bool m_stopped = false;
  int m_minimumSeverity = 1;
  int m_historyLimit = 1000;
};
