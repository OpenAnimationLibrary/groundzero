#include "diagnosticssettingswidget.h"
#include "nativediagnostics.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QEvent>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

DiagnosticsSettingsWidget::DiagnosticsSettingsWidget(QWidget *parent,
                                                     NativeDiagnostics *service)
    : QWidget(parent), m_service(service ? service : NativeDiagnostics::instance()) {
  auto *layout = new QVBoxLayout(this);
  m_enabled = new QCheckBox(this);
  m_enabled->setObjectName("EnableDiagnosticLogging");
  layout->addWidget(m_enabled);
  auto *form = new QFormLayout();
  m_severityLabel = new QLabel(this);
  m_historyLabel = new QLabel(this);
  m_severity = new QComboBox(this);
  m_severity->setObjectName("DiagnosticSeverity");
  for (int index = 0; index < 4; ++index) m_severity->addItem(QString(), index);
  m_history = new QSpinBox(this);
  m_history->setObjectName("MessageHistoryLimit");
  m_history->setRange(100, 10000);
  m_history->setSingleStep(100);
  form->addRow(m_severityLabel, m_severity);
  form->addRow(m_historyLabel, m_history);
  layout->addLayout(form);
  m_retention = new QLabel(this);
  m_retention->setWordWrap(true);
  layout->addWidget(m_retention);
  m_status = new QLabel(this);
  m_status->setObjectName("DiagnosticStatus");
  m_status->setWordWrap(true);
  layout->addWidget(m_status);
  auto *buttons = new QHBoxLayout();
  m_open = new QPushButton(this);
  m_delete = new QPushButton(this);
  m_retry = new QPushButton(this);
  buttons->addWidget(m_open);
  buttons->addWidget(m_delete);
  buttons->addWidget(m_retry);
  layout->addLayout(buttons);
  layout->addStretch();
  connect(m_enabled, &QCheckBox::toggled, m_service, &NativeDiagnostics::setEnabled);
  connect(m_severity, QOverload<int>::of(&QComboBox::currentIndexChanged), m_service,
          &NativeDiagnostics::setMinimumSeverity);
  connect(m_history, QOverload<int>::of(&QSpinBox::valueChanged), m_service,
          &NativeDiagnostics::setHistoryLimit);
  connect(m_delete, &QPushButton::clicked, m_service, &NativeDiagnostics::deleteSavedLogs);
  connect(m_retry, &QPushButton::clicked, this, [this] { m_service->setEnabled(true); });
  connect(m_open, &QPushButton::clicked, this, [this] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_service->directory()));
  });
  connect(m_service, &NativeDiagnostics::settingsChanged, this,
          &DiagnosticsSettingsWidget::refreshSettings);
  connect(m_service, &NativeDiagnostics::statusChanged, this,
          &DiagnosticsSettingsWidget::refreshStatus);
  retranslate();
  refreshSettings();
}

void DiagnosticsSettingsWidget::refreshSettings() {
  const QSignalBlocker enabled(m_enabled), severity(m_severity), history(m_history);
  m_enabled->setChecked(m_service->isEnabled());
  m_severity->setCurrentIndex(m_service->minimumSeverity());
  m_history->setValue(m_service->historyLimit());
  m_severity->setEnabled(m_service->isEnabled());
  refreshStatus();
}

void DiagnosticsSettingsWidget::refreshStatus() {
  const auto status = m_service->status();
  QString text;
  using NativeDiagnostic::State;
  switch (status.state) {
  case State::Off: text = tr("File logging is off."); break;
  case State::Starting: text = tr("Opening diagnostic logs..."); break;
  case State::Ready: text = tr("Saving diagnostic logs on this computer."); break;
  case State::Locked: text = tr("Another application instance is using the diagnostic logs. Retry after it releases them."); break;
  case State::Error: text = tr("Diagnostic logs are unavailable. Check free space and folder permissions, then retry."); break;
  case State::Stopped: text = tr("Diagnostic logging has stopped."); break;
  }
  text += tr("\nWritten: %1. Dropped: %2. Queued: %3.")
              .arg(status.written).arg(status.dropped).arg(qulonglong(status.queued));
  m_status->setText(text);
  m_retry->setEnabled(m_service->isEnabled() &&
      (status.state == State::Locked || status.state == State::Error));
  m_open->setEnabled(!m_service->directory().isEmpty());
  m_delete->setEnabled(!m_service->directory().isEmpty() &&
                      status.state != State::Starting);
}

void DiagnosticsSettingsWidget::retranslate() {
  const QSignalBlocker severity(m_severity);
  m_enabled->setText(tr("Save local diagnostic logs"));
  m_severityLabel->setText(tr("Minimum log severity:"));
  m_historyLabel->setText(tr("Messages kept in Message Center:"));
  m_severity->setItemText(0, tr("Debug"));
  m_severity->setItemText(1, tr("Info"));
  m_severity->setItemText(2, tr("Warning"));
  m_severity->setItemText(3, tr("Error"));
  m_retention->setText(tr("Logs stay on this computer: one 5 MiB file and three backups. "
      "Diagnostic text can contain filenames. Clearing Message Center does not delete saved logs."));
  m_open->setText(tr("Open Log Folder"));
  m_delete->setText(tr("Delete Saved Logs"));
  m_retry->setText(tr("Retry"));
  refreshStatus();
}

void DiagnosticsSettingsWidget::changeEvent(QEvent *event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) retranslate();
}
