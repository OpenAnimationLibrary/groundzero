

#include "toonz/sceneresources.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/levelset.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshexrlevel.h"
#include "toonz/txshpalettelevel.h"
#include "toonz/levelproperties.h"
#include "toonz/txshsoundlevel.h"
#include "toonz/namebuilder.h"
#include "toonz/childstack.h"
#include "toonz/txsheet.h"
#include "toonz/preferences.h"
#include "toonz/particlecache.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfx.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "tpalette.h"
#include "tmacrofx.h"

#include "tmsgcore.h"

#include "tconvert.h"
#include "tlogger.h"
#include "tsystem.h"

#include <algorithm>
#include <set>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>

namespace {
TFilePath decodedFxPath(const ToonzScene *scene, const TFilePath &coded) {
  TFilePath actual = coded.isEmpty() ? coded : scene->decodeFilePath(coded);
  if (!actual.isEmpty() && !actual.isAbsolute())
    actual = scene->getScenePath().getParentDir() + actual;
  return actual;
}

// Use physical identity to share a cache imported by several FX nodes.
TFilePath physicalFxPath(const TFilePath &path) {
  const QFileInfo info(path.getQString());
  const QString canonical = info.canonicalFilePath();
  return TFilePath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

// Copy a complete particle asset to a fresh directory, then publish it with one
// rename. Destination names are generated here, never taken as relative paths
// from the input manifest. Thus absolute references and ../ in an external
// manifest may be read but cannot escape the new package on write.
bool copyParticlePackage(const TFilePath &source, const TFilePath &destination,
                         QString &error) {
  ParticleCache::Sequence sequence;
  if (!ParticleCache::openSequence(source.getQString(), sequence, error))
    return false;
  const QFileInfo target(destination.getQString());
  if (target.exists()) {
    error = QObject::tr("Particle cache destination already exists: %1")
                .arg(target.filePath());
    return false;
  }
  const QString packageDir = target.absolutePath();
  const QFileInfo packageInfo(packageDir);
  QDir parent(packageInfo.absolutePath());
  if (QFileInfo::exists(packageDir) || !parent.mkpath(".")) {
    error = QObject::tr("Cannot create particle cache directory: %1")
                .arg(packageDir);
    return false;
  }
  QTemporaryDir staging(parent.filePath(".particle-import-XXXXXX"));
  if (!staging.isValid()) {
    error = QObject::tr("Cannot stage particle cache import.");
    return false;
  }
  QDir stage(staging.path());
  if (!stage.mkpath("frames") || !stage.mkpath("artwork")) return false;

  QJsonObject manifest;
  if (!sequence.manifestPath.isEmpty()) {
    QFile original(sequence.manifestPath);
    if (!original.open(QIODevice::ReadOnly)) {
      error = original.errorString();
      return false;
    }
    const QJsonDocument json = QJsonDocument::fromJson(original.readAll());
    if (!json.isObject()) {
      error = QObject::tr("Invalid particle cache manifest.");
      return false;
    }
    manifest = json.object();
  }
  manifest["format"] = "opentoonz-particle-cache";
  manifest["version"] = 1;
  manifest["fps"] = sequence.metadata.framesPerSecond;
  manifest["firstFrame"] = sequence.metadata.firstFrame;
  manifest["lastFrame"] = sequence.metadata.lastFrame;
  manifest["units"] = sequence.metadata.units;
  manifest["mapping"] = ParticleCache::mappingToJson(sequence.mapping);
  manifest["extra"] = sequence.metadata.extra;

  std::map<TFilePath, QString> copied;
  std::map<QString, TFilePath> copiedDestinations;
  const QDir sourceRoot(QFileInfo(sequence.manifestPath).absolutePath());
  auto copyFile = [&](const QString &file, const QString &folder,
                      bool artwork, QString &relative) {
    const TFilePath identity = physicalFxPath(TFilePath(file));
    // Native draw sidecars refer to sprites relative to the package root. Keep
    // the entire internal layout, while remapping genuinely external assets.
    const QString originalRelative = QDir::cleanPath(
        sourceRoot.relativeFilePath(QFileInfo(file).absoluteFilePath()));
    const bool internal = !sequence.manifestPath.isEmpty() &&
                          !QDir::isAbsolutePath(originalRelative) &&
                          originalRelative != ".." &&
                          !originalRelative.startsWith("../");
    const auto previous = copied.find(identity);
    if (!internal && previous != copied.end()) {
      relative = previous->second;
      return true;
    }
    const QString name = QFileInfo(file).fileName();
    if (name.isEmpty() || name == "." || name == "..") {
      error = QObject::tr("Invalid particle asset filename.");
      return false;
    }
    relative = internal ? originalRelative
                        : folder + "/" + QString::number(copied.size()) + "_" + name;
    if (relative == target.fileName()) {
      error = QObject::tr("Particle asset conflicts with its manifest filename.");
      return false;
    }
    const auto existingDestination = copiedDestinations.find(relative);
    if (existingDestination != copiedDestinations.end()) {
      if (existingDestination->second == identity) return true;
      error = QObject::tr("Particle assets have conflicting destination names.");
      return false;
    }
    const QString output = stage.filePath(relative);
    if (!stage.mkpath(QFileInfo(relative).path())) {
      error = QObject::tr("Cannot create particle asset directory.");
      return false;
    }
    if (artwork) {
      try {
        if (!TSystem::doesExistFileOrLevel(TFilePath(file))) {
          error = QObject::tr("Missing particle artwork: %1").arg(file);
          return false;
        }
        TXshSimpleLevel::copyFiles(TFilePath(output), TFilePath(file));
        if (!TSystem::doesExistFileOrLevel(TFilePath(output))) {
          error = QObject::tr("Failed to copy particle artwork: %1").arg(file);
          return false;
        }
      } catch (...) {
        error = QObject::tr("Failed to copy particle artwork: %1").arg(file);
        return false;
      }
    } else if (!QFile::copy(file, output)) {
      error = QObject::tr("Failed to copy particle cache frame: %1").arg(file);
      return false;
    }
    copied.emplace(identity, relative);
    copiedDestinations.emplace(relative, identity);
    return true;
  };

  QJsonArray frames;
  for (const auto &frame : sequence.frames) {
    QString relative;
    if (!copyFile(frame.second, "frames", false, relative)) return false;
    QJsonObject item;
    item["frame"] = frame.first;
    item["file"] = relative;
    const auto metadata = sequence.frameMetadata.find(frame.first);
    if (metadata != sequence.frameMetadata.end()) item["extra"] = metadata->second;
    frames.append(item);
  }
  manifest["frames"] = frames;
  QJsonArray artwork;
  for (const QString &path : sequence.metadata.artworkPaths) {
    QString relative;
    if (!copyFile(path, "artwork", true, relative)) return false;
    artwork.append(relative);
  }
  manifest["artwork"] = artwork;
  const QString stagedManifest = stage.filePath(target.fileName());
  QSaveFile output(stagedManifest);
  const QByteArray data = QJsonDocument(manifest).toJson();
  if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() ||
      !output.commit()) {
    error = output.errorString();
    return false;
  }
  ParticleCache::Sequence verify;
  if (!ParticleCache::openSequence(stagedManifest, verify, error)) return false;
  if (!parent.rename(staging.path(), packageDir)) {
    error = QObject::tr("Cannot publish particle cache directory: %1")
                .arg(packageDir);
    return false;
  }
  staging.setAutoRemove(false);
  return true;
}

TFilePath newParticlePackagePath(ToonzScene *scene, const TFilePath &source) {
  QFileInfo file(source.getQString());
  QString name = file.suffix() == "otpc"
                     ? QFileInfo(file.absolutePath()).fileName()
                     : file.completeBaseName();
  // Package names need no source directories, sequence separators or aliases.
  for (QChar &c : name)
    if (!c.isLetterOrNumber() && c != '_' && c != '-') c = '_';
  if (name.isEmpty()) name = "particles";
  const TFilePath root = TFilePath("+extras") + "particlecaches";
  TFilePath directory = root + name.toStdWString();
  for (int suffix = 2;
       QFileInfo::exists(scene->decodeFilePath(directory).getQString()); ++suffix)
    directory = root + (name + "_" + QString::number(suffix)).toStdWString();
  return directory + "cache.otpc";
}

//=============================================================================

class ScenePathRestorer {
  ToonzScene *m_scene;
  TFilePath m_oldPath;

public:
  ScenePathRestorer(ToonzScene *scene, const TFilePath &oldPath)
      : m_scene(scene), m_oldPath(oldPath) {}

