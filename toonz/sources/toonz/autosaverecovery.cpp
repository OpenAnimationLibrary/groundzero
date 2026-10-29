#include "autosaverecovery.h"

// Tnz6 includes
#include "tapp.h"
#include "iocommand.h"
#include "crashhandler.h"

// TnzQt includes
#include "toonzqt/dvdialog.h"
#include "toonzqt/uitranslations.h"

// TnzLib includes
#include "toonz/toonzscene.h"
#include "toonz/toonzfolders.h"
#include "toonz/tproject.h"
#include "toonz/levelset.h"
#include "toonz/levelproperties.h"
#include "toonz/txshlevel.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshpalettelevel.h"
#include "toonz/txshsoundlevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/preferences.h"

// TnzCore includes
#include "tsystem.h"
#include "tstream.h"
#include "tpalette.h"
#include "tundo.h"
#include "tenv.h"

// Qt includes
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QList>
#include <QLocale>
#include <QLockFile>
#include <QMainWindow>
#include <QMap>
#include <QPair>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

//=============================================================================

namespace {

// Recovery copies kept for each scene
const int MaxVersionsPerScene = 5;
// Recovery copies older than this are removed at startup
const int MaxAgeDays = 14;

enum RecoveryItemRole {
  SceneNameRole = Qt::UserRole + 2,
  OriginalSceneRole,
  UncopiedLevelsRole
};

const char *ManifestFileName    = "recovery.ini";
const char *SessionLockFileName = "session.lock";

bool s_initialized = false;
// Whether the scene may have changed since the last recovery copy
bool s_changed = true;
// Whether the previous session did not exit normally, and when it started
bool s_previousSessionInterrupted = false;
qint64 s_previousSessionStart     = 0;  // ms since epoch
// When this session started: only its own copies are pruned per scene
qint64 s_sessionStart = 0;  // ms since epoch
// Whether the session marker is held by another running instance
bool s_lockedByOtherInstance = false;
// The session marker: removed on normal exit, left behind by a crash or kill
std::unique_ptr<QLockFile> s_sessionLock;

//-----------------------------------------------------------------------------

struct Snapshot {
  QString m_folder;     // absolute path of the recovery folder
  QString m_sceneFile;  // absolute path of the recovery copy (.tnz)
  QString m_sceneName;
  QString m_originalScene;      // saved scene file; empty if untitled
  QDateTime m_savedAt;          // UTC
  bool m_userModified = false;  // the user saved over the copy afterwards
  // Levels with unsaved changes that could not be copied: the copy refers to
  // their saved files, which lack those changes
  QStringList m_uncopiedLevels;
};

//-----------------------------------------------------------------------------

bool isRecoveryModeEnabled() {
  Preferences *pref = Preferences::instance();
  return pref->isAutosaveEnabled() && pref->isAutosaveToRecoveryFolderEnabled();
}

QString recoveryRoot() {
  return QDir::cleanPath(
      (ToonzFolder::getMyModuleDir() + "recovery").getQString());
}

bool isSnapshotFolderName(const QString &name) {
  static const QRegularExpression re("^\\d{8}_\\d{6}_");
  return re.match(name).hasMatch();
}

// Folder name part: no dots, so that it is never taken for a file type
QString safeFolderName(QString name) {
  static const QString invalidChars(".\\/:*?\"<>|");
  for (int i = 0; i < name.size(); i++) {
    const QChar c = name.at(i);
    if (c.unicode() < 32 || invalidChars.contains(c)) name[i] = QChar('_');
  }
  return name.left(64);
}

QString groupKey(const QString &originalScene, const QString &sceneName) {
  if (originalScene.isEmpty()) return "untitled:" + sceneName;
  QString key = QDir::cleanPath(originalScene);
#ifdef _WIN32
  key = key.toLower();
#endif
  return key;
}

QString groupKey(const Snapshot &s) {
  return groupKey(s.m_originalScene, s.m_sceneName);
}

//-----------------------------------------------------------------------------

bool readSnapshot(const QString &folder, Snapshot &s) {
  const QString manifest = QDir(folder).filePath(ManifestFileName);
  if (!QFileInfo::exists(manifest)) return false;

  QSettings ini(manifest, QSettings::IniFormat);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  ini.setIniCodec("UTF-8");  // Qt 6 reads and writes UTF-8
#endif
  ini.beginGroup("Recovery");
  // Only a file name: the copy is always inside its recovery folder
  const QString sceneFile =
      QFileInfo(ini.value("sceneFile").toString()).fileName();
  s.m_folder        = folder;
  s.m_sceneFile     = QDir(folder).filePath(sceneFile);
  s.m_sceneName     = ini.value("sceneName").toString();
  s.m_originalScene = ini.value("originalScene").toString();
  s.m_savedAt =
      QDateTime::fromString(ini.value("savedAt").toString(), Qt::ISODateWithMs);
  ini.endGroup();
  const int uncopiedCount = ini.beginReadArray("uncopiedLevels");
  for (int i = 0; i < uncopiedCount; i++) {
    ini.setArrayIndex(i);
    s.m_uncopiedLevels.append(ini.value("original").toString());
  }
  ini.endArray();

  if (sceneFile.isEmpty() || !s.m_savedAt.isValid() ||
      !QFileInfo::exists(s.m_sceneFile))
    return false;

  // The manifest is written right after the copy
  s.m_userModified =
      QFileInfo(s.m_sceneFile).lastModified().toMSecsSinceEpoch() >
      QFileInfo(manifest).lastModified().toMSecsSinceEpoch() + 2000;
  return true;
}

QList<Snapshot> listSnapshots() {
  QList<Snapshot> snapshots;
  QDir dir(recoveryRoot());
  const QStringList names = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QString &name : names) {
    if (!isSnapshotFolderName(name)) continue;
    Snapshot s;
    if (readSnapshot(dir.filePath(name), s)) snapshots.append(s);
  }
  return snapshots;
}

