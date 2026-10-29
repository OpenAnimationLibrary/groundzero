// SPDX-License-Identifier: BSD-3-Clause
// CI artifact generator. Every persisted effect is a production registered FX;
// the scenes and drawing levels are written by OpenToonz's own serializers.
#include "tfx.h"
#include "tfxattributes.h"
#include "tfxparam.h"
#include "tnotanimatableparam.h"
#include "tparamset.h"
#include "trasterfx.h"
#include "trasterimage.h"
#include "trenderer.h"
#include "timage_io.h"
#include "timagecache.h"
#include "tnzimage.h"
#include "toutputproperties.h"
#include "tthread.h"
#include "tenv.h"
#include "texception.h"
#include "toonz/fxdag.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/particlebake.h"
#include "toonz/particlecache.h"
#include "toonz/preferences.h"
#include "toonz/scenefx.h"
#include "toonz/sceneresources.h"
#include "toonz/sceneproperties.h"
#include "toonz/stage.h"
#include "toonz/tcamera.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshzeraryfxcolumn.h"
#include "../../toonz/particlecachebakesource.h"

#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

DV_IMPORT_API void initStdFx();

namespace {
constexpr int FrameCount = 24;
constexpr int ImageSize = 256;
constexpr double FrameRate = 24.0;
constexpr double Pi = 3.14159265358979323846;
const TRectD Viewport(-ImageSize / 2, -ImageSize / 2,
                       ImageSize / 2, ImageSize / 2);

void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
void directory(const QString &path) {
  require(QDir().mkpath(path), "Cannot create demo directory: " + path.toStdString());
}
void writeText(const QString &path, const QByteArray &text) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly),
          "Cannot create demo document: " + path.toStdString());
  require(file.write(text) == text.size(), "Cannot write demo document");
}
template <class Parameter>
Parameter *parameter(TFx *fx, const char *name) {
  auto *value = dynamic_cast<Parameter *>(fx->getParams()->getParam(name));
  require(value != nullptr, std::string("Missing production FX parameter: ") + name);
  return value;
}
void number(TFx *fx, const char *name, double value) {
  parameter<TDoubleParam>(fx, name)->setDefaultValue(value);
}
void range(TFx *fx, const char *name, double first, double last) {
  parameter<TRangeParam>(fx, name)->setDefaultValue(DoublePair(first, last));
}
void integer(TFx *fx, const char *name, int value) {
  parameter<TIntParam>(fx, name)->setValue(value);
}
void enumeration(TFx *fx, const char *name, int value) {
  parameter<TIntEnumParam>(fx, name)->setValue(value);
}

TRenderSettings renderSettings() {
  TRenderSettings settings;
  settings.m_bpp = 32;
  settings.m_linearColorSpace = false;
  settings.m_cameraBox = Viewport;
  return settings;
}

class RenderSession final {
  TRenderer m_renderer;
  unsigned long m_id;
public:
  RenderSession() : m_renderer(1), m_id(TRenderer::buildRenderId()) {
    m_renderer.install(m_id);
    m_renderer.declareRenderStart(m_id);
  }
  ~RenderSession() {
    m_renderer.declareRenderEnd(m_id);
    m_renderer.uninstall();
  }
  void begin(int frame) { m_renderer.declareFrameStart(frame); }
  void end(int frame) { m_renderer.declareFrameEnd(frame); }
};

TRaster32P render(ToonzScene &scene, int frame) {
  TFxP built = buildSceneFx(&scene, double(frame), 1, false);
  auto *fx = dynamic_cast<TRasterFx *>(built.getPointer());
  require(fx != nullptr, "The demo scene has no renderable output");
  RenderSession session;
  session.begin(frame);
  TRaster32P result(ImageSize, ImageSize);
  result->clear();
  TTile tile(result, Viewport.getP00());
  try { fx->compute(tile, frame, renderSettings()); }
  catch (...) { session.end(frame); throw; }
  session.end(frame);
  return result;
}