  ~ScenePathRestorer() { m_scene->setScenePath(m_oldPath, false); }
};

//-----------------------------------------------------------------------------

// se path e' della forma +folder/<oldSavePath>/name.type
// allora sostituisce oldSavePath con newSavePath e ritorna true

bool changeSavePath(TFilePath &path, TFilePath oldSavePath,
                    TFilePath newSavePath) {
  if (oldSavePath == newSavePath) return false;
  TFilePath fp = path.getParentDir();
  std::wstring head;
  TFilePath tail;
  fp.split(head, tail);
  if (head != L"" && tail == oldSavePath) {
    path = path.withParentDir(TFilePath(head) + newSavePath);
    return true;
  } else
    return false;
}

//-----------------------------------------------------------------------------

// From ../../../filename#type.psd to ../../../filename.psd
TFilePath restorePsdPath(const TFilePath &fp) {
  QString path = QString::fromStdWString(fp.getWideString());
  if (fp.getType() != "psd" || !path.contains("#")) return fp;
  int from = path.indexOf("#");
  int to   = path.lastIndexOf(".");
  path.remove(from, to - from);
  return TFilePath(path.toStdWString());
}

//-----------------------------------------------------------------------------

bool makePathUnique(ToonzScene *scene, TFilePath &path) {
  std::wstring name = path.getWideName();
  int id            = 2;
  int i             = name.length() - 1;
  int num = 0, p = 1;
  while (i >= 0 && L'0' <= name[i] && name[i] <= L'9') {
    num += p * (name[i] - L'0');
    p *= 10;
    i--;
  }
  if (i >= 0 && name[i] == L'_') {
    id   = num + 1;
    name = name.substr(0, i);
  }

  bool ret = false;
  while (TSystem::doesExistFileOrLevel(scene->decodeFilePath(path))) {
    ret  = true;
    path = path.withName(name + L"_" + std::to_wstring(id));
    id++;
  }
  return ret;
}

//-----------------------------------------------------------------------------

bool getCollectedPath(ToonzScene *scene, TFilePath &path) {
  if (!path.isAbsolute() || path.getWideString()[0] == L'+') return false;

  TFilePath collectedPath = scene->getImportedLevelPath(path);
  if (path == collectedPath) return false;

  TFilePath actualCollectedPath = scene->decodeFilePath(collectedPath);

  if (makePathUnique(scene, actualCollectedPath))
    collectedPath = collectedPath.withName(actualCollectedPath.getName());

  path = collectedPath;
  return true;
}

// Return a frame-independent key for a physical image source.  Dedicated EXR
// levels intentionally share one file/sequence while selecting different
// parts or layers, so their resource identity cannot be the TXshLevel object.
TFilePath physicalLevelPath(ToonzScene *scene, const TFilePath &path) {
  const TFilePath actual = scene ? scene->decodeFilePath(path) : path;
  return actual.getParentDir() + TFilePath(actual.getLevelNameW());
}

// One scene resource represents all of the dedicated EXR levels backed by the
// same physical document.  The first level performs filesystem work; path
// changes are then applied to every sibling without touching its EXR selector.
class SharedExrSceneLevel final : public SceneResource {
  std::vector<TXshExrLevel *> m_levels;
  std::vector<TFilePath> m_oldPaths;
  SceneLevel m_representative;

  TXshExrLevel *representative() const { return m_levels.front(); }

  void propagatePath(const TFilePath &path, bool retainCachedImages) {
    for (TXshExrLevel *level : m_levels) {
      if (level->getPath() != path) level->setPath(path, retainCachedImages);
    }
  }

public:
  SharedExrSceneLevel(ToonzScene *scene, TXshExrLevel *level)
      : SceneResource(scene), m_representative(scene, level) {
    addLevel(level);
  }

  void addLevel(TXshExrLevel *level) {
    m_levels.push_back(level);
    m_oldPaths.push_back(level->getPath());
  }

