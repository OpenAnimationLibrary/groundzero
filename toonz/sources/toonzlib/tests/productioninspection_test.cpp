// SPDX-License-Identifier: BSD-3-Clause
#include "toonz/castworkflow.h"
#include "toonz/fxdag.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/scenechecker.h"
#include "toonz/sceneresources.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "tenv.h"
#include "tfx.h"
#include "tvectorimage.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

DV_IMPORT_API void initStdFx();

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void initializePrivateRuntime(const QString& root) {
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root);
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, root);
  TEnv::setRootVarName("TOONZROOT");
  TEnv::setSystemVarPrefix("TOONZ");
  require(TEnv::setArgPathValue("TOONZROOT", root.toStdString()),
          "cannot configure private native root");
  for (const auto& entry : TEnv::getSystemPathMap()) {
    const auto path = QDir(root).filePath(QString::fromStdString(entry.second));
    require(QDir().mkpath(path),
            "cannot create private native configuration directory");
    require(TEnv::setArgPathValue("TOONZ" + entry.first, path.toStdString()),
            "cannot configure private native directory");
  }
}

std::shared_ptr<TProject> privateProject(const QString& root) {
  auto project = std::make_shared<TProject>();
  for (const char* name : {"drawings", "scenes", "extras", "palettes"}) {
    const QString path = QDir(root).filePath(name);
    require(QDir().mkpath(path), "cannot create private project directory");
    project->setFolder(name, TFilePath(path));
  }
  return project;
}

struct Scene {
  ToonzScene scene;

  explicit Scene(const QString& root) {
    scene.setProject(privateProject(root));
    scene.setScenePath(scene.getProject()->getScenesPath() + "inspection.tnz");
  }

  TXshSimpleLevel* drawing() {
    auto* level =
        scene.createNewLevel(PLI_XSHLEVEL, L"Drawing")->getSimpleLevel();
    require(level != nullptr, "cannot create private vector level");
    level->setFrame(TFrameId(1), new TVectorImage);
    level->setCastTags({"inspection", QString::fromUtf8("caf\xc3\xa9")});
    return level;
  }

  TXshChildLevel* child(const std::wstring& name) {
    auto* level = scene.createNewLevel(CHILD_XSHLEVEL, name)->getChildLevel();
    require(level != nullptr, "cannot create private child level");
    return level;
  }
};

std::vector<SceneChecker::Issue> missingDrawings(ToonzScene& scene) {
  std::vector<SceneChecker::Issue> result;
  for (const auto& issue : SceneChecker::check(&scene)) {
    if (issue.kind == SceneChecker::MissingDrawing) result.push_back(issue);
  }
  return result;
}

void isolatedAliases(const QString& root) {
  Scene first(QDir(root).filePath("first"));
  Scene second(QDir(root).filePath("second"));
  const TFilePath coded("+drawings/shared.pli");
  const auto firstProject  = first.scene.getProject();
  const auto secondProject = second.scene.getProject();
  require(firstProject != secondProject, "private scenes share a project");
  require(first.scene.decodeFilePath(coded) ==
              firstProject->getFolder("drawings") + "shared.pli",
          "first scene alias did not use its own project");
  require(second.scene.decodeFilePath(coded) ==
              secondProject->getFolder("drawings") + "shared.pli",
          "second scene alias borrowed another project");
  require(
      first.scene.decodeFilePath(coded) != second.scene.decodeFilePath(coded),
      "coded paths from distinct projects collapsed");
  const TFilePath sceneRelative("$scenefolder/sidecar.json");
  require(first.scene.decodeFilePath(sceneRelative) ==
              first.scene.getScenePath().getParentDir() + "sidecar.json",
          "scene-relative alias did not use the owned scene");
  require(first.scene.getProject() == firstProject &&
              second.scene.getProject() == secondProject,
          "alias inspection changed a scene project");
}

void unchangedDrawing(const TXshSimpleLevel* level, const TFilePath& path,
                      const QStringList& tags, const QString& id, bool dirty) {
  require(level->getPath() == path && level->getCastTags() == tags &&
              level->getCastId() == id &&
              level->getProperties()->getDirtyFlag() == dirty,
          "scene inspection changed cast metadata or dirty state");
}

