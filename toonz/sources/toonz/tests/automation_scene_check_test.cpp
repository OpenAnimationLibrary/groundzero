#include "automation_scene_check.h"

#include "automation_jobs.h"

#include "tpalette.h"
#include "tsimplecolorstyles.h"
#include "tstroke.h"
#include "tvectorimage.h"
#include "toonz/scenechecker.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>

#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

constexpr int Drawings = 32;

// A vector level with 32 drawings exposed in one column, painted with styles
// 1 and 2 and, on even drawings, the added style `used`; the added style
// `unused` and its page are painted by none.
struct Fixture final {
  Context& context;
  Document& document;
  TXshSimpleLevel* level = nullptr;
  int used               = 0;
  int unused             = 0;

  explicit Fixture(Context& owner)
      : context(owner)
      , document(owner.addDocument(std::make_unique<ToonzScene>())) {
    document.scene->setScenePath(context.project()->getScenesPath() +
                                 "scene_check.tnz");
    level = document.scene->createNewLevel(PLI_XSHLEVEL, L"Checked")
                ->getSimpleLevel();
    require(level && level->getPalette(), "Cannot create the check fixture");
    TPalette* palette = level->getPalette();
    used              = palette->addStyle(new TSolidColorStyle(TPixel32::Red));
    palette->getPage(0)->addStyle(used);
    unused = palette->addStyle(new TSolidColorStyle(TPixel32::Blue));
    palette->addPage(L"Extra")->addStyle(unused);
    for (int frame = 0; frame < Drawings; ++frame) {
      TVectorImageP image = new TVectorImage;
      image->setPalette(palette);
      addStroke(image, 1, frame);
      if (frame % 2 == 0) addStroke(image, used, frame);
      level->setFrame(TFrameId(frame + 1), image);
      document.scene->getTopXsheet()->setCell(
          frame, 0, TXshCell(level, TFrameId(frame + 1)));
    }
  }
  ~Fixture() { context.removeDocument(document.id); }

  static void addStroke(const TVectorImageP& image, int style, int offset) {
    std::vector<TThickPoint> points{TThickPoint(offset, 0, 1),
                                    TThickPoint(offset + 5, 5, 1),
                                    TThickPoint(offset + 10, 0, 1)};
    TStroke* stroke = TStroke::interpolate(points, 0.1);
    stroke->setStyle(style);
    image->addStroke(stroke);
  }

  QJsonObject submit(const QString& check, QJsonObject extra = {}) {
    extra.insert("document_id", document.id);
    extra.insert("expected_revision", document.revisionText());
    extra.insert("check", check);
    extra.insert("operation_id", "scene-check-test");
    return executeSceneCheck(context, "scene.check.submit", extra).toObject();
  }

  QJsonObject job(const QString& method, const QString& id,
                  QJsonObject params = {}) {
    params.insert("job_id", id);
    return executeJobs(context, method, params).toObject();
  }