  bool save() override {
    // SceneLevel contains the established Save As/path-remapping behavior.
    // Running it once avoids a second sibling trying to copy over the file
    // that the first sibling has just created.
    const bool saved = m_representative.save();
    if (saved) propagatePath(representative()->getPath(), false);
    return saved;
  }

  void updatePath() override {
    m_representative.updatePath();
    propagatePath(representative()->getPath(), true);
  }

  void rollbackPath() override {
    m_representative.rollbackPath();
    if (!m_untitledScene) return;

    for (std::size_t i = 1; i < m_levels.size(); ++i) {
      if (m_levels[i]->getPath() != m_oldPaths[i])
        m_levels[i]->setPath(m_oldPaths[i], true);
    }
  }

  void accept(ResourceProcessor *processor) override {
    // Importing or collecting performs physical file work.  Process the
    // document once, then give every selected part/layer the resulting coded
    // path.  Other visitors may have per-level side effects (for example,
    // moving cast entries between scenes), so retain their original behavior.
    const bool sharedPhysicalOperation =
        dynamic_cast<ResourceImporter *>(processor) != nullptr ||
        dynamic_cast<ResourceCollector *>(processor) != nullptr;
    if (!sharedPhysicalOperation) {
      for (TXshExrLevel *level : m_levels) processor->process(level);
      return;
    }

    TXshExrLevel *source = representative();
    if (dynamic_cast<ResourceImporter *>(processor)) {
      // Import operates on coded/project-relative paths. If legacy and
      // dedicated levels refer to the same decoded source using different
      // spellings, do not let an absolute representative suppress import.
      const auto coded = std::find_if(m_levels.begin(), m_levels.end(),
                                      [](const TXshExrLevel *level) {
                                        return !level->getPath().isAbsolute();
                                      });
      if (coded != m_levels.end()) source = *coded;
    } else if (dynamic_cast<ResourceCollector *>(processor)) {
      // Collection converts external absolute paths into project paths.
      const auto external = std::find_if(m_levels.begin(), m_levels.end(),
                                         [](const TXshExrLevel *level) {
                                           return level->getPath().isAbsolute();
                                         });
      if (external != m_levels.end()) source = *external;
    }
    processor->process(source);
    propagatePath(source->getPath(), false);
    for (TXshExrLevel *level : m_levels)
      if (level != source) level->setDirtyFlag(source->getDirtyFlag());
  }

  bool isDirty() override {
    for (TXshExrLevel *level : m_levels) {
      if (level->getProperties()->getDirtyFlag() ||
          (level->getPalette() && level->getPalette()->getDirtyFlag()))
        return true;
    }
    return false;
  }

  QStringList getResourceName() override {
    return m_representative.getResourceName();
  }
};

// One entry owns the path transaction for every FX sharing this physical cache.
// Source files are never removed by rollback; only parameters are restored.
class SharedParticleCacheResource final : public SceneResource {
  std::vector<SceneFxResource *> m_effects;
  std::vector<TFilePath> m_oldPaths;
  TFilePath m_oldActualPath;
  TFilePath m_savedPath;

  void setPath(const TFilePath &path) {
    for (SceneFxResource *effect : m_effects) effect->setResourcePath(path);
  }

public:
  SharedParticleCacheResource(ToonzScene *scene, SceneFxResource *effect)
      : SceneResource(scene)
      , m_oldActualPath(decodedFxPath(scene, effect->resourcePath())) {
    add(effect);
  }

  void add(SceneFxResource *effect) {
    m_effects.push_back(effect);
    m_oldPaths.push_back(effect->resourcePath());
  }

  bool save() override {
    TFilePath path = m_oldPaths.front();
    SceneResource::updatePath(path);
    if (decodedFxPath(m_scene, path) != m_oldActualPath) {
      const TFilePath destination = newParticlePackagePath(m_scene, m_oldActualPath);
      QString error;
      if (!copyParticlePackage(m_oldActualPath,
                               m_scene->decodeFilePath(destination), error)) {
        TLogger::error() << error.toStdString();
        return false;
      }
      path = destination;
    }
    m_savedPath = path;
    setPath(path);
    return true;
  }

  void updatePath() override {
    if (!m_savedPath.isEmpty()) {
      setPath(m_savedPath);
      return;
    }
    TFilePath path = m_effects.front()->resourcePath();
    SceneResource::updatePath(path);
    setPath(path);
  }

  void rollbackPath() override {
    for (std::size_t i = 0; i < m_effects.size(); ++i)
      m_effects[i]->setResourcePath(m_oldPaths[i]);
  }

  void accept(ResourceProcessor *processor) override {
    processor->process(m_effects.front());
    setPath(m_effects.front()->resourcePath());
  }

  bool isDirty() override { return false; }
  QStringList getResourceName() override {
    return QStringList(m_effects.front()->resourcePath().getQString());
  }
};

void gatherFxResources(TFx *fx, std::set<TFx *> &visited,
                       std::vector<SceneFxResource *> &resources) {
  if (!fx || !visited.insert(fx).second) return;
  if (auto *resource = dynamic_cast<SceneFxResource *>(fx))
    if (!resource->resourcePath().isEmpty()) resources.push_back(resource);
  if (auto *column = dynamic_cast<TZeraryColumnFx *>(fx))
    gatherFxResources(column->getZeraryFx(), visited, resources);
  if (auto *macro = dynamic_cast<TMacroFx *>(fx))
    for (const TFxP &inner : macro->getFxs())
      gatherFxResources(inner.getPointer(), visited, resources);
  for (int i = 0; i < fx->getInputPortCount(); ++i)
    gatherFxResources(fx->getInputPort(i)->getFx(), visited, resources);
}

void gatherXsheetResources(TXsheet *xsheet, std::set<TXsheet *> &sheets,
                           std::set<TFx *> &effects,
                           std::vector<SceneFxResource *> &resources) {
  if (!xsheet || !sheets.insert(xsheet).second) return;
  std::vector<TFx *> fxs;
  xsheet->getFxDag()->getFxs(fxs);
  for (TFx *fx : fxs) gatherFxResources(fx, effects, resources);
  for (int column = 0; column < xsheet->getColumnCount(); ++column)
    if (TXshColumn *item = xsheet->getColumn(column))
      gatherFxResources(item->getFx(), effects, resources);
  std::set<TXshLevel *> levels;
  xsheet->getUsedLevels(levels);
  for (TXshLevel *level : levels)
    if (TXshChildLevel *child = level->getChildLevel())
      gatherXsheetResources(child->getXsheet(), sheets, effects, resources);
}

}  // namespace

