#include "toonzqt/pluginmanager.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMutexLocker>
#include <QSaveFile>
#include <QTemporaryDir>

namespace {
QString normalizedPath(QString path) {
  path = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
  if (path.isEmpty() || path == "." || path == ".." || path.startsWith("../") ||
      QDir::isAbsolutePath(path) || path.contains(':') || path.contains('\n') ||
      path.contains('\r'))
    return QString();
  return path;
}

QStringList discover(const QString &directory) {
  QStringList result;
  // Do not follow directory symlinks: they may form cycles or escape the root.
  QDirIterator it(directory, QStringList("*.plugin"), QDir::Files,
                  QDirIterator::Subdirectories);
  while (it.hasNext())
    result.append(QDir(directory).relativeFilePath(it.next()));
  result.sort();
  return result;
}

bool copyPackage(const QString &source, const QString &target, QString *error) {
  const QFileInfo info(source);
  if (info.isSymLink()) {
    *error = QObject::tr("Plugin packages must not contain symbolic links.");
    return false;
  }
  if (info.isFile()) {
    if (QFile::copy(source, target)) return true;
  } else if (info.isDir() && QDir().mkpath(target)) {
    const auto entries = QDir(source).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const auto &entry : entries)
      if (!copyPackage(entry.filePath(),
                       QDir(target).filePath(entry.fileName()), error))
        return false;
    return true;
  }
  *error = QObject::tr("Could not copy %1.").arg(source);
  return false;
}
}  // namespace

PluginManager::PluginManager(const QString &directory,
                             const QString &configurationPath, QObject *parent)
    : QObject(parent)
    , m_directory(QDir(directory).absolutePath())
    , m_configurationPath(QFileInfo(configurationPath).absoluteFilePath()) {
  m_session.launchBypass = qgetenv("GROUNDZERO_NO_PLUGINS") == "1";
}

void PluginManager::takeLaunchArguments(int &argc, char **argv) {
  for (int i = 1; i < argc;) {
    if (QString::fromLocal8Bit(argv[i]) != "--no-plugins") {
      ++i;
      continue;
    }
    qputenv("GROUNDZERO_NO_PLUGINS", "1");
    for (int j = i; j < argc; ++j) argv[j] = argv[j + 1];
    --argc;
  }
}

QString PluginManager::key(const QString &relativePath) const {
  QString result = normalizedPath(relativePath);
#ifdef Q_OS_WIN
  result = result.toCaseFolded();
#endif
  return result;
}

PluginManager::Settings PluginManager::readSettings(QStringList *lines) const {
  Settings result;
  QFile file(m_configurationPath);
  if (!file.exists()) return result;
  if (!file.open(QIODevice::ReadOnly)) {
    result.error =
        tr("Cannot read plugin configuration: %1").arg(file.errorString());
    return result;
  }
  QByteArray bytes = file.readAll();
  if (bytes.startsWith("\xef\xbb\xbf")) bytes.remove(0, 3);
  QString text = QString::fromUtf8(bytes);
  if (file.error() != QFile::NoError) {
    result.error =
        tr("Cannot read plugin configuration: %1").arg(file.errorString());
    return result;
  }
  if (text.toUtf8() != bytes) {
    result.error = tr("Plugin configuration must use UTF-8 encoding.");
    return result;
  }
  const QStringList content = text.split('\n');
  if (lines) *lines = content;
  for (int i = 0; i < content.size(); ++i) {
    const QString line = content[i].trimmed();
    if (line.isEmpty() || line.startsWith('#')) continue;
    if (line == "@bypass-all") {
      result.bypassAll = true;
      continue;
    }
    const QString path = key(line);
    if (line.startsWith('@') || path.isEmpty() ||
        !path.endsWith(".plugin", Qt::CaseInsensitive)) {
      result.error =
          tr("Invalid plugin configuration entry on line %1.").arg(i + 1);
      return result;
    }
    result.blocked.insert(path);
  }
  return result;
}