void compare(const TRaster32P &a, const TRaster32P &b,
             const std::string &context) {
  require(a && b && a->getSize() == b->getSize(), context + ": invalid render size");
  std::size_t occupied = 0, different = 0;
  for (int y = 0; y < a->getLy(); ++y)
    for (int x = 0; x < a->getLx(); ++x) {
      const TPixel32 &left = a->pixels(y)[x], &right = b->pixels(y)[x];
      if (left.m) ++occupied;
      if (left != right) ++different;
    }
  require(occupied > 0, context + ": empty demo output");
  require(different == 0, context + ": " + std::to_string(different) +
                             " pixels changed");
}

std::shared_ptr<TProject> makeProject(const QString &root) {
  auto project = std::make_shared<TProject>();
  for (const char *folder : {"inputs", "drawings", "scenes", "extras",
                             "outputs", "scripts", "palettes"}) {
    directory(QDir(root).filePath(folder));
    // Project folders are deliberately relative, including scenes.xml's
    // serializer-generated project link. Moving the bundle must remain valid.
    project->setFolder(folder, TFilePath(folder));
    project->setUseScenePath(folder, false);
  }
  require(project->save(TFilePath(QDir(root).filePath("ParticleCacheExamples_otprj.xml"))),
          "Cannot write the portable demo project");
  return project;
}

void setupScene(ToonzScene &scene, const std::shared_ptr<TProject> &project,
                const QString &name) {
  scene.setProject(project);
  scene.setScenePath(project->getScenesPath() + TFilePath(name + ".tnz"));
  auto *camera = scene.getXsheet()->getStageObjectTree()->getCurrentCamera();
  require(camera != nullptr, "Demo scene has no camera");
  camera->setSize(TDimensionD(ImageSize / Stage::inch, ImageSize / Stage::inch));
  camera->setRes(TDimension(ImageSize, ImageSize));
  scene.getProperties()->setBgColor(TPixel32::Transparent);
  for (auto *output : {scene.getProperties()->getOutputProperties(),
                       scene.getProperties()->getPreviewProperties()}) {
    output->setFrameRate(FrameRate);
    output->setRange(0, FrameCount - 1, 1);
    output->setRenderSettings(renderSettings());
  }
  scene.getProperties()->getOutputProperties()->setPath(
      TFilePath("+outputs/") + TFilePath(name + "..png"));
}

// Real editable raster drawings, shared as finite input columns by native and
// cache FXs. No test-only effect registration is needed to open these scenes.
TXshSimpleLevelP artwork(ToonzScene &scene, const QString &name, int column,
                         bool control) {
  const int size = control ? 128 : 32;
  TXshSimpleLevelP level = new TXshSimpleLevel(name.toStdWString());
  level->setType(OVL_XSHLEVEL);
  level->setScene(&scene);
  level->setPath(TFilePath("+drawings/") + TFilePath(name + "..png"));
  level->getProperties()->setDpiPolicy(LevelProperties::DP_CustomDpi);
  level->getProperties()->setDpi(Stage::inch);
  level->getProperties()->setImageRes(TDimension(size, size));
  scene.getLevelSet()->insertLevel(level.getPointer());
  for (int drawing = 0; drawing < (control ? 1 : 2); ++drawing) {
    TRaster32P raster(size, size);
    raster->clear();
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) {
        const double dx = x + .5 - size / 2.0, dy = y + .5 - size / 2.0;
        if (control) {
          const bool check = ((x / 16 + y / 16) & 1) != 0;
          raster->pixels(y)[x] = check ? TPixel32(255, 165, 55)
                                      : TPixel32(45, 185, 235);
        } else {
          const double radius = 10 + 3 * std::cos(5 * std::atan2(dy, dx) + drawing * Pi / 5);
          if (dx * dx + dy * dy <= radius * radius)
            raster->pixels(y)[x] = drawing == 0 ? TPixel32(255, 200, 75)
                                                : TPixel32(70, 220, 240);
        }
      }
    TRasterImageP image(raster);
    image->setDpi(Stage::inch, Stage::inch);
    level->setFrame(TFrameId(drawing + 1), image);
  }
  for (int frame = 0; frame < FrameCount; ++frame)
    require(scene.getXsheet()->setCell(frame, column,
                TXshCell(level.getPointer(), TFrameId(control ? 1 : frame % 2 + 1))),
            "Cannot expose editable demo artwork");
  level->setDirtyFlag(true);
  level->save();
  scene.getXsheet()->getStageObject(TStageObjectId::ColumnId(column))->setName(
      name.toStdString());
  TFx *fx = scene.getXsheet()->getColumn(column)->getFx();
  fx->getAttributes()->setDagNodePos(TPointD(0, column * 120));
  scene.getXsheet()->getFxDag()->removeFromXsheet(fx);
  return level;
}