void resolveSceneFxResources(TFx *fx, const ToonzScene *scene) {
  if (!fx || !scene) return;
  if (auto *resource = dynamic_cast<SceneFxResource *>(fx)) {
    const TFilePath coded = resource->resourcePath();
    const TFilePath actual = decodedFxPath(scene, coded);
    resource->setResolvedResourcePath(actual);
  }
  if (auto *macro = dynamic_cast<TMacroFx *>(fx))
    for (const TFxP &inner : macro->getFxs())
      resolveSceneFxResources(inner.getPointer(), scene);
}

void makeSceneFxResourcesAbsolute(TFx *fx, const ToonzScene *scene) {
  if (!fx || !scene) return;
  if (auto *resource = dynamic_cast<SceneFxResource *>(fx))
    resource->setResourcePath(decodedFxPath(scene, resource->resourcePath()));
  if (auto *column = dynamic_cast<TZeraryColumnFx *>(fx))
    makeSceneFxResourcesAbsolute(column->getZeraryFx(), scene);
  if (auto *macro = dynamic_cast<TMacroFx *>(fx))
    for (const TFxP &inner : macro->getFxs())
      makeSceneFxResourcesAbsolute(inner.getPointer(), scene);
}

//=============================================================================
//
// ResourceImportStrategy
//
//-----------------------------------------------------------------------------

ResourceImportStrategy::ResourceImportStrategy(int strategy)
    : m_childFolderEnabled(false), m_strategy(strategy) {
  setChildFolderEnabled(Preferences::instance()->isSubsceneFolderEnabled());
}

//-----------------------------------------------------------------------------

TFilePath ResourceImportStrategy::process(ToonzScene *scene,
                                          ToonzScene *srcScene,
                                          TFilePath srcPath) {
  TFilePath srcActualPath = srcScene->decodeFilePath(srcPath);
  if (!scene->isExternPath(srcActualPath) || m_strategy == DONT_IMPORT)
    return srcPath;

  TFilePath dstPath;
  if (srcPath.getWideString().find(L'+') == 0)
    dstPath = srcPath;
  else
    dstPath = scene->getImportedLevelPath(srcPath);
  TFilePath actualDstPath = scene->decodeFilePath(dstPath);
  assert(actualDstPath != TFilePath());

  if (m_strategy == IMPORT_AND_OVERWRITE) {
    // bool overwritten = false;
    if (TSystem::doesExistFileOrLevel(actualDstPath)) {
      TSystem::removeFileOrLevel(actualDstPath);
      //  overwritten = true;
    }
    if (TSystem::doesExistFileOrLevel(srcPath))
      TXshSimpleLevel::copyFiles(actualDstPath, srcPath);

    return dstPath;
  } else if (m_strategy == IMPORT_AND_RENAME) {
    std::wstring levelName    = srcPath.getWideName();
    TLevelSet *parentLevelSet = scene->getLevelSet();
    NameModifier nm(levelName);
    std::wstring newName;
    for (;;) {
      newName = nm.getNext();
      if (!parentLevelSet->hasLevel(newName)) break;
    }

    dstPath       = dstPath.withName(newName);
    actualDstPath = scene->decodeFilePath(dstPath);

    if (TSystem::doesExistFileOrLevel(actualDstPath))
      TSystem::removeFileOrLevel(actualDstPath);

    if (TSystem::doesExistFileOrLevel(srcActualPath)) {
      TXshSimpleLevel::copyFiles(actualDstPath, srcActualPath);
    }
    return dstPath;
  }
  return srcPath;
}

//=============================================================================
//
// SceneResource
//
//-----------------------------------------------------------------------------

SceneResource::SceneResource(ToonzScene *scene)
    : m_scene(scene)
    , m_untitledScene(scene->isUntitled())
    , m_oldSavePath(scene->getSavePath()) {}

//-----------------------------------------------------------------------------

SceneResource::~SceneResource() {}

//-----------------------------------------------------------------------------

void SceneResource::updatePath(TFilePath &fp) {
  if (m_untitledScene)
    changeSavePath(fp, m_oldSavePath, m_scene->getSavePath());
}

//=============================================================================
//
// SceneLevel
//
//-----------------------------------------------------------------------------

SceneLevel::SceneLevel(ToonzScene *scene, TXshSimpleLevel *sl)
    : SceneResource(scene)
    , m_sl(sl)
    , m_oldPath(sl->getPath())
    , m_oldActualPath(scene->decodeFilePath(sl->getPath()))
    , m_oldScannedPath(sl->getScannedPath())
    , m_oldRefImgPath()
    , m_oldActualRefImgPath() {
  if (m_oldScannedPath != TFilePath())
    m_oldActualScannedPath = m_scene->decodeFilePath(m_oldScannedPath);
  if ((sl->getPath().getType() == "tlv" || sl->getPath().getType() == "pli") &&
      sl->getPalette()) {
    m_oldRefImgPath       = sl->getPalette()->getRefImgPath();
    m_oldActualRefImgPath = m_scene->decodeFilePath(m_oldRefImgPath);
  }
}

//-----------------------------------------------------------------------------

