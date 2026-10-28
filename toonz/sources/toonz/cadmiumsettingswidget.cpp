#include "cadmiumsettingswidget.h"
#include "cadmiumruntime.h"
#include "cadmiuminstaller.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

CadmiumSettingsWidget::CadmiumSettingsWidget(QWidget *parent, bool compact)
    : QWidget(parent)
    , m_startupMode(new QComboBox(this))
    , m_status(new QLabel(this)) {
  auto *runtime = CadmiumRuntime::instance();
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  m_startupMode->setObjectName("cadmiumStartupMode");
  m_startupMode->setAccessibleName(tr("Cadmium startup mode"));
  m_startupMode->addItem(tr("Off"), CadmiumRuntime::Off);
  m_startupMode->addItem(tr("Load when needed"), CadmiumRuntime::OnDemand);
  m_startupMode->addItem(tr("Preload at startup"), CadmiumRuntime::Preload);
  m_startupMode->setToolTip(tr(
      "Load when needed keeps startup light. Preloading uses memory earlier "
      "to reduce the wait before the first painting request."));
  auto *startupRow = new QHBoxLayout();
  auto *startupLabel = new QLabel(tr("Cadmium startup mode:"), this);
  startupLabel->setBuddy(m_startupMode);
  startupRow->addWidget(startupLabel);
  startupRow->addWidget(m_startupMode, 1);
  layout->addLayout(startupRow);
  m_status->setObjectName("cadmiumRuntimeStatus");
  m_status->setWordWrap(true);
  m_status->setTextFormat(Qt::PlainText);
  layout->addWidget(m_status);

  if (compact) {
    auto *setup = new QPushButton(tr("Cadmium Settings..."), this);
    setup->setObjectName("cadmiumOpenSettings");
    startupRow->addWidget(setup);
    connect(setup, &QPushButton::clicked, this, [this] {
      QDialog dialog(this);
      dialog.setWindowTitle(tr("Cadmium Settings"));
      auto *dialogLayout = new QVBoxLayout(&dialog);
      dialogLayout->addWidget(new CadmiumSettingsWidget(&dialog));
      auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
      connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
      dialogLayout->addWidget(buttons);
      dialog.resize(640, dialog.sizeHint().height());
      dialog.exec();
    });
  } else {
    auto *description = new QLabel(tr(
        "Cadmium paints from reference drawings using models on this computer. "
        "Choose the runtime and installed model folder below. "
        "The startup choice is remembered automatically."), this);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *form = new QFormLayout();
    auto addPath = [this, form](const QString &label, QLineEdit *&field,
                               bool directory) {
      field = new QLineEdit(this);
      field->setReadOnly(true);
      field->setMinimumWidth(200);
      auto *browse = new QPushButton(tr("Browse..."), this);
      auto *row = new QHBoxLayout();
      row->addWidget(field, 1);
      row->addWidget(browse);
      form->addRow(label, row);
      connect(browse, &QPushButton::clicked, this, [this, directory] {
        auto *runtime = CadmiumRuntime::instance();
        const QString path = directory
            ? QFileDialog::getExistingDirectory(
                  this, tr("Choose Cadmium Model Folder"),
                  runtime->modelsDirectory())
            : QFileDialog::getOpenFileName(
                  this, tr("Choose Cadmium Runtime"),
                  runtime->executablePath());
        if (path.isEmpty()) return;
        if (directory)
          runtime->setModelsDirectory(path);
        else
          runtime->setExecutablePath(path);
        refresh();
      });
    };
    addPath(tr("Runtime:"), m_executable, false);
    addPath(tr("Model folder:"), m_models, true);
    m_idleUnload = new QSpinBox(this);
    m_idleUnload->setObjectName("cadmiumIdleUnloadMinutes");
    m_idleUnload->setRange(0, 1440);
    m_idleUnload->setSpecialValueText(tr("Keep loaded"));
    m_idleUnload->setSuffix(tr(" minutes"));
    m_idleUnload->setToolTip(tr(
        "Unloading idle models frees memory. The next painting request "
        "will load them again."));
    form->addRow(tr("Unload after inactivity:"), m_idleUnload);
    connect(m_idleUnload, QOverload<int>::of(&QSpinBox::valueChanged), runtime,
            &CadmiumRuntime::setIdleUnloadMinutes);
    m_executable->setObjectName("cadmiumExecutablePath");
    m_models->setObjectName("cadmiumModelsDirectory");
    layout->addLayout(form);

    auto *actions = new QHBoxLayout();
    m_load = new QPushButton(tr("Load Models"), this);
    m_unload = new QPushButton(tr("Unload Models"), this);
    m_load->setObjectName("cadmiumLoadModels");
    m_unload->setObjectName("cadmiumUnloadModels");
    actions->addWidget(m_load);
    actions->addWidget(m_unload);
    actions->addStretch();
    layout->addLayout(actions);
    connect(m_load, &QPushButton::clicked, runtime, &CadmiumRuntime::start);
    connect(m_unload, &QPushButton::clicked, runtime, &CadmiumRuntime::stop);

    m_installer = new CadmiumInstaller(this);
    m_modelSet = new QComboBox(this);
    m_modelSet->addItem(tr("Universal models — about 1.9 GB"), false);
#ifdef _WIN32
    m_modelSet->addItem(tr("Universal and Windows acceleration models — about 3.5 GB"), true);
#endif
    m_modelSet->setObjectName("cadmiumModelSet");
    layout->addWidget(m_modelSet);
    auto *downloadRow = new QHBoxLayout();
    m_download = new QPushButton(tr("Download Verified Models..."), this);
    m_cancelDownload = new QPushButton(tr("Cancel Download"), this);
    m_download->setObjectName("cadmiumDownloadModels");
    m_cancelDownload->setObjectName("cadmiumCancelDownload");
    m_cancelDownload->setEnabled(false);
    downloadRow->addWidget(m_download);
    downloadRow->addWidget(m_cancelDownload);
    layout->addLayout(downloadRow);
    m_downloadProgress = new QProgressBar(this);
    m_downloadProgress->setRange(0, 1000);
    m_downloadProgress->setValue(0);
    m_downloadProgress->setVisible(false);
    layout->addWidget(m_downloadProgress);
    m_downloadStatus = new QLabel(this);
    m_downloadStatus->setTextFormat(Qt::PlainText);
    m_downloadStatus->setWordWrap(true);
    layout->addWidget(m_downloadStatus);
    connect(m_download, &QPushButton::clicked, this, [this, runtime] {
      QString directory = runtime->modelsDirectory();
      if (directory.isEmpty())
        directory = QDir(QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation)).filePath("cadmium/models");
      directory = QFileDialog::getExistingDirectory(
          this, tr("Choose Model Installation Folder"), directory);
      if (directory.isEmpty()) return;
      const bool accelerated = m_modelSet->currentData().toBool();
      if (QMessageBox::question(this, tr("Install Cadmium Models"),
          tr("Download approximately %1 GB of verified model files? "
             "Model processing stays on this computer. Closing these settings "
             "cancels the download. Previously verified files can be reused.")
              .arg(double(CadmiumInstaller::downloadSize(accelerated)) / 1e9, 0, 'f', 2))
          != QMessageBox::Yes) return;
      runtime->stop();
      m_installer->install(directory, accelerated);
      refresh();
    });
    connect(m_cancelDownload, &QPushButton::clicked,
            m_installer, &CadmiumInstaller::cancel);
    connect(m_installer, &CadmiumInstaller::progress, this,
            [this](qint64 completed, qint64 total, const QString &file) {
      m_downloadStatus->setText(tr("Verifying or downloading %1").arg(file));
      m_downloadProgress->setVisible(true);
      m_downloadProgress->setValue(total > 0 ? int(completed * 1000 / total) : 0);
      refresh();
    });
    connect(m_installer, &CadmiumInstaller::finished, this,
            [this, runtime](const QString &directory) {
      runtime->setModelsDirectory(directory);
      m_downloadProgress->setValue(1000);
      m_downloadStatus->setText(tr("Models installed and verified. Use Load Models when ready."));
      refresh();
    });
    connect(m_installer, &CadmiumInstaller::failed, this,
            [this](const QString &message) {
      m_downloadStatus->setText(message);
      refresh();
    });
    connect(m_installer, &CadmiumInstaller::cancelled, this, [this] {
      m_downloadStatus->setText(tr("Download cancelled. Previously installed files were preserved."));
      refresh();
    });

    auto *hardware = new QLabel(tr(
        "Recommended: 16 GB system memory and an SSD. "
        "Processing can use the CPU; compatible DirectX 12 graphics can "
        "accelerate it. Universal models use about 1.9 GB of storage, or "
        "about 3.5 GB including Windows acceleration models. Allow extra "
        "space for installation and working files. Memory requirements "
        "vary with drawing complexity."), this);
    hardware->setWordWrap(true);
    layout->addWidget(hardware);
    layout->addStretch();
  }

  connect(m_startupMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, [runtime](int index) {
    runtime->setStartupMode(index);
  });
  connect(runtime, &CadmiumRuntime::statusChanged, this,
          &CadmiumSettingsWidget::refresh);
  refresh();
}

void CadmiumSettingsWidget::refresh() {
  auto *runtime = CadmiumRuntime::instance();
  const QSignalBlocker blocker(m_startupMode);
  m_startupMode->setCurrentIndex(runtime->startupMode());
  m_status->setText(runtime->statusText());
  if (m_executable) m_executable->setText(runtime->executablePath());
  if (m_models) m_models->setText(runtime->modelsDirectory());
  if (m_idleUnload) {
    const QSignalBlocker idleBlocker(m_idleUnload);
    m_idleUnload->setValue(runtime->idleUnloadMinutes());
  }
  const bool installing = m_installer && m_installer->busy();
  m_startupMode->setEnabled(!installing);
  if (m_download) m_download->setEnabled(!installing && !runtime->busy());
  if (m_cancelDownload) m_cancelDownload->setEnabled(installing);
  if (m_modelSet) m_modelSet->setEnabled(!installing);
  if (m_load)
    m_load->setEnabled(runtime->startupMode() != CadmiumRuntime::Off &&
                       !installing && !runtime->busy() && !runtime->isReady());
  if (m_unload)
    m_unload->setEnabled(runtime->busy() || runtime->isReady());
}
