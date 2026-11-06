#include "externalapplications.h"

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <utility>

namespace {
bool startIndependentApplication(const ExternalApplications::Launch &launch,
                                 QString *error) {
  QProcess process;
  process.setProgram(launch.target.executable);
  process.setArguments(launch.arguments);
  process.setWorkingDirectory(launch.target.workingDirectory);
  process.setProcessEnvironment(launch.environment);
  const bool started = process.startDetached();
  if (!started && error) *error = process.errorString();
  return started;
}
}  // namespace

ExternalApplications::ExternalApplications(QString applicationDirectory,
                                           QString selfExecutable,
                                           Settings settings, Launcher launcher,
                                           QObject *parent)
    : QObject(parent)
    , m_applicationDirectory(std::move(applicationDirectory))
    , m_selfExecutable(QFileInfo(selfExecutable).canonicalFilePath())
    , m_settings(std::move(settings))
    , m_launcher(launcher ? std::move(launcher) : startIndependentApplication) {
}

const char *ExternalApplications::command(Application application) {
  return application == Ztoryc ? "MI_LaunchZtoryc" : "MI_LaunchOT3D";
}

QString ExternalApplications::name(Application application) {
  return application == Ztoryc ? QStringLiteral("Ztoryc")
                               : QStringLiteral("OT3D");
}

QString ExternalApplications::configuredPath(Application application) const {
  return m_settings.path(application);
}

void ExternalApplications::setConfiguredPath(Application application,
                                             const QString &path) {
  const QString value = path.trimmed();
  if (value != configuredPath(application))
    m_settings.savePath(application, value);
  refresh();
}

ExternalApplications::Target ExternalApplications::target(
    Application application) const {
  Target result;
  const QString configured = configuredPath(application).trimmed();
  if (configured.isEmpty()) {
    result.error = tr("Set the application path in Preferences > Addons.");
    return result;
  }
  QFileInfo file(QDir(m_applicationDirectory).absoluteFilePath(configured));
#ifdef Q_OS_MAC
  if (file.isDir() && file.suffix().compare("app", Qt::CaseInsensitive) == 0) {
    QSettings info(file.absoluteFilePath() + "/Contents/Info.plist",
                   QSettings::NativeFormat);
    const QString executable = info.value("CFBundleExecutable").toString();
    // A bundle declares a filename, not a command or relative traversal.
    if (executable.isEmpty() || executable.contains('/') ||
        executable.contains('\\')) {
      result.error =
          tr("The selected application bundle has no valid executable.");
      return result;
    }
    file.setFile(file.absoluteFilePath() + "/Contents/MacOS/" + executable);
  }
#endif
  if (!file.isFile() || !file.isExecutable()) {
    result.error = tr("The configured executable is missing or cannot be run.");
    return result;
  }
#ifdef Q_OS_WIN
  if (file.suffix().compare("exe", Qt::CaseInsensitive) != 0) {
    result.error = tr("Select the application's executable (.exe).");
    return result;
  }
#endif
  const QString executable = file.canonicalFilePath();
#ifdef Q_OS_WIN
  const bool self =
      executable.compare(m_selfExecutable, Qt::CaseInsensitive) == 0;
#else
  const bool self = executable == m_selfExecutable;
#endif
  if (self) {
    result.error =
        tr("Select the other application's executable, not Groundzero.");
    return result;
  }
  result.executable       = executable;
  result.workingDirectory = file.absolutePath();
  return result;
}

void ExternalApplications::refresh() { emit changed(); }

void ExternalApplications::bindAction(Application application,
                                      QAction *action) {
  const auto update = [this, application, action]() {
    const auto executable = target(application);
    action->setEnabled(executable.available());
    action->setToolTip(
        executable.available()
            ? tr("Launch %1 as a separate application.").arg(name(application))
            : executable.error);
  };
  connect(this, &ExternalApplications::changed, action, update);
  update();
}

bool ExternalApplications::launch(Application application, QString *error) {
  if (error) error->clear();
  const Target executable = target(application);
  refresh();
  if (!executable.available()) {
    if (error) *error = executable.error;
    return false;
  }
  auto environment = QProcessEnvironment::systemEnvironment();
  // Let the other build choose its own Qt plugins and stuff/settings roots.
  // Do not inherit paths selected by Groundzero or its portable launcher.
  for (const char *key : {"QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                          "QML_IMPORT_PATH", "QML2_IMPORT_PATH", "TOONZROOT",
                          "OPENTOONZROOT", "TOONZPROFILES", "OpenToonzPROFILES",
                          "TOONZCONFIG", "TOONZFXPRESETS", "TOONZSTUDIOPALETTE",
                          "TOONZPROJECTS", "TOONZLIBRARY", "TOONZCACHEROOT"})
    environment.remove(QString::fromLatin1(key));
  // No shell, scene argument, save, export, or local room change is involved.
  return m_launcher({executable, {}, environment}, error);
}

QWidget *createExternalApplicationsSettings(ExternalApplications &applications,
                                            QWidget *parent) {
  auto *group =
      new QGroupBox(ExternalApplications::tr("Related Applications"), parent);
  auto *layout      = new QGridLayout(group);
  auto *description = new QLabel(
      ExternalApplications::tr(
          "Set application paths to enable their launch commands in Workflow. "
          "Leave a path empty to keep its command disabled."),
      group);
  description->setWordWrap(true);
  layout->addWidget(description, 0, 0, 1, 3);
  int row = 1;
  for (auto application :
       {ExternalApplications::Ztoryc, ExternalApplications::OT3D}) {
    auto *label =
        new QLabel(ExternalApplications::name(application) + ":", group);
    auto *path = new QLineEdit(group);
    path->setObjectName(application == ExternalApplications::Ztoryc
                            ? "ztorycApplicationPath"
                            : "ot3dApplicationPath");
    path->setPlaceholderText(
        ExternalApplications::tr("Select an application..."));
    auto *browse =
        new QPushButton(ExternalApplications::tr("Browse..."), group);
    auto *status = new QLabel(group);
    status->setWordWrap(true);
    const auto update = [&applications, application, path, status]() {
      const QSignalBlocker blocker(path);
      path->setText(applications.configuredPath(application));
      const auto target = applications.target(application);
      status->setText(target.available() ? ExternalApplications::tr("Available")
                                         : target.error);
    };
    QObject::connect(&applications, &ExternalApplications::changed, path,
                     update);
    QObject::connect(path, &QLineEdit::editingFinished, path,
                     [&applications, application, path]() {
                       applications.setConfiguredPath(application,
                                                      path->text());
                     });
    QObject::connect(browse, &QPushButton::clicked, group,
                     [&applications, application, group]() {
#ifdef Q_OS_WIN
                       const QString filter =
                           ExternalApplications::tr("Applications (*.exe)");
#elif defined(Q_OS_MAC)
      const QString filter = ExternalApplications::tr("Applications (*.app);;All Files (*)");
#else
      const QString filter = ExternalApplications::tr("All Files (*)");
#endif
                       const QString selected = QFileDialog::getOpenFileName(
                           group,
                           ExternalApplications::tr("Select %1")
                               .arg(ExternalApplications::name(application)),
                           applications.configuredPath(application), filter);
                       if (!selected.isEmpty())
                         applications.setConfiguredPath(application, selected);
                     });
    label->setBuddy(path);
    layout->addWidget(label, row, 0);
    layout->addWidget(path, row, 1);
    layout->addWidget(browse, row++, 2);
    layout->addWidget(status, row++, 1, 1, 2);
    update();
  }
  layout->setColumnStretch(1, 1);
  return group;
}