bool SceneLevel::save() {
  bool saveSucceeded = true;
  TFilePath fp       = m_oldPath;
  SceneResource::updatePath(fp);
  TFilePath actualFp      = m_scene->decodeFilePath(fp);
  actualFp                = restorePsdPath(actualFp);
  TFilePath oldActualPath = restorePsdPath(m_oldActualPath);
  assert(actualFp.getWideString() == L"" ||
         actualFp.getWideString()[0] != L'+');
  if (actualFp != oldActualPath ||
      !TSystem::doesExistFileOrLevel(oldActualPath) ||
      m_sl->getProperties()->getDirtyFlag() ||
      (m_sl->getPalette() && m_sl->getPalette()->getDirtyFlag())) {
    try {
      TSystem::touchParentDir(actualFp);
      if (actualFp != oldActualPath &&
          TSystem::doesExistFileOrLevel(oldActualPath) &&
          m_sl->getProperties()->getDirtyFlag() == false &&
          (!m_sl->getPalette() ||
           (m_sl->getPalette() &&
            m_sl->getPalette()->getDirtyFlag() == false))) {
        TXshSimpleLevel::copyFiles(actualFp, oldActualPath);
        // Must NOT KEEP FRAMES, it generate a level frames bind necessary to
        // imageBuilder path refresh.
        m_sl->setPath(fp, false);
      } else {
        try {
          m_sl->save(actualFp, oldActualPath);
        } catch (...) {
          throw;
        }
        if ((actualFp.getType() == "tlv" || actualFp.getType() == "pli") &&
            actualFp != oldActualPath && m_oldRefImgPath != TFilePath()) {
          // Devo preoccuparmi dell'eventuale livello colormodel
          TFilePath actualRefImagPath =
              m_scene->decodeFilePath(m_oldRefImgPath);
          TFilePath actualRefImagPathTpl = actualRefImagPath.withType("tpl");
          TFilePath oldRefImagPathTpl = m_oldActualRefImgPath.withType("tpl");
          TSystem::copyFile(actualRefImagPath, m_oldActualRefImgPath);
          if (actualRefImagPath.getType() == "tlv")
            TSystem::copyFile(actualRefImagPathTpl, oldRefImagPathTpl);
        }

        if (actualFp.getType() == "tif" || actualFp.getType() == "tiff" ||
            actualFp.getType() == "tga" || actualFp.getType() == "tzi") {
          TFilePath clnin = oldActualPath.withNoFrame().withType("cln");
          if (TSystem::doesExistFileOrLevel(clnin))
            TSystem::copyFile(actualFp.withNoFrame().withType("cln"), clnin);
        }
      }
      // Se il livello e' tlv verifico se esiste il corrispondente unpainted ed
      // in caso affermativo lo copio.
      // Questo controllo viene fatto qui e non nella copia o nel salvataggio
      // del livello perche' in generale
      // non si vuole che il livello unpainted venga copiato con il livello.
      if (actualFp.getType() == "tlv") {
        TFilePath oldUnpaintedLevelPath =
            oldActualPath.getParentDir() + "nopaint\\" +
            TFilePath(oldActualPath.getName() + "_np." +
                      oldActualPath.getType());
        TFilePath unpaintedLevelPath =
            actualFp.getParentDir() + "nopaint\\" +
            TFilePath(actualFp.getName() + "_np." + actualFp.getType());
        if (TSystem::doesExistFileOrLevel(oldUnpaintedLevelPath) &&
            !TSystem::doesExistFileOrLevel(unpaintedLevelPath) &&
            TSystem::touchParentDir(unpaintedLevelPath))
          TSystem::copyFile(unpaintedLevelPath, oldUnpaintedLevelPath);
        TFilePath oldUnpaintedPalettePath =
            oldUnpaintedLevelPath.withType("tpl");
        TFilePath unpaintedPalettePath = unpaintedLevelPath.withType("tpl");
        if (TSystem::doesExistFileOrLevel(oldUnpaintedPalettePath) &&
            !TSystem::doesExistFileOrLevel(unpaintedPalettePath) &&
            TSystem::touchParentDir(unpaintedPalettePath))
          TSystem::copyFile(unpaintedPalettePath, oldUnpaintedPalettePath);
      }
    } catch (...) {
      saveSucceeded = false;
    }
  }
  fp = m_oldScannedPath;
  if (fp != TFilePath()) {
    SceneResource::updatePath(fp);
    actualFp = m_scene->decodeFilePath(fp);
    if (actualFp != m_oldActualScannedPath &&
        TSystem::doesExistFileOrLevel(m_oldActualScannedPath)) {
      try {
        TSystem::touchParentDir(actualFp);
        TSystem::copyFileOrLevel_throw(actualFp, m_oldActualScannedPath);
        m_sl->clearFrames();
        m_sl->load();
      } catch (...) {
        saveSucceeded = false;
      }
    }
  }
  return saveSucceeded;
}

//-----------------------------------------------------------------------------

void SceneLevel::updatePath() {
  if (!m_untitledScene) return;
  TFilePath fp = m_oldPath;
  SceneResource::updatePath(fp);
  m_sl->setPath(fp, true);
  fp = m_oldScannedPath;
  SceneResource::updatePath(fp);
  m_sl->setScannedPath(fp);
}

//-----------------------------------------------------------------------------

void SceneLevel::rollbackPath() {
  if (!m_untitledScene) return;
  m_sl->setPath(m_oldPath, true);
  m_sl->setScannedPath(m_oldScannedPath);
}

//-----------------------------------------------------------------------------

bool SceneLevel::isDirty() {
  if (m_sl->getProperties()->getDirtyFlag() ||
      (m_sl->getPalette() && m_sl->getPalette()->getDirtyFlag()))
    return true;
  else
    return false;
}
//-----------------------------------------------------------------------------

QStringList SceneLevel::getResourceName() {
  QStringList ret;
  QString string;
  bool levelIsDirty = false;
  if (m_sl->getProperties()->getDirtyFlag()) {
    string += QString::fromStdString(m_sl->getPath().getLevelName());
    levelIsDirty = true;
  }
  if (m_sl->getPalette() && m_sl->getPalette()->getDirtyFlag()) {
    QString paletteName =
        QString::fromStdWString(m_sl->getPalette()->getPaletteName());
    if (m_sl->getType() & FULLCOLOR_TYPE) {
      if (levelIsDirty) ret << string;
      ret << paletteName + ".tpl";
    } else {
      if (levelIsDirty) string += " and ";
      if (m_sl->getPath().getType() == "pli")
        string += paletteName + ".pli (palette)";
      else
        string += paletteName + ".tpl";
      ret << string;
    }
  } else if (levelIsDirty)
    ret << string;

  if (ret.isEmpty())
    ret << QString::fromStdString(m_sl->getPath().getLevelName());
  return ret;
}

//=============================================================================
//
// ScenePalette
//
//-----------------------------------------------------------------------------

ScenePalette::ScenePalette(ToonzScene *scene, TXshPaletteLevel *pl)
    : SceneResource(scene)
    , m_pl(pl)
    , m_oldPath(pl->getPath())
    , m_oldActualPath(scene->decodeFilePath(pl->getPath())) {}

//-----------------------------------------------------------------------------