  QJsonObject finish(const QString& id) {
    QElapsedTimer timer;
    timer.start();
    while (!job("job.status", id).value("terminal").toBool()) {
      require(timer.elapsed() < 60000, "A scene check did not finish");
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return job("job.status", id);
  }
};

std::set<int> reportedStyles(const QJsonObject& result) {
  std::set<int> styles;
  for (const QJsonValue& row : result.value("rows").toArray())
    styles.insert(row.toObject().value("style_id").toInt());
  return styles;
}

void unusedStylesMatchSceneChecker(Context& context) {
  Fixture fixture(context);
  const QString id = fixture.submit("unused_styles").value("job_id").toString();
  const QJsonObject ended = fixture.finish(id);
  require(ended.value("status") == "completed", "The unused-style scan failed");
  const QJsonObject result = fixture.job("job.result", id, {{"limit", 256}});
  SceneChecker::Options options;
  options.unusedStyles = true;
  std::set<int> native;
  for (const SceneChecker::Issue& issue :
       SceneChecker::check(fixture.document.scene.get(), options))
    if (issue.kind == SceneChecker::UnusedStyle) native.insert(issue.styleId);
  require(reportedStyles(result) == native,
          "The scan disagrees with SceneChecker on unused styles");
  require(native.count(fixture.unused) && !native.count(fixture.used),
          "The fixture does not exercise used and unused styles");
  require(result.value("complete").toBool() &&
              ended.value("analysis")
                      .toObject()
                      .value("cost")
                      .toObject()
                      .value("drawings_done") == Drawings,
          "The scan did not report every drawing done");
}

void stepsStayShort(Context& context) {
  Fixture fixture(context);
  constexpr int delay = 5;
  setSceneCheckItemDelayMs(delay);
  const QString id = fixture.submit("unused_styles").value("job_id").toString();
  fixture.finish(id);
  setSceneCheckItemDelayMs(0);
  const SceneCheckSteps steps = sceneCheckSteps(context, id);
  // Each item takes at least `delay`, so a step that stops once the slice has
  // passed holds at most slice / delay items, however slow the machine.
  require(steps.longestStepItems <= SceneCheckSliceMs / delay,
          "A scan step kept the application thread past its slice");
  require(steps.steps >= Drawings * delay / (SceneCheckSliceMs + delay),
          "The scan did not yield between steps");
}

void cancelEndsAtOnceWithRows(Context& context) {
  Fixture fixture(context);
  setSceneCheckItemDelayMs(5);
  const QString id = fixture.submit("unused_styles").value("job_id").toString();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
  const QJsonObject canceled = fixture.job("job.cancel", id);
  setSceneCheckItemDelayMs(0);
  require(canceled.value("status") == "canceled" &&
              canceled.value("terminal").toBool(),
          "cancel() did not end the scan at once");
  const QJsonObject result = fixture.job("job.result", id);
  require(!result.value("complete").toBool() &&
              result.value("reasons").toArray() == QJsonArray{"canceled"},
          "A canceled scan did not keep its rows marked incomplete");
}

void writeEndsTheScan(Context& context) {
  Fixture fixture(context);
  setSceneCheckItemDelayMs(5);
  const QString id = fixture.submit("unused_styles").value("job_id").toString();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
  ++fixture.document.revision;
  const QJsonObject ended = fixture.finish(id);
  setSceneCheckItemDelayMs(0);
  require(
      ended.value("status") == "failed" &&
          ended.value("failure").toObject().value("code") == "revision_changed",
      "A write during the scan did not fail it with revision_changed");
}

// A level exposed only inside a sub-xsheet, which the top sheet exposes in its
// second column.
QString childSheetLevel(Fixture& fixture) {
  ToonzScene& scene = *fixture.document.scene;
  TXshChildLevel* child =
      scene.createNewLevel(CHILD_XSHLEVEL, L"Sub")->getChildLevel();
  TXshSimpleLevel* inner =
      scene.createNewLevel(PLI_XSHLEVEL, L"Inner")->getSimpleLevel();
  require(child && inner, "Cannot create the child-sheet fixture");
  inner->setFrame(TFrameId(1), new TVectorImage);
  child->getXsheet()->setCell(0, 0, TXshCell(inner, TFrameId(1)));
  scene.getTopXsheet()->setCell(0, 1, TXshCell(child, TFrameId(1)));
  return fixture.document.idForLevel(inner);
}

void inventoryListsLevelRows(Context& context) {
  Fixture fixture(context);
  const QString inner = childSheetLevel(fixture);
  const QString id    = fixture.submit("inventory").value("job_id").toString();
  require(fixture.finish(id).value("status") == "completed",
          "The inventory failed");
  const QJsonObject result = fixture.job("job.result", id);
  const QJsonArray rows    = result.value("rows").toArray();
  require(!rows.isEmpty() &&
              rows.first().toObject().value("row_kind") == "dependency",
          "The inventory listed no dependency rows");
  require(rows.first().toObject().value("sheet_path").toArray().isEmpty(),
          "A top-sheet level carried a child sheet path");
  bool nested = false;
  for (const QJsonValue& value : rows) {
    const QJsonObject row = value.toObject();
    if (row.value("item").toObject().value("owner_id") != inner) continue;
    nested = row.value("sheet_path").toArray().size() == 1;
  }
  require(nested, "A level inside a sub-xsheet did not name that sheet");
}

void secondCheckIsBusy(Context& context) {
  Fixture fixture(context);
  setSceneCheckItemDelayMs(5);
  const QString id = fixture.submit("unused_styles").value("job_id").toString();
  bool busy        = false;
  try {
    fixture.submit("inventory");
  } catch (const Error& error) {
    busy = error.code == "analysis_busy" &&
           error.details.value("active_job_id") == id;
  }
  fixture.job("job.cancel", id);
  setSceneCheckItemDelayMs(0);
  require(busy, "A second check started while one ran");
}

}  // namespace

void runAutomationSceneCheckTests(Context& context) {
  const std::pair<const char*, std::function<void(Context&)>> cases[]{
      {"unused styles match SceneChecker", unusedStylesMatchSceneChecker},
      {"steps stay short", stepsStayShort},
      {"cancel ends at once with rows", cancelEndsAtOnceWithRows},
      {"a write ends the scan", writeEndsTheScan},
      {"inventory lists level rows", inventoryListsLevelRows},
      {"a second check is busy", secondCheckIsBusy}};
  for (const auto& test : cases) {
    test.second(context);
    std::cout << "automation-scene-check-test: " << test.first << " PASS\n";
  }
}

}  // namespace GroundzeroAutomation
