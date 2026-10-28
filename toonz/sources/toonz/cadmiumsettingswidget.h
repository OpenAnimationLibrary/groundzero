#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QProgressBar;
class CadmiumInstaller;

class CadmiumSettingsWidget final : public QWidget {
  Q_OBJECT

public:
  explicit CadmiumSettingsWidget(QWidget *parent = nullptr,
                                 bool compact = false);

private:
  void refresh();
  QComboBox *m_startupMode;
  QLabel *m_status;
  QLineEdit *m_executable = nullptr;
  QLineEdit *m_models = nullptr;
  QPushButton *m_load = nullptr;
  QPushButton *m_unload = nullptr;
  QSpinBox *m_idleUnload = nullptr;
  CadmiumInstaller *m_installer = nullptr;
  QComboBox *m_modelSet = nullptr;
  QPushButton *m_download = nullptr;
  QPushButton *m_cancelDownload = nullptr;
  QLabel *m_downloadStatus = nullptr;
  QProgressBar *m_downloadProgress = nullptr;
};