bool ScenePalette::save() {
  assert(m_oldPath != TFilePath());
  TFilePath fp = m_oldPath;
  SceneResource::updatePath(fp);
  TFilePath actualFp = m_scene->decodeFilePath(fp);
  try {
    TSystem::touchParentDir(actualFp);
    if (actualFp != m_oldActualPath &&
        TSystem::doesExistFileOrLevel(m_oldActualPath)) {
      TSystem::copyFile(actualFp, m_oldActualPath);
    }
    m_pl->save();  // actualFp non so perche' era cosi'
    return true;
  } catch (...) {
    TLogger::error() << "Can't save " << actualFp;
    return false;
  }
}

//-----------------------------------------------------------------------------

void ScenePalette::updatePath() {
  TFilePath fp = m_oldPath;
  SceneResource::updatePath(fp);
  if (fp != m_oldPath) m_pl->setPath(fp);
}

//-----------------------------------------------------------------------------

void ScenePalette::rollbackPath() { m_pl->setPath(m_oldPath); }

//-----------------------------------------------------------------------------

bool ScenePalette::isDirty() {
  return m_pl->getPalette() && m_pl->getPalette()->getDirtyFlag();
}

//-----------------------------------------------------------------------------

QStringList ScenePalette::getResourceName() {
  return QStringList(QString::fromStdString(m_pl->getPath().getLevelName()));
}

//=============================================================================
//
// SceneSound
//
//-----------------------------------------------------------------------------

SceneSound::SceneSound(ToonzScene *scene, TXshSoundLevel *sl)
    : SceneResource(scene)
    , m_sl(sl)
    , m_oldPath(sl->getPath())
    , m_oldActualPath(scene->decodeFilePath(sl->getPath())) {}

//-----------------------------------------------------------------------------

bool SceneSound::save() {
  assert(m_oldPath != TFilePath());
  TFilePath fp = m_oldPath;
  SceneResource::updatePath(fp);
  TFilePath actualFp = m_scene->decodeFilePath(fp);
  try {
    TSystem::touchParentDir(actualFp);
    if (!TSystem::doesExistFileOrLevel(m_oldActualPath)) {
      m_sl->save(actualFp);
    } else if (actualFp != m_oldActualPath) {
      TSystem::copyFile(actualFp, m_oldActualPath);
    }
    return true;
  } catch (...) {
    TLogger::error() << "Can't save " << actualFp;
    return false;
  }
}

//-----------------------------------------------------------------------------

void SceneSound::updatePath() {
  TFilePath fp = m_oldPath;
  SceneResource::updatePath(fp);
  if (fp != m_oldPath) m_sl->setPath(fp);
}

//-----------------------------------------------------------------------------

void SceneSound::rollbackPath() { m_sl->setPath(m_oldPath); }

//-----------------------------------------------------------------------------

QStringList SceneSound::getResourceName() {
  return QStringList(
      QString::fromStdString(m_sl->getPath().getLevelName()));
}

//=============================================================================
//
// SceneResources
//
//-----------------------------------------------------------------------------

SceneResources::SceneResources(ToonzScene *scene, TXsheet *subXsheet)
    : m_scene(scene)
    , m_commitDone(false)
    , m_wasUntitled(scene->isUntitled())
    , m_subXsheet(subXsheet) {
  getResources();
}

//-----------------------------------------------------------------------------

SceneResources::~SceneResources() {
  if (!m_commitDone) rollbackPaths();
  clearPointerContainer(m_resources);
}

//-----------------------------------------------------------------------------

void SceneResources::getResources() {
  ToonzScene *scene = m_scene;
  std::vector<TXshLevel *> levels;
  scene->getLevelSet()->listLevels(levels);
  std::vector<TXshLevel *>::iterator it;
  std::map<TFilePath, SharedExrSceneLevel *> sharedExrResources;

  for (it = levels.begin(); it != levels.end(); ++it) {
    TXshSimpleLevel *sl = (*it)->getSimpleLevel();
    if (sl) {
      if (auto *exrLevel = dynamic_cast<TXshExrLevel *>(sl);
          exrLevel && !exrLevel->getPath().isEmpty()) {
        const TFilePath key = physicalLevelPath(scene, exrLevel->getPath());
        auto shared         = sharedExrResources.find(key);
        if (shared == sharedExrResources.end()) {
          auto *resource          = new SharedExrSceneLevel(scene, exrLevel);
          sharedExrResources[key] = resource;
          m_resources.push_back(resource);
        } else {
          shared->second->addLevel(exrLevel);
        }
      } else {
        m_resources.push_back(new SceneLevel(scene, sl));
      }
      continue;
    }
    TXshPaletteLevel *pl = (*it)->getPaletteLevel();
    if (pl) { 
        m_resources.push_back(new ScenePalette(scene, pl));
        continue; 
    }
    TXshSoundLevel *sdl = (*it)->getSoundLevel();
    if (sdl) {
        m_resources.push_back(new SceneSound(scene, sdl));
        continue;
    }
  }

  // Particle caches live on FX nodes rather than in the cast. Include detached
  // internal nodes, macros, zerary FX columns and nested xsheets exactly once.
  std::set<TXsheet *> sheets;
  std::set<TFx *> effects;
  std::vector<SceneFxResource *> fxResources;
  gatherXsheetResources(m_subXsheet ? m_subXsheet : scene->getXsheet(),
                        sheets, effects, fxResources);
  if (!m_subXsheet)
    for (TXshLevel *level : levels)
      if (TXshChildLevel *child = level->getChildLevel())
        gatherXsheetResources(child->getXsheet(), sheets, effects, fxResources);

  std::map<TFilePath, SharedParticleCacheResource *> sharedCaches;
  for (SceneFxResource *effect : fxResources) {
    const TFilePath actual = decodedFxPath(scene, effect->resourcePath());
    ParticleCache::Sequence sequence;
    QString error;
    TFilePath key = physicalFxPath(actual);
    if (ParticleCache::openSequence(actual.getQString(), sequence, error)) {
      if (!sequence.manifestPath.isEmpty())
        key = physicalFxPath(TFilePath(sequence.manifestPath));
      else if (!sequence.frames.empty())
        key = physicalFxPath(TFilePath(sequence.frames.begin()->second));
    }
    const auto found = sharedCaches.find(key);
    if (found == sharedCaches.end()) {
      auto *resource = new SharedParticleCacheResource(scene, effect);
      sharedCaches[key] = resource;
      m_resources.push_back(resource);
    } else {
      found->second->add(effect);
    }
  }
}