TXshZeraryFxColumnP effectColumn(ToonzScene &scene, TFx *fx, int index,
                                const std::string &name) {
  TXshZeraryFxColumnP column = new TXshZeraryFxColumn(FrameCount);
  column->getZeraryColumnFx()->setZeraryFx(fx);
  scene.getXsheet()->insertColumn(index, column.getPointer());
  scene.getXsheet()->getStageObject(TStageObjectId::ColumnId(index))->setName(name);
  if (fx->getFxId().empty()) scene.getXsheet()->getFxDag()->assignUniqueId(fx);
  fx->setName(std::wstring(name.begin(), name.end()));
  column->getFx()->getAttributes()->setDagNodePos(TPointD(520, 60));
  scene.getXsheet()->getFxDag()->addToXsheet(column->getFx());
  return column;
}

TFxP cacheFx(ToonzScene &scene, const QString &package, TFx *artworkColumn) {
  TFxP cache = TFx::create("STD_particleCacheFx");
  require(bool(cache), "Particle Cache FX is not registered");
  auto *resource = dynamic_cast<SceneFxResource *>(cache.getPointer());
  require(resource != nullptr, "Particle Cache has no scene resource interface");
  resource->setResourcePath(TFilePath("+extras/") + TFilePath(package + "/cache.otpc"));
  number(cache.getPointer(), "sceneFps", FrameRate);
  number(cache.getPointer(), "startFrame", 0);
  cache->getInputPort("Artwork1")->setFx(artworkColumn);
  scene.getXsheet()->getFxDag()->assignUniqueId(cache.getPointer());
  return cache;
}

struct Demo {
  QString sceneName;
  QString package;
  std::string nativeType;
  int cacheColumn = 0;
  std::map<int, TRaster32P> reference;
};

void saveScene(ToonzScene &scene) {
  scene.getXsheet()->updateFrameCount();
  require(scene.getFrameCount() == FrameCount, "Demo has an incorrect timeline length");
  TFilePath path = scene.getScenePath();
  scene.save(path, nullptr, false);  // Scene icons would require an OpenGL GUI.
  require(QFileInfo::exists(path.getQString()), "Scene serializer produced no demo");
}

ParticleCache::SnapshotPtr externalFrame(int frame, QString &) {
  auto snapshot = std::make_shared<ParticleCache::Snapshot>();
  snapshot->hasStableIds = snapshot->hasVelocity = snapshot->hasArtworkFrame = true;
  const double time = (frame - 101) / 12.0;
  for (int i = 0; i < 12; ++i) {
    ParticleCache::Particle p;
    p.id = 1001 + i;
    const double phase = i * 2 * Pi / 12;
    p.position = {{-85 + 170 * time, 72 * std::sin(phase + time * Pi), double(i)}};
    p.velocity = {{170, 72 * Pi * std::cos(phase + time * Pi), 0}};
    p.size = 6 + i % 3;
    p.rotation = time * 180 + i * 30;
    p.age = time;
    p.lifetime = 2;
    p.artworkFrame = i % 2;
    snapshot->particles.push_back(p);
  }
  // Scramble disk order on alternating frames: interpolation must use IDs.
  if (frame % 2) std::reverse(snapshot->particles.begin(), snapshot->particles.end());
  auto floating = [&](const char *name, int components, auto getter) {
    ParticleCache::Attribute a;
    a.name = name; a.components = components;
    a.type = components == 3 ? ParticleCache::AttributeType::Vector
                            : ParticleCache::AttributeType::Float;
    for (const auto &p : snapshot->particles)
      for (int c = 0; c < components; ++c) a.floats.push_back(float(getter(p, c)));
    snapshot->attributes.push_back(std::move(a));
  };
  ParticleCache::Attribute ids;
  ids.name = "particleId"; ids.type = ParticleCache::AttributeType::Int;
  for (const auto &p : snapshot->particles) ids.integers.push_back(std::int32_t(p.id));
  snapshot->attributes.push_back(std::move(ids));
  floating("P", 3, [](const auto &p, int c) { return p.position[c]; });
  floating("v", 3, [](const auto &p, int c) { return p.velocity[c]; });
  floating("pscale", 1, [](const auto &p, int) { return p.size; });
  floating("rotation", 1, [](const auto &p, int) { return p.rotation; });
  floating("age", 1, [](const auto &p, int) { return p.age; });
  floating("life", 1, [](const auto &p, int) { return p.lifetime; });
  floating("ot_frame", 1, [](const auto &p, int) { return p.artworkFrame; });
  return snapshot;
}

