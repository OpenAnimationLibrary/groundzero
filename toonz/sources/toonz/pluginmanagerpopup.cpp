#include "pluginmanagerpopup.h"
#include "toonzqt/pluginmanager.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {
QString pathKey(QString path) {
#ifdef Q_OS_WIN
  path = path.toCaseFolded();
#endif
  return path;
}

void openFolder(const QString &path, QWidget *parent) {
  if (!QDir().mkpath(path) ||
      !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
    QMessageBox::warning(parent, QObject::tr("Plugin Manager"),
                         QObject::tr("Could not open the plugin folder."));
}
}  // namespace

PluginManagerPopup::PluginManagerPopup(PluginManager *manager, QWidget *parent)
    : QDialog(parent), m_manager(manager) {
  setWindowTitle(tr("Plugin Manager"));
  setObjectName("PluginManager");
  resize(760, 460);
  auto *layout = new QVBoxLayout(this);
  m_bypass     = new QCheckBox(tr("Bypass all plugins"), this);
  m_bypass->setObjectName("BypassAllPlugins");
  layout->addWidget(m_bypass);
  auto *explanation = new QLabel(
      tr("Manage third-party FX libraries. Changes take effect after "
         "restarting "
         "Groundzero. Files stay in place.\n"
         "Bypass preserves individual selections. Disabled effects will be "
         "unavailable to scenes and rendering after restart."),
      this);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  m_status->setObjectName("PluginManagerStatus");
  layout->addWidget(m_status);
  m_plugins = new QTreeWidget(this);
  m_plugins->setObjectName("PluginLibraries");
  m_plugins->setHeaderLabels(
      {tr("Enable at startup"), tr("Current session"), tr("Next startup")});
  m_plugins->setRootIsDecorated(false);
  m_plugins->setUniformRowHeights(true);
  m_plugins->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_plugins->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_plugins->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  layout->addWidget(m_plugins, 1);

  auto *buttons       = new QHBoxLayout;
  auto *installFile   = new QPushButton(tr("Install File..."), this);
  auto *installFolder = new QPushButton(tr("Install Folder..."), this);
  auto *open          = new QPushButton(tr("Open Plugin Folder"), this);
  auto *openConfig    = new QPushButton(tr("Open Configuration Folder"), this);
  auto *reload        = new QPushButton(tr("Refresh"), this);
  buttons->addWidget(installFile);
  buttons->addWidget(installFolder);
  buttons->addWidget(open);
  buttons->addWidget(openConfig);
  buttons->addWidget(reload);
  layout->addLayout(buttons);
  auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
  layout->addWidget(close);
  connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(reload, &QPushButton::clicked, this, &PluginManagerPopup::refresh);
  connect(open, &QPushButton::clicked, this,
          [this] { openFolder(m_manager->directory(), this); });
  connect(openConfig, &QPushButton::clicked, this, [this] {
    openFolder(QFileInfo(m_manager->configurationPath()).absolutePath(), this);
  });
  connect(installFile, &QPushButton::clicked, this, [this] { install(false); });
  connect(installFolder, &QPushButton::clicked, this,
          [this] { install(true); });
  connect(m_bypass, &QCheckBox::toggled, this, [this](bool checked) {
    QString error;
    if (!m_manager->setBypassAll(checked, &error)) {
      QMessageBox::warning(this, tr("Plugin Manager"), error);
      refresh();
    }
  });
  connect(m_plugins, &QTreeWidget::itemChanged, this,
          [this](QTreeWidgetItem *item, int column) {
            if (column != 0) return;
            QString error;
            if (!m_manager->setEnabled(item->data(0, Qt::UserRole).toString(),
                                       item->checkState(0) == Qt::Checked,
                                       &error)) {
              QMessageBox::warning(this, tr("Plugin Manager"), error);
              refresh();
            }
          });
  connect(manager, &PluginManager::settingsChanged, this,
          &PluginManagerPopup::refresh, Qt::QueuedConnection);
  connect(manager, &PluginManager::statusChanged, this,
          &PluginManagerPopup::refresh, Qt::QueuedConnection);
  refresh();
}

void PluginManagerPopup::showEvent(QShowEvent *event) {
  QDialog::showEvent(event);
  refresh();
}