PluginManager::Settings PluginManager::settings() const {
  return readSettings();
}

QStringList PluginManager::installedFiles() const {
  return discover(m_directory);
}

bool PluginManager::update(const QString &path, bool value, bool bypass,
                           QString *error) {
  QString detail;
  if (!error) error = &detail;
  error->clear();
  const QString configDirectory = QFileInfo(m_configurationPath).absolutePath();
  if (!QDir().mkpath(configDirectory)) {
    *error = tr("Cannot create the plugin configuration folder.");
    return false;
  }
  QLockFile lock(m_configurationPath + ".lock");
  if (!lock.tryLock()) {
    *error =
        tr("Another instance is updating the plugin configuration. Retry "
           "shortly.");
    return false;
  }
  QStringList lines;
  const Settings current = readSettings(&lines);
  if (!current.error.isEmpty()) {
    *error = current.error;
    return false;
  }
  if (lines.isEmpty()) {
    lines << "# Third-party FX plugin blocklist. UTF-8; paths relative to "
             "plugins."
          << "# Listed .plugin libraries are disabled at startup."
          << "# @bypass-all skips all plugins without changing individual "
             "entries.";
  }
  const QString target = bypass ? QString("@bypass-all") : key(path);
  if (target.isEmpty() ||
      (!bypass && !target.endsWith(".plugin", Qt::CaseInsensitive))) {
    *error = tr("Invalid plugin path.");
    return false;
  }
  // Preserve manual comments and entries for temporarily missing plugins.
  for (int i = lines.size() - 1; i >= 0; --i) {
    const QString line = lines[i].trimmed();
    if (line.isEmpty() || line.startsWith('#')) continue;
    if ((bypass && line == target) || (!bypass && key(line) == target))
      lines.removeAt(i);
  }
  if (value) lines.append(bypass ? target : "./" + target);
  QSaveFile file(m_configurationPath);
  const QByteArray bytes = (lines.join('\n') + '\n').toUtf8();
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    *error = tr("Cannot save plugin configuration: %1").arg(file.errorString());
    return false;
  }
  emit settingsChanged();
  return true;
}

bool PluginManager::setEnabled(const QString &path, bool enabled,
                               QString *error) {
  return update(path, !enabled, false, error);
}

bool PluginManager::setBypassAll(bool bypass, QString *error) {
  return update(QString(), bypass, true, error);
}

bool PluginManager::install(const QString &source, QString *error) {
  QString detail;
  if (!error) error = &detail;
  error->clear();
  const QFileInfo info(source);
  if ((!info.isDir() &&
       (!info.isFile() ||
        info.suffix().compare("plugin", Qt::CaseInsensitive))) ||
      info.isSymLink() || info.fileName().isEmpty()) {
    *error = tr("Select a .plugin library or a folder containing plugins.");
    return false;
  }
  // Staging is outside the scanned tree, including if installation is
  // interrupted.
  const QString parent = QFileInfo(m_directory).absolutePath();
  if (!QDir().mkpath(m_directory)) {
    *error = tr("Cannot create the plugin folder.");
    return false;
  }
  QLockFile lock(QDir(parent).filePath("plugin-install.lock"));
  if (!lock.tryLock()) {
    *error = tr("Another plugin installation is in progress.");
    return false;
  }
  const QString target = QDir(m_directory).filePath(info.fileName());
  if (QFileInfo::exists(target)) {
    *error = tr("%1 is already installed. Existing files were kept.")
                 .arg(info.fileName());
    return false;
  }
  const QString canonicalRoot   = QFileInfo(m_directory).canonicalFilePath();
  const QString canonicalSource = info.canonicalFilePath();
  if (info.isDir() && (canonicalRoot == canonicalSource ||
                       canonicalRoot.startsWith(canonicalSource + '/'))) {
    *error = tr("The source package must not contain the plugin folder.");
    return false;
  }
  QTemporaryDir stage(QDir(parent).filePath(".plugin-install-XXXXXX"));
  const QString staged = stage.filePath(info.fileName());
  if (!stage.isValid() || !copyPackage(source, staged, error)) {
    if (error->isEmpty()) *error = tr("Cannot stage the plugin installation.");
    return false;
  }
  if (info.isDir() && discover(staged).isEmpty()) {
    *error = tr("The selected folder contains no .plugin libraries.");
    return false;
  }
  if (!QDir().rename(staged, target)) {
    *error =
        tr("Cannot finish the plugin installation. Existing files were kept.");
    return false;
  }
  emit settingsChanged();
  return true;
}