bool removeSnapshotFolder(const QString &folder) {
  // Never remove anything else than a recovery folder
  const QFileInfo fi(folder);
  if (!isSnapshotFolderName(fi.fileName()) ||
      QDir::cleanPath(fi.absolutePath()) != recoveryRoot())
    return false;
  return QDir(folder).removeRecursively();
}

// Keep the latest versions of the scene written by this session. Copies the
// user saved over are kept, and so are the copies of earlier sessions: those
// of an interrupted session are the ones to recover, and the recovery dialog
// promises to keep them until they expire.
void removeOldVersions(const QString &key) {
  QList<Snapshot> versions;
  for (const Snapshot &s : listSnapshots())
    if (!s.m_userModified && groupKey(s) == key &&
        s.m_savedAt.toMSecsSinceEpoch() >= s_sessionStart)
      versions.append(s);
  std::sort(versions.begin(), versions.end(),
            [](const Snapshot &a, const Snapshot &b) {
              return a.m_savedAt > b.m_savedAt;
            });
  // Failed resource copies may be retried without another edit. Bound those
  // partial snapshots separately so repeated failures cannot evict the last
  // complete recovery copy or grow the recovery folder without limit.
  int completeCount = 0, partialCount = 0;
  for (const Snapshot &version : versions) {
    int &count =
        version.m_uncopiedLevels.isEmpty() ? completeCount : partialCount;
    if (++count > MaxVersionsPerScene) removeSnapshotFolder(version.m_folder);
  }
}

void removeExpiredSnapshots() {
  const QDateTime limit = QDateTime::currentDateTimeUtc().addDays(-MaxAgeDays);
  QDir dir(recoveryRoot());
  const QStringList names = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QString &name : names) {
    if (!isSnapshotFolderName(name)) continue;
    const QString folder = dir.filePath(name);
    Snapshot s;
    if (readSnapshot(folder, s)) {
      if (!s.m_userModified && s.m_savedAt < limit)
        removeSnapshotFolder(folder);
    } else if (QFileInfo(folder).lastModified().toUTC() < limit)
      // Incomplete copy, e.g. interrupted by a crash
      removeSnapshotFolder(folder);
  }
}

