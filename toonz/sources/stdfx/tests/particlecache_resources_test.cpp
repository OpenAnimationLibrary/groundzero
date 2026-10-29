// SPDX-License-Identifier: BSD-3-Clause
#include "../stdfx.h"
#include "tfxparam.h"
#include "tnotanimatableparam.h"
#include "toonz/particlecache.h"
#include "toonz/sceneresources.h"
#include "toonz/scenefx.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/levelset.h"
#include "toonz/txsheet.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshzeraryfxcolumn.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/fxdag.h"
#include "toonz/stage.h"
#include "toonz/tcamera.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

DV_IMPORT_API void initStdFx();

// Resource behavior is tested independently of sprite rendering. This probe
// uses the same public interface and real scene graph/visitor paths as the FX.
class ParticleResourceProbe final : public TStandardZeraryFx,
                                    public SceneFxResource {
  FX_PLUGIN_DECLARATION(ParticleResourceProbe)
  TStringParamP m_path;
public:
  TFilePath resolved;
  ParticleResourceProbe() : m_path(L"") { bindParam(this, "cacheFile", m_path); }
  TFilePath resourcePath() const override { return TFilePath(m_path->getValue()); }
  void setResourcePath(const TFilePath &p) override {
    m_path->setValue(p.getWideString());
    resolved = TFilePath();
  }
  void setResolvedResourcePath(const TFilePath &p) override { resolved = p; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(); return false;
  }
  void doCompute(TTile &tile, double, const TRenderSettings &) override {
    tile.getRaster()->clear();
  }
};
FX_PLUGIN_IDENTIFIER(ParticleResourceProbe, "particleResourceProbe")