void PluginManager::prepareStartup() {
  const QMutexLocker locker(&m_mutex);
  if (m_policyPrepared || m_session.started) return;
  m_policyPrepared   = true;
  m_session.settings = readSettings();
  // Keep separately launched tcomposer processes consistent with their parent
  // even when the user has already saved changes for the next Groundzero
  // launch.
  const QJsonObject inherited =
      QJsonDocument::fromJson(
          QByteArray::fromBase64(qgetenv("GROUNDZERO_FX_PLUGIN_POLICY")))
          .object();
#ifdef Q_OS_WIN
  const auto sensitivity = Qt::CaseInsensitive;
#else
  const auto sensitivity = Qt::CaseSensitive;
#endif
  if (inherited.value("directory")
              .toString()
              .compare(m_directory, sensitivity) == 0 &&
      inherited.value("blocked").isArray() &&
      inherited.value("bypass").isBool() &&
      inherited.value("files").isArray()) {
    m_session.settings.bypassAll = inherited.value("bypass").toBool();
    m_session.settings.error     = inherited.value("error").toString();
    m_session.settings.blocked.clear();
    for (const auto &entry : inherited.value("blocked").toArray()) {
      const QString path = key(entry.toString());
      if (!path.isEmpty()) m_session.settings.blocked.insert(path);
    }
    for (const auto &entry : inherited.value("files").toArray()) {
      const QString path = normalizedPath(entry.toString());
      if (!path.isEmpty()) m_startupFiles.append(path);
    }
  } else if (!m_session.launchBypass && !m_session.settings.bypassAll &&
             m_session.settings.error.isEmpty()) {
    // Freeze discovery too: plugins installed during this session belong to
    // the next launch and must not appear early in a child renderer.
    m_startupFiles = installedFiles();
  }
  QJsonArray blocked;
  for (const QString &path : m_session.settings.blocked) blocked.append(path);
  QJsonArray files;
  for (const QString &path : m_startupFiles) files.append(path);
  const QJsonObject policy{{"directory", m_directory},
                           {"blocked", blocked},
                           {"files", files},
                           {"bypass", m_session.settings.bypassAll},
                           {"error", m_session.settings.error}};
  qputenv("GROUNDZERO_FX_PLUGIN_POLICY",
          QJsonDocument(policy).toJson(QJsonDocument::Compact).toBase64());
}

QStringList PluginManager::startupFiles() {
  const QMutexLocker locker(&m_mutex);
  if (m_session.started) return QStringList();
  m_session.started = true;
  if (!m_policyPrepared) m_session.settings = readSettings();
  if (m_session.launchBypass || m_session.settings.bypassAll ||
      !m_session.settings.error.isEmpty())
    return QStringList();
  QStringList result;
  for (const QString &path :
       m_policyPrepared ? m_startupFiles : installedFiles())
    if (!m_session.settings.blocked.contains(key(path)))
      result.append(QDir(m_directory).filePath(path));
  return result;
}

void PluginManager::recordLoadResult(const QString &file, bool loaded,
                                     const QString &error) {
  {
    const QMutexLocker locker(&m_mutex);
    m_session.results.insert(key(QDir(m_directory).relativeFilePath(file)),
                             {loaded, error});
  }
  emit statusChanged();
}

PluginManager::Session PluginManager::session() const {
  const QMutexLocker locker(&m_mutex);
  return m_session;
}
