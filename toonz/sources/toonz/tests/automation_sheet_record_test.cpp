#include "automation_sheet_record.h"

#include "automation.h"

#include "tdoubleparam.h"
#include "toutputproperties.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QJsonArray>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace GroundzeroAutomation {
namespace {

void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error("sheet record: " + message);
}

// Two level columns: drawings 1, 1, 2, blank, 2 and a hold of 3; one camera.
std::unique_ptr<ToonzScene> fixture(Context& context) {
  auto scene = std::make_unique<ToonzScene>();
  scene->setProject(context.project());
  scene->setScenePath(context.project()->getScenesPath() + "sheet_record.tnz");
  TXshLevel* level = scene->createNewLevel(PLI_XSHLEVEL, L"Record");
  require(level != nullptr, "cannot create the fixture level");
  TXsheet* sheet = scene->getXsheet();
  const TXshCell one(level, TFrameId(1));
  const TXshCell two(level, TFrameId(2));
  for (int row : {0, 1}) sheet->setCell(row, 0, one);
  for (int row : {2, 4}) sheet->setCell(row, 0, two);
  for (int row = 0; row < 6; ++row)
    sheet->setCell(row, 1, TXshCell(level, TFrameId(3)));
  return scene;
}

void requireRefused(Context& context, const QString& section,
                    const std::function<void(ToonzScene&)>& change) {
  auto saved               = fixture(context);
  const QJsonObject record = automationSheetRecord(*saved);
  auto reopened            = fixture(context);
  requireAutomationSheetRecord(record, *reopened);
  change(*reopened);
  try {
    requireAutomationSheetRecord(record, *reopened);
  } catch (const Error& error) {
    require(error.code == "invalid_package", "wrong refusal code");
    require(error.details.value("section").toString() == section,
            "wrong section for a change in " + section.toStdString() + ": " +
                error.details.value("path").toString().toStdString());
    return;
  }
  require(false, "a change in " + section.toStdString() + " was accepted");
}

void testRuns(Context& context) {
  auto scene = fixture(context);
  const QJsonObject sheet =
      automationSheetRecord(*scene).value("sheet").toObject();
  const QJsonArray columns = sheet.value("columns").toArray();
  require(columns.size() == 2, "two saved columns expected");
  require(columns.at(0).toObject().value("cell_runs").toInt() == 3,
          "1-1, 2, 2 separated by a blank are three runs");
  require(columns.at(1).toObject().value("cell_runs").toInt() == 1,
          "a hold is one run");
  scene->getXsheet()->insertColumn(2);
  const QJsonObject after =
      automationSheetRecord(*scene).value("sheet").toObject();
  try {
    requireEqualRecord("sheet", sheet, after);
  } catch (const Error& error) {
    require(false, "a trailing empty column changed the record at " +
                       error.details.value("path").toString().toStdString());
  }
}

void testDifferencePath(Context& context) {
  auto saved               = fixture(context);
  const QJsonObject record = automationSheetRecord(*saved);
  auto reopened            = fixture(context);
  reopened->getXsheet()->getColumn(1)->setCamstandVisible(false);
  try {
    requireAutomationSheetRecord(record, *reopened);
  } catch (const Error& error) {
    require(error.details.value("path").toString() ==
                "sheet.columns[1].camstand_visible",
            "the first differing path names the column and field");
    require(error.details.value("saved").toString() == "[true]",
            "the saved value is reported");
    return;
  }
  require(false, "a camstand change was accepted");
}

void testRefusals(Context& context) {
  requireRefused(context, "sheet", [](ToonzScene& scene) {
    scene.getXsheet()->getColumn(0)->setPreviewVisible(false);
  });
  requireRefused(context, "sheet", [](ToonzScene& scene) {
    TXsheet* sheet = scene.getXsheet();
    sheet->setCell(3, 0, sheet->getCell(2, 0));
  });
  requireRefused(context, "sheet", [](ToonzScene& scene) {
    scene.getXsheet()->getColumn(1)->lock(true);
  });
  requireRefused(context, "cameras", [](ToonzScene& scene) {
    scene.getCurrentCamera()->setRes(TDimension(640, 480));
  });
  requireRefused(context, "cameras", [](ToonzScene& scene) {
    TStageObjectTree* objects = scene.getXsheet()->getStageObjectTree();
    TStageObject* camera =
        objects->getStageObject(objects->getCurrentCameraId(), false);
    camera->getParam(TStageObject::T_X)->setValue(12, 3.0);
  });
  requireRefused(context, "scene_properties", [](ToonzScene& scene) {
    scene.getProperties()->setBgColor(TPixel32(1, 2, 3, 4));
  });
  requireRefused(context, "scene_properties", [](ToonzScene& scene) {
    scene.getProperties()->getPreviewProperties()->setRange(0, 9, 1);
  });
  requireRefused(context, "scene_properties", [](ToonzScene& scene) {
    scene.getProperties()->getVGuides().push_back(4.5);
  });
}

}  // namespace

void runAutomationSheetRecordTests(Context& context) {
  testRuns(context);
  testDifferencePath(context);
  testRefusals(context);
}

}  // namespace GroundzeroAutomation
