#pragma once

#include <QWidget>

class NativeDiagnostics;
class QCheckBox;
class QComboBox;
class QSpinBox;
class QLabel;
class QPushButton;

class DiagnosticsSettingsWidget final : public QWidget {
  Q_OBJECT
public:
  explicit DiagnosticsSettingsWidget(QWidget *parent = nullptr,
                                     NativeDiagnostics *service = nullptr);
protected:
  void changeEvent(QEvent *) override;
private:
  void retranslate();
  void refreshSettings();
  void refreshStatus();
  NativeDiagnostics *m_service;
  QCheckBox *m_enabled;
  QComboBox *m_severity;
  QSpinBox *m_history;
  QLabel *m_severityLabel, *m_historyLabel, *m_retention, *m_status;
  QPushButton *m_open, *m_delete, *m_retry;
};
