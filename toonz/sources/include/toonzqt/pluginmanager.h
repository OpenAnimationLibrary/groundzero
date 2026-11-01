#pragma once

#include "tcommon.h"
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>

#undef DVAPI
#ifdef TOONZQT_EXPORTS
#define DVAPI DV_EXPORT_API
#else
#define DVAPI DV_IMPORT_API
#endif

// Manages third-party FX libraries only. No library is opened by this class.
class DVAPI PluginManager final : public QObject {
  Q_OBJECT
public:
  struct Settings {
    bool bypassAll = false;
    QSet<QString> blocked;
    QString error;
  };
  struct LoadResult {
    bool loaded = false;
    QString error;
  };
  struct Session {
    bool started      = false;
    bool launchBypass = false;
    Settings settings;
    QMap<QString, LoadResult> results;
  };

  static PluginManager *instance();
  // Consume before TCli parsing; inherited by child render processes only.
  static void takeLaunchArguments(int &argc, char **argv);
  PluginManager(const QString &directory, const QString &configurationPath,
                QObject *parent = nullptr);
  QString directory() const { return m_directory; }
  QString configurationPath() const { return m_configurationPath; }
  Settings settings() const;
  QStringList installedFiles() const;
  bool setEnabled(const QString &relativePath, bool enabled, QString *error);
  bool setBypassAll(bool bypass, QString *error);
  bool install(const QString &source, QString *error);

  // The loader captures one immutable policy for the session, shared by all FX.
  // Call on the main thread before starting the worker; child renders inherit
  // it.
  void prepareStartup();
  QStringList startupFiles();
  void recordLoadResult(const QString &file, bool loaded, const QString &error);
  Session session() const;

signals:
  void settingsChanged();
  void statusChanged();

private:
  QString key(const QString &relativePath) const;
  Settings readSettings(QStringList *lines = nullptr) const;
  bool update(const QString &path, bool value, bool bypass, QString *error);
  QString m_directory, m_configurationPath;
  mutable QMutex m_mutex;
  bool m_policyPrepared = false;
  Session m_session;
};