//-----------------------------------------------------------------------------

bool SceneResources::save(const TFilePath newScenePath,
                          bool showWarnings) {
  TFilePath oldScenePath = m_scene->getScenePath();
  ScenePathRestorer restorePath(m_scene, oldScenePath);
  // TODO: The save path of resources should not be decided by changing scene's property,
  // refactor and remove setScenePath
  m_scene->setScenePath(newScenePath, false);

  bool saveSucceeded = true;
  QStringList failedList;
  for (int i = 0; i < (int)m_resources.size(); i++) {
    if (!m_resources[i]->save()) {
      saveSucceeded = false;
      failedList << m_resources[i]->getResourceName();
    }
  }

  getDirtyResources(failedList);
  if (!failedList.isEmpty()) saveSucceeded = false;

  if (showWarnings && !saveSucceeded) {
    failedList.removeDuplicates();
    // show up to 5 items
    int extraCount = failedList.count() - 5;
    if (extraCount > 0) {
      failedList = failedList.mid(0, 5);
      failedList.append(QObject::tr("and %1 more item(s).").arg(extraCount));
    }

    DVGui::warning(QObject::tr("Failed to save the following resources:\n") +
                   "  " + failedList.join("\n  "));
  }
  return saveSucceeded;
}

//-----------------------------------------------------------------------------

void SceneResources::updatePaths() {
  for (int i = 0; i < (int)m_resources.size(); i++)
    m_resources[i]->updatePath();
}

//-----------------------------------------------------------------------------

void SceneResources::rollbackPaths() {
  for (int i = 0; i < (int)m_resources.size(); i++)
    m_resources[i]->rollbackPath();
}

//-----------------------------------------------------------------------------

void SceneResources::accept(ResourceProcessor *processor, bool autoCommit) {
  for (int i = 0; i < (int)m_resources.size() && !processor->aborted(); i++)
    m_resources[i]->accept(processor);
  if (autoCommit && !processor->aborted()) commit();
  if (processor->aborted()) rollbackPaths();
}

//-----------------------------------------------------------------------------
// return the name list of dirty resources
void SceneResources::getDirtyResources(QStringList &dirtyResources) {
  for (SceneResource *resource : m_resources)
    if (resource->isDirty()) {
      dirtyResources << resource->getResourceName();
    }
  dirtyResources.removeDuplicates();
}

//=============================================================================
//
// ResourceImporter
//
//-----------------------------------------------------------------------------

ResourceImporter::ResourceImporter(ToonzScene *scene, std::shared_ptr<TProject> dstProject,
                                   ResourceImportStrategy &importStrategy)
    : m_scene(scene)
    , m_dstProject(dstProject)
    , m_dstScene(new ToonzScene())
    , m_importStrategy(importStrategy) {
  m_dstScene->setProject(dstProject);
  // scene file may not be in the "+scenes" path for the sandbox project.
  // in such case, try to save as "+scenes/filename.tnz" on import.
  TFilePath relativeScenePath =
      scene->getScenePath() - scene->getProject()->getScenesPath();
  if (relativeScenePath.isAbsolute())
    relativeScenePath = scene->getScenePath().withoutParentDir();
  TFilePath newFp = dstProject->getScenesPath() + relativeScenePath;
  makeUnique(newFp);
  m_dstScene->setScenePath(newFp);
}

//-----------------------------------------------------------------------------

ResourceImporter::~ResourceImporter() { delete m_dstScene; }

//-----------------------------------------------------------------------------

bool ResourceImporter::makeUnique(TFilePath &path) {
  return makePathUnique(m_dstScene, path);
}

//-----------------------------------------------------------------------------

TFilePath ResourceImporter::getImportedScenePath() const {
  return m_dstScene->getScenePath();
}

//-----------------------------------------------------------------------------

TFilePath ResourceImporter::codePath(const TFilePath &oldPath,
                                     const TFilePath &newActualPath) {
  return oldPath.withName(newActualPath.getName());
}

//-----------------------------------------------------------------------------

std::string ResourceImporter::extractPsdSuffix(TFilePath &path) {
  if (path.getType() != "psd") return "";
  std::string name = path.getName();
  int i            = name.find("#");
  if (i == std::string::npos) return "";
  std::string suffix = name.substr(i);
  path               = path.withName(name.substr(0, i));
  return suffix;
}

//-----------------------------------------------------------------------------

TFilePath ResourceImporter::buildPsd(const TFilePath &basePath,
                                     const std::string &suffix) {
  return basePath.withName(basePath.getName() + suffix);
}

//-----------------------------------------------------------------------------

void ResourceImporter::process(TXshSimpleLevel *sl) {
  if (sl->getPath().isAbsolute()) return;
  TFilePath newPath;

  TFilePath slPath   = sl->getPath();
  std::string suffix = extractPsdSuffix(slPath);

  TFilePath imgRefPath;
  if (sl->getPalette()) imgRefPath = sl->getPalette()->getRefImgPath();
  newPath = m_importStrategy.process(m_dstScene, m_scene, slPath);
  if (imgRefPath != TFilePath() &&
      !m_dstScene->isExternPath(m_dstScene->decodeFilePath(imgRefPath)))
    m_importStrategy.process(m_dstScene, m_scene, imgRefPath);

  if (suffix != "") newPath = buildPsd(newPath, suffix);

  sl->setPath(newPath);
  if (sl->getScannedPath() != TFilePath()) {
    newPath =
        m_importStrategy.process(m_dstScene, m_scene, sl->getScannedPath());
    sl->setScannedPath(newPath);
  }
  sl->setDirtyFlag(false);
}

//-----------------------------------------------------------------------------

void ResourceImporter::process(TXshPaletteLevel *pl) {
  if (pl->getPath().isAbsolute()) return;
  TFilePath newPath;
  newPath = m_importStrategy.process(m_dstScene, m_scene, pl->getPath());
  pl->setPath(newPath);
}

//-----------------------------------------------------------------------------

void ResourceImporter::process(TXshSoundLevel *sl) {
  if (sl->getPath().isAbsolute()) return;
  TFilePath newPath;
  newPath = m_importStrategy.process(m_dstScene, m_scene, sl->getPath());
  sl->setPath(newPath);
}