namespace {
void check(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void write(const QString &path, const QByteArray &bytes) {
  check(QDir().mkpath(QFileInfo(path).absolutePath()), "create fixture directory");
  QFile file(path);
  check(file.open(QIODevice::WriteOnly), "open fixture output");
  check(file.write(bytes) == bytes.size(), "write fixture output");
}
QByteArray read(const QString &path) {
  QFile file(path);
  check(file.open(QIODevice::ReadOnly), "read collected asset");
  return file.readAll();
}
std::shared_ptr<TProject> project(const QString &root) {
  auto result = std::make_shared<TProject>();
  for (const char *folder : {"extras", "scenes", "drawings", "palettes"}) {
    const QString path = root + "/" + folder;
    check(QDir().mkpath(path), "create project folder");
    result->setFolder(folder, TFilePath(path));
  }
  return result;
}
void setup(ToonzScene &scene, const std::shared_ptr<TProject> &p) {
  scene.setProject(p);
  scene.setScenePath(p->getScenesPath() + "test.tnz");
}
QString fixture(const QString &root) {
  ParticleCache::Metadata metadata;
  metadata.firstFrame = 1; metadata.lastFrame = 2;
  ParticleCache::WriteOptions options;
  options.preserveOriginalAttributes = false;
  QString error;
  auto frame = [](int number, QString &) -> ParticleCache::SnapshotPtr {
    auto snapshot = std::make_shared<ParticleCache::Snapshot>();
    snapshot->hasStableIds = true;
    ParticleCache::Particle p; p.id = 7; p.position[0] = number;
    snapshot->particles.push_back(p);
    return snapshot;
  };
  check(ParticleCache::writePackage(root, metadata, frame, options, error),
        error.toUtf8().constData());
  const QString path = root + "/cache.otpc";
  QJsonObject manifest = QJsonDocument::fromJson(read(path)).object();
  const QByteArray draws = R"({"draws":[{"artwork":"artwork/native.otsprite"}]})";
  write(root + "/artwork/native.otsprite", "unchanged native sprite bytes");
  write(root + "/frames/00000001.draws.json", draws);
  manifest["artwork"] = QJsonArray{"artwork/native.otsprite",
                                  "frames/00000001.draws.json"};
  QJsonArray entries = manifest["frames"].toArray();
  auto first = entries[0].toObject();
  first["extra"] = QJsonObject{{"native", true},
                               {"drawFile", "frames/00000001.draws.json"}};
  entries[0] = first;
  manifest["frames"] = entries;
  write(path, QJsonDocument(manifest).toJson());
  return path;
}
void verifyPackage(const QString &path, const QString &source) {
  ParticleCache::Sequence sequence;
  QString error;
  check(ParticleCache::openSequence(path, sequence, error), error.toUtf8().constData());
  check(sequence.frames.size() == 2, "lost particle sequence frames");
  const auto extra = sequence.frameMetadata.at(1);
  check(extra["native"].toBool(), "lost native frame metadata");
  check(extra["drawFile"].toString() == "frames/00000001.draws.json",
        "changed native sidecar reference");
  QDir target(QFileInfo(path).absolutePath());
  QDir original(QFileInfo(source).absolutePath());
  for (const QString &relative : {QString("frames/00000001.draws.json"),
                                  QString("artwork/native.otsprite")})
    check(read(target.filePath(relative)) == read(original.filePath(relative)),
          "native sidecar or sprite changed during collection");
  for (const QString &file : ParticleCache::sequenceFiles(sequence))
    check(QFileInfo::exists(file), "collected cache has a missing dependency");
}
struct StopProcessor : ResourceProcessor {
  bool stopped = false;
  void process(SceneFxResource *resource) override {
    resource->setResourcePath(TFilePath("changed.otpc")); stopped = true;
  }
  bool aborted() const override { return stopped; }
};
struct ImportStrategy : ResourceImportStrategy {
  explicit ImportStrategy(int mode) : ResourceImportStrategy(mode) {}
  TFilePath process(ToonzScene *, ToonzScene *, TFilePath path) override { return path; }
};

TFxP preparedGenerator(ToonzScene &scene, TFx *source, TAffine &placement) {
  TFxP built = buildSceneFx(&scene, 1.0, scene.getXsheet(), TFxP(source),
                           BSFX_DEFAULT_TR, false);
  placement = TAffine();
  while (auto affine = dynamic_cast<NaAffineFx *>(built.getPointer())) {
    placement = placement * affine->getPlacement(1.0);
    built = affine->getInputPort(0)->getFx();
  }
  return built;
}

void verifyStandaloneCache(const QString &path,
                           const std::shared_ptr<TProject> &p) {
  ToonzScene scene;
  setup(scene, p);
  TXsheet *xsheet = scene.getXsheet();
  FxDag *dag = xsheet->getFxDag();
  TCamera *camera = xsheet->getStageObjectTree()->getCurrentCamera();
  check(camera != nullptr, "standalone fixture has no camera");
  camera->setSize(TDimensionD(64.0 / Stage::inch, 64.0 / Stage::inch));
  camera->setRes(TDimension(64, 64));

  // Baking retains this real native generator as an internal node. Without
  // textures it renders the native default circles; no artwork input is needed.
  TFxP source = TFx::create("STD_particlesFx");
  TFxP cache = TFx::create("STD_particleCacheFx");
  check(bool(source) && bool(cache), "particle generators are not registered");
  auto resource = dynamic_cast<SceneFxResource *>(cache.getPointer());
  check(resource != nullptr, "cache does not implement the scene resource interface");
  resource->setResourcePath(TFilePath(path));
  dag->getInternalFxs()->addFx(source.getPointer());
  dag->addToXsheet(source.getPointer());
  TAffine nativePlacement, cachePlacement;
  TFxP native = preparedGenerator(scene, source.getPointer(), nativePlacement);
  check(native && native->getFxType() == source->getFxType(),
        "retained native generator did not expand");

  // Match the standalone branch of Bake and Use Cache, leaving the original
  // graph in the scene. The old unary dispatch dropped the cache here.
  dag->removeFromXsheet(source.getPointer());
  dag->getInternalFxs()->addFx(cache.getPointer());
  dag->addToXsheet(cache.getPointer());
  TFxP playback = preparedGenerator(scene, cache.getPointer(), cachePlacement);
  check(playback && playback->getFxType() == cache->getFxType(),
        "standalone cache with no artwork disappeared during scene preparation");
  check(cachePlacement == nativePlacement &&
            cache->getInputPortCount() == 1 &&
            !cache->getInputPort("Artwork1")->isConnected(),
        "standalone cache changed placement or required a dummy artwork port");
  check(bool(buildSceneFx(&scene, 1.0, 1, false)),
        "standalone cache disappeared from the complete scene graph");

  TXshZeraryFxColumnP control = new TXshZeraryFxColumn(2);
  TFxP controlFx = new ParticleResourceProbe;
  control->getZeraryColumnFx()->setZeraryFx(controlFx.getPointer());
  xsheet->insertColumn(0, control.getPointer());
  xsheet->getStageObject(TStageObjectId::ColumnId(0))
      ->setParam(TStageObject::T_X, 0, 3.0 / Stage::inch);
  source->getInputPort("Control1")->setFx(control->getFx());
  check(cache->addInputPort("Placement1", new TRasterFxPort, 1),
        "cannot add an explicit cache placement anchor");
  cache->getInputPort("Placement1")->setFx(source.getPointer());
  native = preparedGenerator(scene, source.getPointer(), nativePlacement);
  playback = preparedGenerator(scene, cache.getPointer(), cachePlacement);
  check(native && playback && cachePlacement == nativePlacement &&
            !cachePlacement.isIdentity(),
        "control-only cache lost its inherited stage placement");

  // Control1 precedes a later Texture2 in the native graph. Placement must win
  // over Artwork2 even when a new artwork port was added after the anchor.
  TXshZeraryFxColumnP artwork = new TXshZeraryFxColumn(2);
  TFxP artworkFx = new ParticleResourceProbe;
  artwork->getZeraryColumnFx()->setZeraryFx(artworkFx.getPointer());
  xsheet->insertColumn(1, artwork.getPointer());
  xsheet->getStageObject(TStageObjectId::ColumnId(1))
      ->setParam(TStageObject::T_X, 0, -7.0 / Stage::inch);
  check(source->addInputPort("Texture2", new TRasterFxPort, 0) &&
            cache->addInputPort("Artwork2", new TRasterFxPort, 0),
        "cannot extend standalone generator artwork");
  source->getInputPort("Texture2")->setFx(artwork->getFx());
  cache->getInputPort("Artwork2")->setFx(artwork->getFx());
  native = preparedGenerator(scene, source.getPointer(), nativePlacement);
  playback = preparedGenerator(scene, cache.getPointer(), cachePlacement);
  check(native && playback && cachePlacement == nativePlacement,
        "artwork placement overrode the retained control placement anchor");

  // The first connected Texture1 is hidden, so the native scene builder skips
  // it and inherits the visible Control1 placement. Anchoring to the retained
  // native node must make that same selection instead of using hidden Texture1.
  TXshZeraryFxColumnP hidden = new TXshZeraryFxColumn(2);
  TFxP hiddenFx = new ParticleResourceProbe;
  hidden->getZeraryColumnFx()->setZeraryFx(hiddenFx.getPointer());
  xsheet->insertColumn(2, hidden.getPointer());
  xsheet->getStageObject(TStageObjectId::ColumnId(2))
      ->setParam(TStageObject::T_X, 0, 19.0 / Stage::inch);
  hidden->setPreviewVisible(false);
  source->getInputPort("Texture1")->setFx(hidden->getFx());
  cache->getInputPort("Artwork1")->setFx(hidden->getFx());
  const TAffine controlPlacement = nativePlacement;
  native = preparedGenerator(scene, source.getPointer(), nativePlacement);
  playback = preparedGenerator(scene, cache.getPointer(), cachePlacement);
  check(native && playback && nativePlacement == controlPlacement &&
            cachePlacement == nativePlacement,
        "hidden artwork displaced the standalone cache from its visible control");
  check(cache->getInputPort("Placement1")->getFx() == source.getPointer() &&
            !dag->checkLoop(source.getPointer(), cache.getPointer()),
        "standalone placement anchor formed a cache cycle");
  check(dag->getInternalFxs()->containsFx(source.getPointer()) &&
            source->getInputPort("Texture1")->getFx() == hidden->getFx() &&
            source->getInputPort("Control1")->getFx() == control->getFx() &&
            source->getInputPort("Texture2")->getFx() == artwork->getFx(),
        "standalone cache replacement changed the retained native graph");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir directory;
  try {
    initStdFx();
    check(directory.isValid(), "temporary directory unavailable");
    const QString source = fixture(directory.filePath("external"));
    ToonzScene scene;
    setup(scene, project(directory.filePath("project")));
    TFxP firstOwner = new ParticleResourceProbe;
    TFxP secondOwner = new ParticleResourceProbe;
    auto *first = static_cast<ParticleResourceProbe *>(firstOwner.getPointer());
    auto *second = static_cast<ParticleResourceProbe *>(secondOwner.getPointer());
    first->setResourcePath(TFilePath(source));
    second->setResourcePath(TFilePath(source));
    scene.getXsheet()->getFxDag()->getInternalFxs()->addFx(first);
    // A detached child sheet in the cast still owns resources that must travel.
    TXshChildLevelP child = new TXshChildLevel(L"nested");
    child->setScene(&scene);
    child->getXsheet()->getFxDag()->getInternalFxs()->addFx(second);
    scene.getLevelSet()->insertLevel(child.getPointer());

    {
      SceneResources resources(&scene, nullptr);
      ResourceCollector collector(&scene);
      resources.accept(&collector);
      check(!collector.aborted(), "collection failed");
      check(collector.getCollectedResourceCount() == 1, "duplicate cache copied twice");
    }
    check(first->resourcePath() == second->resourcePath(), "nested cache path not propagated");
    check(!first->resourcePath().isAbsolute(), "collected cache was not project-relative");
    const TFilePath collected = scene.decodeFilePath(first->resourcePath());
    verifyPackage(collected.getQString(), source);
    const TFilePath saved = first->resourcePath();
    {
      SceneResources resources(&scene, nullptr);
      StopProcessor stop;
      resources.accept(&stop);
      check(first->resourcePath() == saved && second->resourcePath() == saved,
            "aborted resource operation failed to roll back paths");
    }
    check(QFileInfo::exists(collected.getQString()), "rollback removed committed cache");

    TFxP clone = first->clone(false);
    auto *probe = dynamic_cast<ParticleResourceProbe *>(clone.getPointer());
    check(probe != nullptr, "resource clone failed");
    resolveSceneFxResources(probe, &scene);
    check(probe->resolved == collected && first->resolved.isEmpty(),
          "clone path resolution mutated original or failed alias decoding");
    check(first->resourcePath() == saved, "render resolution changed serialized path");
    makeSceneFxResourcesAbsolute(probe, &scene);
    check(probe->resourcePath() == collected && first->resourcePath() == saved,
          "clipboard resource path not detached from source project");

    TFxP columnOwner = new ParticleResourceProbe;
    auto *columnResource = static_cast<ParticleResourceProbe *>(columnOwner.getPointer());
    columnResource->setResourcePath(saved);
    TXshZeraryFxColumnP column = new TXshZeraryFxColumn(1);
    auto *columnFx = column->getZeraryColumnFx();
    columnFx->setZeraryFx(columnOwner.getPointer());
    scene.getXsheet()->insertColumn(0, column.getPointer());
    TFxP built = buildSceneFx(&scene, 0.0, TFxP(columnFx), false);
    auto *builtResource = dynamic_cast<ParticleResourceProbe *>(built.getPointer());
    check(builtResource && builtResource->resolved == collected &&
              columnResource->resolved.isEmpty(),
          "scene builder did not resolve resource on its detached render clone");

    auto importedProject = project(directory.filePath("imported"));
    ImportStrategy import(ResourceImportStrategy::IMPORT_AND_RENAME);
    {
      SceneResources resources(&scene, nullptr);
      ResourceImporter importer(&scene, importedProject, import);
      resources.accept(&importer);
      check(!importer.aborted(), "cache project import failed");
    }
    ToonzScene importedScene;
    setup(importedScene, importedProject);
    verifyPackage(importedScene.decodeFilePath(first->resourcePath()).getQString(), source);
    check(first->resourcePath() == second->resourcePath(), "import lost cache deduplication");

    // Save As must preserve references whose physical location does not change.
    scene.setProject(importedProject);
    scene.setScenePath(importedProject->getScenesPath() + "test.tnz");
    const TFilePath importedPath = first->resourcePath();
    {
      SceneResources resources(&scene, nullptr);
      check(resources.save(importedProject->getScenesPath() + "renamed.tnz", false),
            "Save As failed for cache resources");
      resources.updatePaths(); resources.commit();
    }
    check(first->resourcePath() == importedPath, "Save As changed stable project alias");
    verifyStandaloneCache(source, importedProject);
    std::cout << "PASS: particle cache collection, native sidecars, nested deduplication, "
                 "rollback, clone paths, clipboard paths, import, Save As and "
                 "standalone generator placement\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL particle cache resources: " << error.what() << '\n';
    return 1;
  } catch (const TException &) {
    std::cerr << "FAIL particle cache resources: unexpected OT exception\n";
    return 1;
  }
}
