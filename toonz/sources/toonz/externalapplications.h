#pragma once

#include <QObject>
#include <QProcessEnvironment>
#include <QStringList>
#include <functional>

class QAction;
class QWidget;

// Explicit executable paths only: no installation discovery or polling.
class ExternalApplications final : public QObject {
  Q_OBJECT
public:
  enum Application { Ztoryc, OT3D };
  struct Settings {
    std::function<QString(Application)> path;
    std::function<void(Application, const QString &)> savePath;
  };
  struct Target {
    QString executable, workingDirectory, error;
    bool available() const { return !executable.isEmpty(); }
  };
  struct Launch {
    Target target;
    QStringList arguments;
    QProcessEnvironment environment;
  };
  using Launcher = std::function<bool(const Launch &, QString *)>;

  ExternalApplications(QString applicationDirectory, QString selfExecutable,
                       Settings settings, Launcher launcher = {},
                       QObject *parent = nullptr);
  QString configuredPath(Application application) const;
  void setConfiguredPath(Application application, const QString &path);
  Target target(Application application) const;
  bool launch(Application application, QString *error = nullptr);
  void bindAction(Application application, QAction *action);
  void refresh();
  static const char *command(Application application);
  static QString name(Application application);

signals:
  void changed();

private:
  QString m_applicationDirectory, m_selfExecutable;
  Settings m_settings;
  Launcher m_launcher;
};

QWidget *createExternalApplicationsSettings(ExternalApplications &applications,
                                            QWidget *parent = nullptr);

namespace NativeExternalApplications {
ExternalApplications &instance();
}