void PluginManagerPopup::refresh() {
  const QSignalBlocker bypassBlocker(m_bypass), treeBlocker(m_plugins);
  const auto settings = m_manager->settings();
  const auto session  = m_manager->session();
  const QString selected =
      m_plugins->currentItem()
          ? m_plugins->currentItem()->data(0, Qt::UserRole).toString()
          : QString();
  m_bypass->setChecked(settings.bypassAll);
  m_bypass->setEnabled(settings.error.isEmpty());
  QString status;
  if (!settings.error.isEmpty())
    status = settings.error;
  else if (settings.bypassAll)
    status = tr("All plugins will be bypassed at next startup.");
  if (session.launchBypass)
    status += tr("\nAll plugins were bypassed for this launch (--no-plugins).");
  if (!session.settings.error.isEmpty())
    status += tr("\nPlugins were bypassed at startup: %1")
                  .arg(session.settings.error);
  status += tr("\nConfiguration: %1").arg(m_manager->configurationPath());
  m_status->setText(status.trimmed());
  m_plugins->clear();
  m_plugins->setEnabled(settings.error.isEmpty());
  for (const QString &path : m_manager->installedFiles()) {
    const QString key  = pathKey(path);
    const bool enabled = !settings.blocked.contains(key);
    auto *item         = new QTreeWidgetItem(m_plugins);
    item->setText(0, path);
    item->setData(0, Qt::UserRole, path);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(0, enabled ? Qt::Checked : Qt::Unchecked);
    const auto result = session.results.constFind(key);
    QString current;
    if (!session.started)
      current = tr("Not loaded");
    else if (session.launchBypass || session.settings.bypassAll ||
             !session.settings.error.isEmpty())
      current = tr("Bypassed");
    else if (session.settings.blocked.contains(key))
      current = tr("Disabled");
    else if (result == session.results.cend())
      current = tr("Not loaded");
    else if (!result->loaded)
      current = tr("Failed");
    else if (!result->error.isEmpty())
      current = tr("Partially loaded");
    else
      current = tr("Loaded");
    item->setText(1, current);
    if (result != session.results.cend()) item->setToolTip(1, result->error);
    const bool nextEnabled = enabled && !settings.bypassAll;
    const bool loaded      = result != session.results.cend() && result->loaded;
    item->setText(2, nextEnabled ? tr("Enabled") : tr("Skipped"));
    if (nextEnabled != loaded)
      item->setText(2, item->text(2) + tr(" (restart required)"));
    if (path == selected) m_plugins->setCurrentItem(item);
  }
}

void PluginManagerPopup::install(bool folder) {
  const QString source =
      folder
          ? QFileDialog::getExistingDirectory(this, tr("Install Plugin Folder"))
          : QFileDialog::getOpenFileName(this, tr("Install Plugin File"),
                                         QString(),
                                         tr("OpenToonz FX plugins (*.plugin)"));
  if (source.isEmpty()) return;
  QString error;
  if (!m_manager->install(source, &error))
    QMessageBox::warning(this, tr("Plugin Manager"), error);
  refresh();
}

PluginSettingsWidget::PluginSettingsWidget(PluginManager *manager,
                                           QWidget *parent)
    : QWidget(parent), m_manager(manager) {
  auto *layout = new QVBoxLayout(this);
  auto *title  = new QLabel(tr("Third-party FX Plugins"), this);
  layout->addWidget(title);
  m_bypass = new QCheckBox(tr("Bypass all plugins"), this);
  m_bypass->setObjectName("BypassAllPlugins");
  m_bypass->setToolTip(
      tr("Skip loading all third-party FX plugins at startup. "
         "Requires restart. Individual selections are preserved."));
  layout->addWidget(m_bypass);
  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  layout->addWidget(m_status);
  auto *open = new QPushButton(tr("Plugin Manager..."), this);
  open->setObjectName("OpenPluginManager");
  layout->addWidget(open, 0, Qt::AlignLeft);
  connect(open, &QPushButton::clicked, this, [this] {
    PluginManagerPopup dialog(m_manager, this);
    dialog.exec();
    refresh();
  });
  connect(m_bypass, &QCheckBox::toggled, this, [this](bool checked) {
    QString error;
    if (!m_manager->setBypassAll(checked, &error)) {
      QMessageBox::warning(this, tr("Plugin Manager"), error);
      refresh();
    }
  });
  connect(manager, &PluginManager::settingsChanged, this,
          &PluginSettingsWidget::refresh);
  refresh();
}

void PluginSettingsWidget::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  refresh();
}

void PluginSettingsWidget::refresh() {
  const QSignalBlocker blocker(m_bypass);
  const auto settings = m_manager->settings();
  m_bypass->setChecked(settings.bypassAll);
  m_bypass->setEnabled(settings.error.isEmpty());
  m_status->setText(
      settings.error.isEmpty()
          ? tr("Requires restart. Use the Plugin Manager to enable or disable "
               "individual libraries without moving their files.")
          : settings.error);
}