//-----------------------------------------------------------------------------

bool acquireSessionLock() {
  if (s_sessionLock) return true;
  if (s_lockedByOtherInstance) return false;

  const QString root = recoveryRoot();
  if (!QDir().mkpath(root)) return false;

  std::unique_ptr<QLockFile> lock(
      new QLockFile(QDir(root).filePath(SessionLockFileName)));
  // A lock is stale only when its process is gone, never because of its age
  lock->setStaleLockTime(0);
  if (!lock->tryLock(0)) {
    // Another OpenToonz instance is running with this user profile
    if (lock->error() == QLockFile::LockFailedError)
      s_lockedByOtherInstance = true;
    return false;
  }
  s_sessionLock = std::move(lock);
  return true;
}

//-----------------------------------------------------------------------------

void setLevelPath(TXshLevel *level, const TFilePath &path) {
  // Keep the loaded frames: only the path written to the scene changes
  if (TXshSimpleLevel *sl = level->getSimpleLevel())
    sl->setPath(path, true);
  else if (TXshPaletteLevel *pl = level->getPaletteLevel())
    pl->setPath(path);
  else if (TXshSoundLevel *sdl = level->getSoundLevel())
    sdl->setPath(path);
}

// Sets level paths for the recovery copy and restores them when destroyed
class LevelPathSwapper {
  std::vector<std::pair<TXshLevel *, TFilePath>> m_oldPaths;

public:
  ~LevelPathSwapper() {
    for (auto it = m_oldPaths.rbegin(); it != m_oldPaths.rend(); ++it)
      setLevelPath(it->first, it->second);
  }

  void setPath(TXshLevel *level, const TFilePath &path) {
    m_oldPaths.push_back(std::make_pair(level, level->getPath()));
    setLevelPath(level, path);
  }
};

class RecoverySaveGuard {
public:
  RecoverySaveGuard() { TApp::instance()->setSaveInProgress(true); }
  ~RecoverySaveGuard() { TApp::instance()->setSaveInProgress(false); }
};

//-----------------------------------------------------------------------------

bool hasUnsavedChanges(TXshSimpleLevel *sl, const TFilePath &decodedPath) {
  if (sl->getProperties()->getDirtyFlag()) return true;
  // The full-color palette of raster levels is not stored with the level
  TPalette *palette = sl->getPalette();
  if (palette && palette->getDirtyFlag() && !(sl->getType() & FULLCOLOR_TYPE))
    return true;
  // A level that was never saved
  return sl->getFrameCount() > 0 && !TSystem::doesExistFileOrLevel(decodedPath);
}

// Path of the copy of a level, relative to the recovery folder
TFilePath getRelativeCopyPath(const TFilePath &folder, const TFilePath &path,
                              const TFilePath &decodedPath,
                              QSet<QString> &usedPaths) {
  TFilePath relPath = TFilePath("levels") + decodedPath.withoutParentDir();

  // Keep the layout of the levels in the scene folder, if it stays inside the
  // recovery folder
  const TFilePath sceneFolderAlias("$scenefolder");
  if (sceneFolderAlias.isAncestorOf(path)) {
    const TFilePath candidate = path - sceneFolderAlias;
    const QString cleanFolder = QDir::cleanPath(folder.getQString());
    if (!candidate.isEmpty() && !candidate.isAbsolute() &&
        QDir::cleanPath((folder + candidate).getQString())
            .startsWith(cleanFolder + "/"))
      relPath = candidate;
  }

  const std::wstring name = relPath.getWideName();
  TFilePath result        = relPath;
  for (int i = 2;
       usedPaths.contains(QDir::cleanPath(result.getQString()).toLower()); i++)
    result = relPath.withName(name + L"_" + std::to_wstring(i));
  usedPaths.insert(QDir::cleanPath(result.getQString()).toLower());
  return result;
}