void missingDrawingLocation(const QString& root) {
  Scene fixture(root);
  auto* level = fixture.drawing();
  auto* sheet = fixture.scene.getTopXsheet();
  sheet->setCell(2, 0, TXshCell(level, TFrameId(8)));
  sheet->setCell(3, 0, TXshCell(level, TFrameId(8)));
  sheet->setCell(5, 0, TXshCell(level, TFrameId(1)));
  const auto path   = level->getPath();
  const auto tags   = level->getCastTags();
  const auto id     = level->getCastId();
  const bool dirty  = level->getProperties()->getDirtyFlag();
  const auto issues = missingDrawings(fixture.scene);
  require(issues.size() == 1, "missing drawing was omitted or duplicated");
  const auto& issue = issues.front();
  require(issue.xsheet == sheet && issue.column == 0 && issue.row == 2 &&
              issue.rowCount == 2,
          "missing drawing lost its exact sheet and local exposure range");
  require(SceneChecker::kindName(issue.kind) == "missingDrawing" &&
              SceneChecker::severityName(issue.severity) == "error",
          "missing drawing lost its stable native code or severity");
  unchangedDrawing(level, path, tags, id, dirty);
  require(sheet->getCell(2, 0).m_frameId == TFrameId(8) &&
              sheet->getCell(5, 0).m_frameId == TFrameId(1),
          "scene inspection changed exposures");
  level->setFrame(TFrameId(8), new TVectorImage);
  require(missingDrawings(fixture.scene).empty(),
          "repaired drawing retained a stale issue");
}

struct NestedScene {
  Scene fixture;
  TXshSimpleLevel* art;
  TXshChildLevel* inner;
  TXshChildLevel* outer;

  explicit NestedScene(const QString& root)
      : fixture(root)
      , art(fixture.drawing())
      , inner(fixture.child(L"Inner"))
      , outer(fixture.child(L"Outer")) {
    inner->getXsheet()->setCell(3, 2, TXshCell(art, TFrameId(8)));
    inner->getXsheet()->setCell(4, 2, TXshCell(art, TFrameId(8)));
    outer->getXsheet()->setCell(5, 1, TXshCell(inner, TFrameId(4)));
    auto* top = fixture.scene.getTopXsheet();
    top->setCell(10, 0, TXshCell(outer, TFrameId(6)));
    top->setCell(11, 0, TXshCell(outer, TFrameId(5)));
    top->setCell(30, 0, TXshCell(outer, TFrameId(6)));
    top->setCell(0, 1, TXshCell(outer, TFrameId(6)));
  }
};

void requireLeaf(const CastWorkflow::Occurrence& use,
                 const NestedScene& fixture) {
  require(use.sheet == fixture.inner->getXsheet() && use.level == fixture.art &&
              use.path.size() == 2 && use.first == 3 && use.last == 4 &&
              use.column == 2,
          "nested occurrence lost its leaf coordinates or identity");
}

void requireParents(const CastWorkflow::Occurrence& use,
                    const NestedScene& fixture, TXsheet* top) {
  require(use.path[0].child == fixture.outer &&
              use.path[1].child == fixture.inner && use.path[1].row == 5 &&
              use.path[1].column == 1 && CastWorkflow::validPath(top, use),
          "nested occurrence has an invalid instance path");
}

void sharedNestedInstances(const QString& root) {
  NestedScene fixture(root);
  auto* top        = fixture.fixture.scene.getTopXsheet();
  const auto index = CastWorkflow::index(top, {fixture.art});
  require(index.complete && !index.cycle && index.occurrences.size() == 3,
          "shared sheet identity replaced exposed instances");
  std::set<std::pair<int, int>> roots;
  for (const auto& use : index.occurrences) {
    requireLeaf(use, fixture);
    requireParents(use, fixture, top);
    roots.emplace(use.path[0].row, use.path[0].column);
  }
  require(roots == std::set<std::pair<int, int>>{{10, 0}, {30, 0}, {0, 1}},
          "adjacent child frames split or disjoint runs merged");
  const auto issues = missingDrawings(fixture.fixture.scene);
  require(
      issues.size() == 1 && issues.front().xsheet == fixture.inner->getXsheet(),
      "shared sheet issue was repeated for every outer instance");
  CastWorkflow::Limits unique;
  unique.expandInstances   = false;
  unique.recordOccurrences = false;
  const auto summary       = CastWorkflow::index(top, {}, {}, unique);
  require(summary.complete && summary.used == index.used &&
              summary.cells < index.cells,
          "unique reachability repeated shared-sheet expansion");
}