Demo externalDemo(const std::shared_ptr<TProject> &project) {
  Demo demo;
  demo.sceneName = "01-external-motion";
  demo.package = "external-motion";
  demo.cacheColumn = 1;
  ToonzScene scene;
  setupScene(scene, project, demo.sceneName);
  const auto sprite = artwork(scene, "external-star", 0, false);
  ParticleCache::Metadata metadata;
  metadata.firstFrame = 101; metadata.lastFrame = 113;
  metadata.framesPerSecond = 12;
  metadata.mapping.position = "P";
  metadata.mapping.velocity = "v";
  metadata.mapping.id = "particleId";
  metadata.extra["description"] = "Stable-ID external motion, 12 fps in a 24 fps scene";
  for (const auto &fid : sprite->getFids())
    metadata.artworkPaths << scene.decodeFilePath(sprite->getPath()).withFrame(fid).getQString();
  QString error;
  const QString destination = project->decode(TFilePath("extras/" + demo.package)).getQString();
  require(ParticleCache::writePackage(destination, metadata, externalFrame,
                                      ParticleCache::WriteOptions(), error),
          "Cannot write external motion demo: " + error.toStdString());
  TFxP cache = cacheFx(scene, demo.package, scene.getXsheet()->getColumn(0)->getFx());
  enumeration(cache.getPointer(), "interpolation", 1);
  effectColumn(scene, cache.getPointer(), demo.cacheColumn, "External Motion - OT Artwork");
  for (int frame : {0, 11, 23}) demo.reference.emplace(frame, render(scene, frame));
  saveScene(scene);
  return demo;
}