bool saveLevelCopy(TXshSimpleLevel *sl, const TFilePath &copyPath) {
  TPalette *palette       = sl->getPalette();
  const bool levelDirty   = sl->getProperties()->getDirtyFlag();
  const bool paletteDirty = palette && palette->getDirtyFlag();
  const bool askOverwrite = palette && palette->getAskOverwriteFlag();

  // Saving a raster level with a modified full-color palette would write that
  // palette, shared by the raster levels of the project, in place
  if (paletteDirty && (sl->getType() & FULLCOLOR_TYPE))
    palette->setDirtyFlag(false);

  bool ok = true;
  try {
    // Saving to another path copies the saved level, then writes the
    // modified frames and the palette. The level keeps its path.
    sl->save(copyPath, TFilePath(), true);
  } catch (...) {
    ok = false;
  }

  // The copy must not make the level or its palette look saved
  sl->getProperties()->setDirtyFlag(levelDirty);
  if (palette) {
    palette->setDirtyFlag(paletteDirty);
    palette->setAskOverwriteFlag(askOverwrite);
  }
  return ok && TSystem::doesExistFileOrLevel(copyPath);
}

bool savePaletteCopy(TPalette *palette, const TFilePath &copyPath) {
  try {
    if (!TSystem::touchParentDir(copyPath)) return false;
    TOStream os(copyPath);
    if (!os.checkStatus()) return false;
    os << palette;
    return os.checkStatus();
  } catch (...) {
    return false;
  }
}

// Link the copy to the project of the scene, so that it opens in that project
// and the project folder aliases of the scene settings resolve as before
void writeProjectLink(const TFilePath &folder, ToonzScene *scene) {
  std::shared_ptr<TProject> project = scene->getProject();
  if (!project) return;
  try {
    TOStream os(folder + "scenes.xml");
    std::map<std::string, std::string> attr;
    attr["type"] = "projectFolder";
    os.openChild("parentProject", attr);
    os << project->getProjectFolder();
    os.closeChild();
  } catch (...) {
  }
}

bool writeManifest(const QString &folder, const QString &sceneFile,
                   const QString &sceneName, const QString &originalScene,
                   const QString &projectPath, const QDateTime &savedAt,
                   const QList<QPair<QString, QString>> &copiedLevels,
                   const QStringList &uncopiedLevels) {
  QSettings ini(QDir(folder).filePath(ManifestFileName), QSettings::IniFormat);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  ini.setIniCodec("UTF-8");  // Qt 6 reads and writes UTF-8
#endif
  ini.beginGroup("Recovery");
  ini.setValue("version", 1);
  ini.setValue("application",
               QString::fromStdString(TEnv::getApplicationFullName()));
  ini.setValue("sceneFile", sceneFile);
  ini.setValue("sceneName", sceneName);
  ini.setValue("originalScene", originalScene);
  ini.setValue("project", projectPath);
  ini.setValue("savedAt", savedAt.toString(Qt::ISODateWithMs));
  ini.endGroup();
  // Levels with unsaved changes: saved file -> copy in the recovery folder
  ini.beginWriteArray("copiedLevels", copiedLevels.size());
  for (int i = 0; i < copiedLevels.size(); i++) {
    ini.setArrayIndex(i);
    ini.setValue("original", copiedLevels[i].first);
    ini.setValue("copy", copiedLevels[i].second);
  }
  ini.endArray();
  // Levels with unsaved changes whose copy failed
  ini.beginWriteArray("uncopiedLevels", uncopiedLevels.size());
  for (int i = 0; i < uncopiedLevels.size(); i++) {
    ini.setArrayIndex(i);
    ini.setValue("original", uncopiedLevels[i]);
  }
  ini.endArray();
  ini.sync();
  return ini.status() == QSettings::NoError;
}

}  // namespace

//=============================================================================
// AutosaveRecovery
//-----------------------------------------------------------------------------