void traversalBoundaries(const QString& root) {
  NestedScene fixture(root);
  auto* top = fixture.fixture.scene.getTopXsheet();
  CastWorkflow::Limits cells;
  cells.cells = 1;
  require(!CastWorkflow::index(top, {}, {}, cells).complete,
          "cell limit was ignored");
  CastWorkflow::Limits occurrences;
  occurrences.occurrences = 1;
  require(!CastWorkflow::index(top, {}, {}, occurrences).complete,
          "result limit was ignored");
  CastWorkflow::Limits depth;
  depth.depth = 0;
  require(!CastWorkflow::index(top, {}, {}, depth).complete,
          "depth limit was ignored");
  require(!CastWorkflow::index(top, {}, [] { return true; }).complete,
          "canceled traversal certified a complete report");
  fixture.inner->getXsheet()->setCell(8, 0,
                                      TXshCell(fixture.outer, TFrameId(1)));
  const auto cycle = CastWorkflow::index(top, {fixture.art});
  fixture.inner->getXsheet()->setCell(8, 0, TXshCell());
  require(cycle.cycle && cycle.cells < 100,
          "cycle was omitted or traversed without a bound");
  require(!CastWorkflow::index(top, {fixture.art}).cycle,
          "removed cycle remained in a new report");
}

TFxP cacheFx(TXsheet* sheet, const TFilePath& path) {
  TFxP fx = TFx::create("STD_particleCacheFx");
  require(bool(fx), "native particle cache factory is unavailable");
  auto* resource = dynamic_cast<SceneFxResource*>(fx.getPointer());
  require(resource != nullptr, "native cache lacks its dependency interface");
  resource->setResourcePath(path);
  sheet->getFxDag()->getInternalFxs()->addFx(fx.getPointer());
  return fx;
}

struct DependencyVisitor final : ResourceProcessor {
  std::vector<TFilePath> paths;

  void process(SceneFxResource* resource) override {
    paths.push_back(resource->resourcePath());
  }
};

void fxOutsideCast(const QString& root) {
  Scene fixture(root);
  const TFilePath coded("+extras/unavailable/cache.otpc");
  auto first  = cacheFx(fixture.scene.getTopXsheet(), coded);
  auto second = cacheFx(fixture.scene.getTopXsheet(), coded);
  require(first != second && fixture.scene.getLevelSet()->getLevelCount() == 0,
          "FX dependency fixture unexpectedly uses cast artwork");
  auto* child = fixture.child(L"Unused child");
  const TFilePath nested("+extras/unused-child/cache.otpc");
  auto third = cacheFx(child->getXsheet(), nested);
  DependencyVisitor inventory;
  {
    // This existing visitor performs I/O; all fixture aliases resolve inside
    // the private tree.
    SceneResources resources(&fixture.scene, nullptr);
    resources.accept(&inventory);
    resources.commit();
  }
  require(
      inventory.paths.size() == 2 &&
          std::set<TFilePath>(inventory.paths.begin(), inventory.paths.end()) ==
              std::set<TFilePath>{coded, nested},
      "dependency visitor omitted FX, unused nested content, or shared-file "
      "deduplication");
  for (const TFxP& fx : {first, second, third}) {
    const auto* resource = dynamic_cast<SceneFxResource*>(fx.getPointer());
    require(resource->resourcePath() == (fx == third ? nested : coded),
            "dependency inspection rewrote a stored FX path");
  }
  require(fixture.scene.getLevelSet()->getLevelCount() == 1,
          "dependency inspection imported FX resources into the cast");
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir directory;
    require(directory.isValid(),
            "cannot create private inspection fixture root");
    require(QDir().mkpath(directory.filePath("portablestuff")) &&
                QDir::setCurrent(directory.path()),
            "cannot select private portable fixture directories");
    initializePrivateRuntime(directory.filePath("runtime"));
    initStdFx();
    isolatedAliases(directory.filePath("aliases"));
    missingDrawingLocation(directory.filePath("missing"));
    sharedNestedInstances(directory.filePath("shared"));
    traversalBoundaries(directory.filePath("bounds"));
    fxOutsideCast(directory.filePath("fx"));
    std::cout << "PASS: isolated production inspection native boundaries (5 "
                 "groups)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL production inspection: " << error.what() << '\n';
    return 1;
  } catch (const TException&) {
    std::cerr << "FAIL production inspection: unexpected native exception\n";
    return 1;
  }
}