Demo nativeDemo(const std::shared_ptr<TProject> &project, bool tiled) {
  Demo demo;
  demo.sceneName = tiled ? "03-tiled-particles" : "02-standard-particles";
  demo.package = tiled ? "tiled-bake" : "standard-bake";
  demo.nativeType = tiled ? "STD_iwa_TiledParticlesFx" : "STD_particlesFx";
  demo.cacheColumn = 2;
  ToonzScene scene;
  setupScene(scene, project, demo.sceneName);
  artwork(scene, demo.package + "-star", 0, false);
  artwork(scene, demo.package + "-control", 1, true);
  TXsheet *xsheet = scene.getXsheet();
  TFxP source = TFx::create(demo.nativeType);
  require(bool(source), "Native particle FX is not registered");
  source->getInputPort("Texture1")->setFx(xsheet->getColumn(0)->getFx());
  source->getInputPort("Control1")->setFx(xsheet->getColumn(1)->getFx());
  integer(source.getPointer(), "random_seed", 127);
  integer(source.getPointer(), "starting_frame", 1);
  number(source.getPointer(), "birth_rate", 3);
  number(source.getPointer(), "length", 90);
  number(source.getPointer(), "height", 70);
  range(source.getPointer(), "lifetime", 30, 30);
  range(source.getPointer(), "speed", .75, 2);
  range(source.getPointer(), "scale", 35, 60);
  range(source.getPointer(), "rot", -25, 25);
  range(source.getPointer(), "opacity", 75, 100);
  enumeration(source.getPointer(), "animation", 2);
  if (tiled) {
    integer(source.getPointer(), "base_ctrl", 1);
    enumeration(source.getPointer(), "rendermode", 0);
    number(source.getPointer(), "triangleSize", 24);
    number(source.getPointer(), "margin", 0);
    number(source.getPointer(), "flap_velocity", 12);
  } else {
    range(source.getPointer(), "trail", 2, 2);
    number(source.getPointer(), "trail_step", 1);
    number(source.getPointer(), "birth_color_fade", 35);
    integer(source.getPointer(), "birth_color_ctrl", 1);
  }
  auto column = effectColumn(scene, source.getPointer(), demo.cacheColumn,
                              tiled ? "Tiled Particle Playback" : "Standard Particle Playback");
  source->setName(L"EDITABLE SOURCE - connect to Xsheet to compare");
  for (int frame : {5, 12, 23}) demo.reference.emplace(frame, render(scene, frame));

  ParticleCache::Metadata metadata;
  metadata.firstFrame = 0; metadata.lastFrame = FrameCount - 1;
  metadata.framesPerSecond = FrameRate;
  metadata.extra["sourceEffect"] = QString::fromStdString(demo.nativeType);
  metadata.extra["sceneStartFrame"] = 0;
  metadata.extra["sceneFramesPerSecond"] = FrameRate;
  metadata.extra["sizeSemantics"] = "artwork-scale";
  metadata.extra["captureFraming"] = "camera";
  const QString destination = project->decode(TFilePath("extras/" + demo.package)).getQString();
  QString error;
  const bool written = ParticleBake::writePackage(destination, metadata,
      [&](int frame, ParticleBake::Frame &result, QString &failure) {
        ParticleBakeScene::Source prepared;
        if (!ParticleBakeScene::prepare(&scene, xsheet, source, frame, prepared, failure))
          return false;
        if (!prepared.native) { failure = "Demo source exposure is empty"; return false; }
        TRenderSettings settings = renderSettings();
        settings.m_affine = prepared.placement;
        ParticleBake::Options options;
        options.domain = settings.m_affine.inv() * Viewport;
        options.pixelDomain = Viewport;
        options.framesPerSecond = FrameRate;
        options.proceduralArtworkLastFrame = FrameCount - 1;
        RenderSession session;
        session.begin(frame);
        bool ok = false;
        try {
          ok = ParticleBake::captureFrame(prepared.native.getPointer(), frame,
                                           settings, options, result, failure);
        } catch (...) { session.end(frame); throw; }
        session.end(frame);
        return ok;
      }, error);
  require(written, "Cannot bake native demo: " + error.toStdString());

  TFxP cache = cacheFx(scene, demo.package, xsheet->getColumn(0)->getFx());
  cache->setName(L"BAKED PLAYBACK - retained native appearance");
  // Same non-destructive graph replacement used by Bake and Use Cache: the
  // source and its input links remain editable in the saved FX schematic.
  xsheet->getFxDag()->getInternalFxs()->addFx(source.getPointer());
  source->getAttributes()->setDagNodePos(TPointD(260, 220));
  column->getZeraryColumnFx()->setZeraryFx(cache.getPointer());
  for (const auto &item : demo.reference)
    compare(item.second, render(scene, item.first), demo.sceneName.toStdString() + "/live-vs-cache");
  saveScene(scene);
  return demo;
}

void insideBundle(const QString &file, const QString &root) {
  const QFileInfo info(file);
  require(info.isFile() && info.canonicalFilePath().startsWith(root + '/'),
          "Demo dependency is missing or outside the relocated bundle: " + file.toStdString());
}