void AutosaveRecovery::initSession() {
  if (s_initialized) return;
  s_initialized  = true;
  s_sessionStart = QDateTime::currentMSecsSinceEpoch();

  // A new recovery copy is written only after a change
  auto setChanged = []() { s_changed = true; };
  TApp *app       = TApp::instance();
  QObject::connect(TUndoManager::manager(), &TUndoManager::historyChanged,
                   setChanged);
  QObject::connect(app->getCurrentScene(), &TSceneHandle::sceneSwitched,
                   setChanged);
  QObject::connect(app->getCurrentScene(), &TSceneHandle::sceneChanged,
                   setChanged);
  QObject::connect(app->getCurrentScene(), &TSceneHandle::castChanged,
                   setChanged);
  QObject::connect(app->getCurrentScene(), &TSceneHandle::nameSceneChanged,
                   setChanged);
  QObject::connect(app->getCurrentXsheet(), &TXsheetHandle::xsheetChanged,
                   setChanged);

  const QString root = recoveryRoot();
  const bool enabled = isRecoveryModeEnabled();

  // Nothing to check if the recovery mode was never enabled
  if (!QFileInfo(root).isDir()) {
    if (enabled) acquireSessionLock();
    return;
  }

  const QFileInfo lockInfo(QDir(root).filePath(SessionLockFileName));
  if (lockInfo.exists()) {
    const qint64 lockTime = lockInfo.lastModified().toMSecsSinceEpoch();
    // Taking over the marker succeeds only if its session is gone
    if (acquireSessionLock()) {
      s_previousSessionInterrupted = true;
      s_previousSessionStart       = lockTime;
    }
  } else if (enabled)
    acquireSessionLock();

  // Without the recovery mode, this session leaves no marker behind
  if (!enabled) s_sessionLock.reset();

  removeExpiredSnapshots();
}

//-----------------------------------------------------------------------------

void AutosaveRecovery::endSession() { s_sessionLock.reset(); }

//-----------------------------------------------------------------------------

bool AutosaveRecovery::saveRecoveryCopy() {
  if (!s_initialized || !s_changed) return false;

  TApp *app         = TApp::instance();
  ToonzScene *scene = app->getCurrentScene()->getScene();
  if (!scene || app->isSaveInProgress()) return false;

  // The recovery mode may have been enabled after the launch
  acquireSessionLock();

  const QString root = recoveryRoot();
  QDir rootDir(root);
  if (!QDir().mkpath(root)) return false;

  const QDateTime savedAt = QDateTime::currentDateTimeUtc();
  QString sceneName       = QString::fromStdWString(scene->getSceneName());
  if (sceneName.isEmpty()) sceneName = "untitled";
  const QString originalScene =
      scene->isUntitled()
          ? QString()
          : scene->decodeFilePath(scene->getScenePath()).getQString();

  const QString baseName = savedAt.toLocalTime().toString("yyyyMMdd_HHmmss") +
                           "_" + safeFolderName(sceneName);
  QString folderName = baseName;
  for (int i = 2; rootDir.exists(folderName); i++)
    folderName = baseName + "_" + QString::number(i);
  if (!rootDir.mkdir(folderName)) return false;
  const QString folder = rootDir.filePath(folderName);
  const TFilePath folderFp(folder);
  const QString sceneFile = sceneName + ".tnz";

  RecoverySaveGuard saveGuard;

  QList<QPair<QString, QString>> copiedLevels;
  QStringList uncopiedLevels;
  bool sceneSaved = true;
  {
    LevelPathSwapper levelPaths;

    std::vector<TXshLevel *> levels;
    scene->getLevelSet()->listLevels(levels);
    QSet<QString> usedCopyPaths;
    for (TXshLevel *level : levels) {
      if (!level) continue;
      TXshSimpleLevel *sl  = level->getSimpleLevel();
      TXshPaletteLevel *pl = level->getPaletteLevel();
      if (!sl && !pl && !level->getSoundLevel()) continue;

      const TFilePath path = level->getPath();
      if (path.isEmpty()) continue;

      // The copy refers to the saved files with absolute paths...
      const TFilePath decodedPath = scene->decodeFilePath(path);
      TFilePath newPath           = decodedPath;

      // ...and to the copies of the unsaved levels with $scenefolder, so that
      // Save Scene As can copy them next to the saved scene
      TPalette *palette = pl ? pl->getPalette() : 0;
      if ((sl && hasUnsavedChanges(sl, decodedPath)) ||
          (palette && palette->getDirtyFlag())) {
        const TFilePath relPath =
            getRelativeCopyPath(folderFp, path, decodedPath, usedCopyPaths);
        const TFilePath copyPath = folderFp + relPath;
        if (sl ? saveLevelCopy(sl, copyPath)
               : savePaletteCopy(palette, copyPath)) {
          newPath = TFilePath("$scenefolder") + relPath;
          copiedLevels.append(
              qMakePair(decodedPath.getQString(), relPath.getQString()));
        } else
          uncopiedLevels.append(decodedPath.getQString());
      }

      if (newPath != path) levelPaths.setPath(level, newPath);
    }

    writeProjectLink(folderFp, scene);

    try {
      scene->saveCopy(folderFp + sceneFile.toStdWString());
    } catch (...) {
      sceneSaved = false;
    }
  }  // The levels get their paths back here

  std::shared_ptr<TProject> project = scene->getProject();
  if (!sceneSaved ||
      !writeManifest(
          folder, sceneFile, sceneName, originalScene,
          project ? project->getProjectPath().getQString() : QString(), savedAt,
          copiedLevels, uncopiedLevels)) {
    removeSnapshotFolder(folder);
    return false;
  }

  // A partial snapshot is useful, but its missing drawings still need a retry
  // even if the artist makes no further edits.
  const bool complete = uncopiedLevels.isEmpty();
  s_changed           = !complete;
  removeOldVersions(groupKey(originalScene, sceneName));
  return complete;
}