void ResourceImporter::process(SceneFxResource *resource) {
  const TFilePath source = decodedFxPath(m_scene, resource->resourcePath());
  if (source.isEmpty() || aborted()) return;
  const TFilePath key = physicalFxPath(source);
  const auto found = m_importedFxResources.find(key);
  if (found != m_importedFxResources.end()) {
    resource->setResourcePath(found->second);
    return;
  }
  if (!m_importStrategy.importsFxResources() || !m_dstScene->isExternPath(source)) {
    // Recode against the receiving project, preserving the original physical
    // location when the user selected Load rather than Import.
    resource->setResourcePath(m_dstScene->codeFilePath(source));
    return;
  }
  const TFilePath target = newParticlePackagePath(m_dstScene, source);
  QString error;
  if (!copyParticlePackage(source, m_dstScene->decodeFilePath(target), error)) {
    m_fxFailed = true;
    DVGui::warning(QObject::tr("Failed to import particle cache:\n%1").arg(error));
    return;
  }
  m_importedFxResources[key] = target;
  resource->setResourcePath(target);
}

//=============================================================================
//
// ResourceCollector
//
//-----------------------------------------------------------------------------

ResourceCollector::ResourceCollector(ToonzScene *scene)
    : m_scene(scene), m_count(0) {}

//-----------------------------------------------------------------------------

ResourceCollector::~ResourceCollector() {}

//-----------------------------------------------------------------------------

bool ResourceCollector::makeUnique(TFilePath &path) {
  return makePathUnique(m_scene, path);
}

void ResourceCollector::process(SceneFxResource *resource) {
  const TFilePath source = decodedFxPath(m_scene, resource->resourcePath());
  if (source.isEmpty() || aborted()) return;
  const TFilePath key = physicalFxPath(source);
  const auto found = m_collectedFiles.find(key);
  if (found != m_collectedFiles.end()) {
    resource->setResourcePath(found->second);
    return;
  }
  ParticleCache::Sequence sequence;
  QString error;
  if (!ParticleCache::openSequence(source.getQString(), sequence, error)) {
    m_fxFailed = true;
    DVGui::warning(QObject::tr("Failed to collect particle cache:\n%1").arg(error));
    return;
  }
  bool external = m_scene->isExternPath(source);
  for (const QString &file : ParticleCache::sequenceFiles(sequence))
    external = external || m_scene->isExternPath(TFilePath(file));
  if (!external) {
    resource->setResourcePath(m_scene->codeFilePath(source));
    return;
  }
  const TFilePath target = newParticlePackagePath(m_scene, source);
  if (!copyParticlePackage(source, m_scene->decodeFilePath(target), error)) {
    m_fxFailed = true;
    DVGui::warning(QObject::tr("Failed to collect particle cache:\n%1").arg(error));
    return;
  }
  m_collectedFiles[key] = target;
  resource->setResourcePath(target);
  ++m_count;
}

//-----------------------------------------------------------------------------

void ResourceCollector::process(TXshSimpleLevel *sl) {
  TFilePath path     = sl->getPath();
  std::string suffix = ResourceImporter::extractPsdSuffix(path);
  std::map<TFilePath, TFilePath>::iterator it = m_collectedFiles.find(path);
  if (it != m_collectedFiles.end()) {
    TFilePath destPath = it->second;
    if (suffix != "") destPath = ResourceImporter::buildPsd(destPath, suffix);
    sl->setPath(destPath);
  } else {
    TFilePath collectedPath = path;
    if (getCollectedPath(m_scene, collectedPath)) {
      TFilePath actualCollectedPath = m_scene->decodeFilePath(collectedPath);
      if (actualCollectedPath != path && TSystem::doesExistFileOrLevel(path) &&
          !TSystem::doesExistFileOrLevel(actualCollectedPath)) {
        try {
          TSystem::touchParentDir(actualCollectedPath);
          TXshSimpleLevel::copyFiles(actualCollectedPath, path);
        } catch (...) {
        }
      }
      ++m_count;
      TFilePath destPath = collectedPath;
      if (suffix != "") destPath = ResourceImporter::buildPsd(destPath, suffix);
      sl->setPath(destPath);
      m_collectedFiles[path] = collectedPath;
    }
  }

  if (sl->getScannedPath() != TFilePath()) {
    path                    = sl->getScannedPath();
    TFilePath collectedPath = path;
    if (getCollectedPath(m_scene, collectedPath)) {
      TFilePath actualCollectedPath = m_scene->decodeFilePath(collectedPath);
      if (actualCollectedPath != path && TSystem::doesExistFileOrLevel(path)) {
        try {
          TSystem::touchParentDir(actualCollectedPath);
          TXshSimpleLevel::copyFiles(actualCollectedPath, path);
        } catch (...) {
        }
      }
      sl->setScannedPath(collectedPath);
      m_count++;
    }
  }
  sl->setDirtyFlag(false);
}

//-----------------------------------------------------------------------------

void ResourceCollector::process(TXshSoundLevel *sl) {
  TFilePath path          = sl->getPath();
  TFilePath collectedPath = path;
  if (!getCollectedPath(m_scene, collectedPath)) return;

  TFilePath actualCollectedPath = m_scene->decodeFilePath(collectedPath);
  if (actualCollectedPath != path && TSystem::doesExistFileOrLevel(path)) {
    try {
      TSystem::touchParentDir(actualCollectedPath);
      TXshSimpleLevel::copyFiles(actualCollectedPath, path);

    } catch (...) {
    }
  }
  sl->setPath(collectedPath);
  m_count++;
}

//-----------------------------------------------------------------------------

void ResourceCollector::process(TXshPaletteLevel *pl) {
  TFilePath path          = pl->getPath();
  TFilePath collectedPath = path;
  if (!getCollectedPath(m_scene, collectedPath)) return;

  TFilePath actualCollectedPath = m_scene->decodeFilePath(collectedPath);
  if (actualCollectedPath != path && TSystem::doesExistFileOrLevel(path)) {
    try {
      TSystem::touchParentDir(actualCollectedPath);
      TXshSimpleLevel::copyFiles(actualCollectedPath, path);

    } catch (...) {
    }
  }
  pl->setPath(collectedPath);
  m_count++;
}