void validateDemo(const QString &root, const Demo &demo) {
  ToonzScene reopened;
  reopened.load(TFilePath(QDir(root).filePath("scenes/" + demo.sceneName + ".tnz")), false);
  require(reopened.getFrameCount() == FrameCount, "Reopened demo changed timeline length");
  require(reopened.getProperties()->getOutputProperties()->getFrameRate() == FrameRate,
          "Reopened demo changed scene timing");
  auto *column = dynamic_cast<TXshZeraryFxColumn *>(
      reopened.getXsheet()->getColumn(demo.cacheColumn));
  require(column != nullptr, "Reopened demo lost its cache column");
  TFx *cache = column->getZeraryColumnFx()->getZeraryFx();
  auto *resource = dynamic_cast<SceneFxResource *>(cache);
  require(resource && cache->getFxType() == "STD_particleCacheFx",
          "Reopened demo lost its production cache effect");
  require(!resource->resourcePath().isAbsolute(), "Serialized demo cache path is not portable");
  const QString cachePath = reopened.decodeFilePath(resource->resourcePath()).getQString();
  ParticleCache::Sequence sequence;
  QString error;
  require(ParticleCache::openSequence(cachePath, sequence, error),
          "Cannot reopen relocated cache: " + error.toStdString());
  require(sequence.frames.size() == (demo.nativeType.empty() ? 13u : 24u),
          "Reopened demo cache lost sequence samples");
  for (const QString &file : ParticleCache::sequenceFiles(sequence)) insideBundle(file, root);
  for (int i = 0; i < reopened.getLevelSet()->getLevelCount(); ++i) {
    auto *level = reopened.getLevelSet()->getLevel(i)->getSimpleLevel();
    if (!level) continue;
    require(!level->getPath().isAbsolute(), "Serialized artwork path is not portable");
    for (const auto &fid : level->getFids()) {
      insideBundle(reopened.decodeFilePath(level->getPath()).withFrame(fid).getQString(), root);
      require(bool(level->getFrame(fid, false)), "Cannot load relocated editable artwork");
    }
  }
  if (!demo.nativeType.empty()) {
    bool retainedSource = false;
    auto *internal = reopened.getXsheet()->getFxDag()->getInternalFxs();
    for (int i = 0; i < internal->getFxCount(); ++i) {
      TFx *fx = internal->getFx(i);
      if (fx->getFxType() == demo.nativeType) {
        retainedSource = fx->getInputPort("Texture1")->isConnected() &&
                         fx->getInputPort("Control1")->isConnected();
        break;
      }
    }
    require(retainedSource, "Reopened native demo lost its editable generator or input links");
  } else {
    require(sequence.metadata.framesPerSecond == 12 && sequence.metadata.firstFrame == 101 &&
                sequence.mapping.position == "P" && sequence.mapping.velocity == "v" &&
                sequence.mapping.id == "particleId", "External demo lost mapping or cache timing");
  }
  ParticleCache::Playback playback;
  playback.sceneFramesPerSecond = FrameRate;
  playback.interpolate = demo.nativeType.empty();
  for (const auto &item : demo.reference) {
    const auto snapshot = ParticleCache::sampleSequence(sequence, item.first, playback,
                                                         sequence.mapping, error);
    require(snapshot && snapshot->hasStableIds && !snapshot->particles.empty(),
            "Demo cache lost stable particle identity or mapped data: " + error.toStdString());
    if (!demo.nativeType.empty()) {
      ParticleBake::Frame native;
      require(ParticleBake::loadFrame(*snapshot, native, error) && !native.draws.empty(),
              "Native demo lost its draw recipe or artwork: " + error.toStdString());
      require(native.frame == item.first, "Native demo changed its captured frame timing");
    } else {
      require(snapshot->particles.size() == 12 && snapshot->hasVelocity && snapshot->hasArtworkFrame,
              "External demo lost mapped particle data");
      std::set<std::int64_t> identities;
      const double sample = item.first * .5;
      const double lowerTime = std::floor(sample) / 12;
      const double upperTime = std::ceil(sample) / 12;
      const double weight = sample - std::floor(sample);
      for (const auto &particle : snapshot->particles) {
        require(particle.id >= 1001 && particle.id <= 1012 &&
                    identities.insert(particle.id).second,
                "External demo changed stable particle identities");
        const double phase = (particle.id - 1001) * 2 * Pi / 12;
        const double expectedX = -85 + 170 * item.first / FrameRate;
        const double expectedY = 72 * ((1 - weight) * std::sin(phase + lowerTime * Pi) +
                                         weight * std::sin(phase + upperTime * Pi));
        require(std::abs(particle.position[0] - expectedX) < .0001 &&
                    std::abs(particle.position[1] - expectedY) < .0001,
                "External demo lost mapped positions or interpolated the wrong particle IDs");
      }
    }
    const TRaster32P image = render(reopened, item.first);
    compare(item.second, image, demo.sceneName.toStdString() + "/relocated-scene");
    if (item.first == 11 || item.first == 12)
      TImageWriter::save(TFilePath(QDir(root).filePath("previews/" + demo.sceneName + ".png")), image);
  }
  std::cout << "Validated " << demo.sceneName.toStdString()
            << ": saved, relocated, reopened, rendered; dependencies stay in bundle\n";
}
}  // namespace