//-----------------------------------------------------------------------------

bool AutosaveRecovery::showRecoveryDialog() {
  if (!s_previousSessionInterrupted) return false;
  s_previousSessionInterrupted = false;

  // The latest copy of each scene written by the interrupted session, if it
  // is newer than the saved scene
  QMap<QString, Snapshot> latest;
  QMap<QString, QStringList> folders;
  for (const Snapshot &s : listSnapshots()) {
    const qint64 savedAt = s.m_savedAt.toMSecsSinceEpoch();
    if (savedAt < s_previousSessionStart - 2000) continue;
    if (!s.m_originalScene.isEmpty()) {
      const QFileInfo fi(s.m_originalScene);
      if (fi.exists() && fi.lastModified().toMSecsSinceEpoch() >= savedAt)
        continue;
    }
    const QString key = groupKey(s);
    folders[key].append(s.m_folder);
    if (!latest.contains(key) || latest[key].m_savedAt < s.m_savedAt)
      latest[key] = s;
  }
  if (latest.isEmpty()) return false;

  QList<Snapshot> snapshots = latest.values();
  std::sort(snapshots.begin(), snapshots.end(),
            [](const Snapshot &a, const Snapshot &b) {
              return a.m_savedAt > b.m_savedAt;
            });

  RecoveryDialog dialog(TApp::instance()->getMainWindow(),
                        CrashHandler::hasPendingReport());
  for (const Snapshot &s : snapshots) {
    QStringList groupFolders = folders.value(groupKey(s));
    // Show Folder opens the latest copy
    groupFolders.removeAll(s.m_folder);
    groupFolders.prepend(s.m_folder);
    dialog.addItem(
        s.m_sceneName,
        QLocale().toString(s.m_savedAt.toLocalTime(), QLocale::ShortFormat),
        s.m_originalScene, s.m_sceneFile, groupFolders, s.m_uncopiedLevels);
  }
  dialog.exec();

  const QString sceneToOpen = dialog.getSceneToOpen();
  if (!sceneToOpen.isEmpty() && QFileInfo::exists(sceneToOpen))
    IoCmd::loadScene(TFilePath(sceneToOpen), false);
  return true;
}

//=============================================================================
// RecoveryDialog
//-----------------------------------------------------------------------------

RecoveryDialog::RecoveryDialog(QWidget *parent, bool hasCrashReport)
    : QDialog(parent)
    , m_hasCrashReport(hasCrashReport)
    , m_crashReportButton(0) {
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  setWindowTitle(tr("Recover Unsaved Work"));

  m_introLabel = new QLabel(this);
  m_introLabel->setWordWrap(true);
  m_introLabel->setTextFormat(Qt::RichText);

  m_list = new QTreeWidget(this);
  m_list->setColumnCount(3);
  m_list->setRootIsDecorated(false);
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);
  m_list->header()->setStretchLastSection(true);
  m_list->setMinimumWidth(600);

  m_openButton             = new QPushButton(tr("Open"), this);
  m_folderButton           = new QPushButton(tr("Show Folder"), this);
  m_discardButton          = new QPushButton(tr("Discard"), this);
  QPushButton *closeButton = new QPushButton(tr("Close"), this);
  m_openButton->setDefault(true);

  QHBoxLayout *buttonsLay = new QHBoxLayout();
  buttonsLay->addWidget(m_openButton);
  buttonsLay->addWidget(m_folderButton);
  buttonsLay->addWidget(m_discardButton);
  if (hasCrashReport) {
    m_crashReportButton = new QPushButton(tr("Crash Report"), this);
    buttonsLay->addWidget(m_crashReportButton);
    connect(m_crashReportButton, &QPushButton::clicked, this,
            &RecoveryDialog::onCrashReport);
  }
  buttonsLay->addStretch(1);
  buttonsLay->addWidget(closeButton);

  QVBoxLayout *mainLayout = new QVBoxLayout();
  mainLayout->addWidget(m_introLabel);
  mainLayout->addWidget(m_list, 1);
  mainLayout->addLayout(buttonsLay);
  setLayout(mainLayout);

  connect(m_list, &QTreeWidget::currentItemChanged, this,
          &RecoveryDialog::onCurrentItemChanged);
  connect(m_list, &QTreeWidget::itemDoubleClicked, this,
          &RecoveryDialog::onOpen);
  connect(m_openButton, &QPushButton::clicked, this, &RecoveryDialog::onOpen);
  connect(m_folderButton, &QPushButton::clicked, this,
          &RecoveryDialog::onShowFolder);
  connect(m_discardButton, &QPushButton::clicked, this,
          &RecoveryDialog::onDiscard);
  connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);

  retranslateUi();
  UiTranslations::onLanguageChanged(this, [this]() { retranslateUi(); });
  onCurrentItemChanged();
}

//-----------------------------------------------------------------------------

void RecoveryDialog::retranslateUi() {
  QStringList sl;
  if (m_hasCrashReport)
    sl.append(tr("<b>OpenToonz crashed during the previous session.</b>"));
  else
    sl.append(
        tr("<b>OpenToonz did not shut down normally during the previous "
           "session.</b>"));
  sl.append(
      tr("Autosave recovery copies newer than the saved scenes were found. "
         "Open a copy to check it, then use Save Scene As to keep it. The "
         "levels that had unsaved changes are stored with the copy, and Save "
         "Scene As offers to copy them next to the saved scene."));
  sl.append(tr("Recovery copies that are not discarded are removed "
               "automatically after %1 days.")
                .arg(MaxAgeDays));
  if (m_hasCrashReport)
    sl.append(tr("A crash report was saved. Click 'Crash Report' to view it."));
  m_introLabel->setText(sl.join("<br><br>"));

  QSignalBlocker blocker(m_list);
  const int horizontal = m_list->horizontalScrollBar()->value();
  const int vertical   = m_list->verticalScrollBar()->value();
  m_list->setHeaderLabels(QStringList() << tr("Scene") << tr("Saved")
                                        << tr("Original Location"));
  for (int row = 0; row < m_list->topLevelItemCount(); ++row)
    retranslateItem(m_list->topLevelItem(row));
  m_list->doItemsLayout();
  m_list->horizontalScrollBar()->setValue(horizontal);
  m_list->verticalScrollBar()->setValue(vertical);
}