int main(int argc, char **argv) {
  QGuiApplication application(argc, argv);
  if (argc != 3) {
    std::cerr << "Usage: particlecache_demo <new-or-empty-output-directory> <example-README>\n";
    return 2;
  }
  try {
    const QFileInfo outputInfo(QDir::cleanPath(QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath()));
    require(!outputInfo.isSymLink(), "Demo output must not be a symbolic link");
    if (outputInfo.exists()) {
      require(outputInfo.isDir() && QDir(outputInfo.absoluteFilePath()).entryList(
                  QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty(),
              "Demo output must be new or empty; existing files are never replaced");
      require(QDir().rmdir(outputInfo.absoluteFilePath()), "Cannot prepare empty demo output");
    }
    directory(outputInfo.absolutePath());
    QTemporaryDir staging(QDir(outputInfo.absolutePath()).filePath(".particle-demos-XXXXXX"));
    QTemporaryDir state;
    require(staging.isValid() && state.isValid(), "Cannot create demo generation workspace");
    const QString stuff = state.filePath("stuff");
    directory(QDir(stuff).filePath("profiles/layouts/rooms/Default"));
    directory(QDir(stuff).filePath("library"));
    directory(state.filePath("cache"));
    TEnv::setStuffDir(TFilePath(stuff));
    TEnv::setApplicationFullName("OpenToonz Particle Cache Examples");
    Preferences::instance()->setValue(backupEnabled, false);
    Preferences::instance()->setValue(compressSceneFiles, false);
    TImageCache::instance()->setRootDir(TFilePath(state.filePath("cache")));
    TThread::init();
    initImageIo();
    initStdFx();
    const auto project = makeProject(staging.path());
    std::vector<Demo> demos;
    demos.push_back(externalDemo(project));
    demos.push_back(nativeDemo(project, false));
    demos.push_back(nativeDemo(project, true));
    require(QFile::copy(QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath(),
                        staging.filePath("README.md")), "Cannot copy example README");
    directory(staging.filePath("previews"));
    // A real relocation catches accidentally persisted generation-time paths.
    require(QDir().rename(staging.path(), outputInfo.absoluteFilePath()),
            "Cannot publish portable demo bundle");
    staging.setAutoRemove(false);
    TImageCache::instance()->clear();
    ParticleCache::clearMemoryCache();
    const QString root = QFileInfo(outputInfo.absoluteFilePath()).canonicalFilePath();
    for (const auto &demo : demos) validateDemo(root, demo);
    writeText(QDir(root).filePath("VALIDATED.txt"),
              "Generated with production OpenToonz scene, level, particle and bake APIs.\n"
              "Three scenes saved, moved, reopened and rendered at three sample frames each.\n"
              "Native source/cache pixels match exactly at frames 6, 13 and 24.\n"
              "All particle files, native sprites, sidecars and drawing frames resolve inside this bundle.\n"
              "No external tools, external artwork or test-only FX are required to open the scenes.\n");
    std::cout << "Particle Cache example bundle: " << root.toStdString() << '\n';
    return 0;
  } catch (const TException &error) {
    std::cerr << "Particle demo generation failed: "
              << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) {
    std::cerr << "Particle demo generation failed: " << error.what() << '\n';
  }
  return 1;
}