//-----------------------------------------------------------------------------

void RecoveryDialog::retranslateItem(QTreeWidgetItem *item) {
  const QString sceneName = item->data(0, SceneNameRole).toString();
  const QString originalScene = item->data(0, OriginalSceneRole).toString();
  const QStringList uncopiedLevels =
      item->data(0, UncopiedLevelsRole).toStringList();
  item->setText(0, sceneName);
  item->setToolTip(0, QString());
  if (!uncopiedLevels.isEmpty()) {
    item->setText(0, tr("%1 (incomplete)").arg(sceneName));
    QStringList levels;
    for (const QString &level : uncopiedLevels)
      levels.append(QDir::toNativeSeparators(level));
    item->setToolTip(
        0, tr("The unsaved changes of these levels could not be copied, and "
              "the copy uses their saved files:\n%1")
               .arg(levels.join("\n")));
  }
  item->setText(2, originalScene.isEmpty()
                       ? tr("Untitled scene")
                       : QDir::toNativeSeparators(originalScene));
  item->setToolTip(2, item->text(2));
}

//-----------------------------------------------------------------------------

void RecoveryDialog::addItem(const QString &sceneName, const QString &savedAt,
                             const QString &originalScene,
                             const QString &sceneFile,
                             const QStringList &folders,
                             const QStringList &uncopiedLevels) {
  QTreeWidgetItem *item = new QTreeWidgetItem(m_list);
  item->setText(1, savedAt);
  item->setData(0, Qt::UserRole, sceneFile);
  item->setData(0, Qt::UserRole + 1, folders);
  item->setData(0, SceneNameRole, sceneName);
  item->setData(0, OriginalSceneRole, originalScene);
  item->setData(0, UncopiedLevelsRole, uncopiedLevels);
  retranslateItem(item);
  if (!m_list->currentItem()) m_list->setCurrentItem(item);
  m_list->resizeColumnToContents(0);
  m_list->resizeColumnToContents(1);
}

//-----------------------------------------------------------------------------

void RecoveryDialog::onCurrentItemChanged() {
  const bool hasItem = m_list->currentItem() != 0;
  m_openButton->setEnabled(hasItem);
  m_folderButton->setEnabled(hasItem);
  m_discardButton->setEnabled(hasItem);
}

//-----------------------------------------------------------------------------

void RecoveryDialog::onOpen() {
  QTreeWidgetItem *item = m_list->currentItem();
  if (!item) return;
  m_sceneToOpen = item->data(0, Qt::UserRole).toString();
  accept();
}

//-----------------------------------------------------------------------------

void RecoveryDialog::onShowFolder() {
  QTreeWidgetItem *item = m_list->currentItem();
  if (!item) return;
  const QStringList folders = item->data(0, Qt::UserRole + 1).toStringList();
  if (!folders.isEmpty())
    QDesktopServices::openUrl(QUrl::fromLocalFile(folders.first()));
}

//-----------------------------------------------------------------------------

void RecoveryDialog::onDiscard() {
  QTreeWidgetItem *item = m_list->currentItem();
  if (!item) return;

  const QString question =
      tr("Delete the recovery copies of %1?\nThis cannot be undone.")
          .arg(item->text(0));
  if (DVGui::MsgBox(question, tr("Discard"), tr("Cancel"), 1, this) != 1)
    return;

  const QStringList folders = item->data(0, Qt::UserRole + 1).toStringList();
  for (const QString &folder : folders) removeSnapshotFolder(folder);

  delete item;
  if (m_list->topLevelItemCount() == 0) reject();
}

//-----------------------------------------------------------------------------

void RecoveryDialog::onCrashReport() {
  CrashHandler::showPendingReport(this);
  // The report is shown once; it stays in the reports folder
  m_crashReportButton->setEnabled(false);
}
