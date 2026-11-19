#include "automation_level_lifecycle.h"

#include "toonz/hook.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/sceneproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "tpalette.h"
#include "trasterimage.h"
#include "tvectorimage.h"

#include <QJsonArray>
#include <QJsonObject>

#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

namespace GroundzeroAutomation {
namespace {

void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error("level lifecycle: " + message);
}

// A vector level `Art` with drawings 1 and 2, exposed on rows 0-2 of the top
// sheet's first column and on rows 5-6 of a sub-xsheet `Sub`, which the top
// sheet exposes in its second column.
struct Fixture final {
  Context& context;
  Document& document;
  TXshSimpleLevel* level = nullptr;
  TXshChildLevel* child  = nullptr;

  // Without `nested`, no sub-xsheet: the worker's asset accounting, which
  // level.duplicate runs, admits simple levels only.
  explicit Fixture(Context& owner, bool nested = true)
      : context(owner)
      , document(owner.addDocument(std::make_unique<ToonzScene>())) {
    ToonzScene& scene = *document.scene;
    scene.setScenePath(context.project()->getScenesPath() + "levels.tnz");
    level = scene.createNewLevel(PLI_XSHLEVEL, L"Art")->getSimpleLevel();
    require(level != nullptr, "cannot create the fixture level");
    level->setFrame(TFrameId(1), new TVectorImage);
    level->setFrame(TFrameId(2), new TVectorImage);
    TXsheet& top = *scene.getTopXsheet();
    for (int row = 0; row < 3; ++row)
      top.setCell(row, 0, TXshCell(level, TFrameId(row == 1 ? 2 : 1)));
    if (!nested) return;
    child = scene.createNewLevel(CHILD_XSHLEVEL, L"Sub")->getChildLevel();
    require(child != nullptr, "cannot create the fixture sub-xsheet");
    for (int row = 5; row < 7; ++row)
      child->getXsheet()->setCell(row, 0, TXshCell(level, TFrameId(1)));
    top.setCell(0, 1, TXshCell(child, TFrameId(1)));
  }
  ~Fixture() { context.removeDocument(document.id); }
};

// P-LV-7: the scan finds the nested exposures the native walk finds, with
// the sheet that holds them; a drawing filter narrows it.
void usageCoversNestedSheets(Context& context) {
  Fixture fixture(context);
  std::set<TXshLevel*> native;
  fixture.document.scene->getTopXsheet()->getUsedLevels(native);
  require(native.count(fixture.level) == 1, "native walk misses the level");
  const LevelUsage usage = scanLevelUsage(fixture.document, fixture.level);
  require(usage.complete && usage.sheets == 2, "scan did not visit 2 sheets");
  require(usage.cells == 5 && usage.columns.size() == 2,
          "scan did not find 5 cells in 2 columns");
  require(!usage.columns[0].sheetLevel &&
              usage.columns[1].sheetLevel == fixture.child,
          "scan did not name the sheet of each column");
  const std::set<TFrameId> second{TFrameId(2)};
  const LevelUsage filtered =
      scanLevelUsage(fixture.document, fixture.level, &second);
  require(
      filtered.cells == 1 && filtered.columns[0].rows == std::vector<int>{1},
      "a drawing filter did not narrow the scan");
  const QJsonObject json = levelUsageJson(fixture.document, usage);
  const QJsonArray rows  = json.value("rows").toArray();
  require(rows.size() == 2 &&
              rows[0].toObject().value("column_id").isString() &&
              rows[1].toObject().value("column_id").isNull() &&
              rows[1].toObject().value("sheet_level_id").isString(),
          "usage rows do not name top and nested columns correctly");
}

// A lock on a nested column counts: the change would reach it.
void nestedLockCounts(Context& context) {
  Fixture fixture(context);
  fixture.child->getXsheet()->getColumn(0)->lock(true);
  const LevelUsage usage = scanLevelUsage(fixture.document, fixture.level);
  require(usage.locked(), "a locked nested column was not reported");
  bool refused = false;
  try {
    requireEditableUsage(fixture.document, usage);
  } catch (const Error& error) {
    refused = error.code == "locked" &&
              error.details.value("columns").toArray().size() == 1;
  }
  require(refused, "a locked nested exposure did not refuse with locked");
}

// More sub-xsheets than the bound make the scan incomplete, and an
// incomplete scan refuses every change.
void sheetBoundMakesScanIncomplete(Context& context) {
  Fixture fixture(context);
  for (int index = 0; index < 128; ++index)
    fixture.document.scene->createNewLevel(CHILD_XSHLEVEL,
                                           L"Extra" + std::to_wstring(index));
  const LevelUsage usage = scanLevelUsage(fixture.document, fixture.level);
  require(!usage.complete && usage.reason == "sheet_limit",
          "129 sheets did not stop the scan");
  bool refused = false;
  try {
    requireEditableUsage(fixture.document, usage);
  } catch (const Error& error) {
    refused = error.code == "resource_limit";
  }
  require(refused, "an incomplete scan did not refuse with resource_limit");
}

// Applicability follows the native Level Settings popup.
void settingsFollowNativeApplicability(Context& context) {
  Fixture fixture(context);
  const LevelUsage usage = scanLevelUsage(fixture.document, fixture.level);
  const QJsonObject vector =
      levelSettingsReport(fixture.document, fixture.level, usage);
  for (const char* name : {"dpi", "premultiply", "white_transparent",
                           "antialias_softness", "subsampling"})
    require(vector.value(name).toObject().value("reason") ==
                "not_applicable_to_format",
            std::string("vector level admits ") + name);
  TXshSimpleLevel* raster =
      fixture.document.scene->createNewLevel(OVL_XSHLEVEL, L"Paint")
          ->getSimpleLevel();
  raster->getProperties()->setDirtyFlag(true);
  const QJsonObject rgba = levelSettingsReport(
      fixture.document, raster, scanLevelUsage(fixture.document, raster));
  require(rgba.value("premultiply").toObject().value("writable").toBool(),
          "rgba8 premultiply is not writable");
  require(
      rgba.value("subsampling").toObject().value("reason") == "unsaved_level",
      "a dirty level admits subsampling");
  require(rgba.value("color_space_gamma").toObject().value("reason") ==
              "linear_raster_only",
          "an rgba8 level admits colour-space gamma");
}

QJsonValue call(Fixture& fixture, const QString& method, QJsonObject params) {
  params.insert("document_id", fixture.document.id);
  return executeLevelLifecycle(fixture.context, method, params);
}

QString refusal(Fixture& fixture, const QString& method,
                const QJsonObject& params, QJsonObject* details = nullptr) {
  try {
    call(fixture, method, params);
  } catch (const Error& error) {
    if (details) *details = error.details;
    return error.code;
  }
  return QString();
}

TXshSimpleLevel* vectorLevel(Fixture& fixture, const std::wstring& name,
                             int drawings) {
  TXshSimpleLevel* level =
      fixture.document.scene->createNewLevel(PLI_XSHLEVEL, name)
          ->getSimpleLevel();
  for (int number = 1; number <= drawings; ++number)
    level->setFrame(TFrameId(number), new TVectorImage);
  return level;
}

bool levelInCast(Fixture& fixture, const std::wstring& name) {
  return fixture.document.scene->getLevelSet()->getLevel(name) != nullptr;
}

// SCN-05 #3: a level used in a nested sheet is refused with its usage, and
// clear blanks every exposure in every sheet before the removal.
void removeClearsNestedExposures(Context& context) {
  Fixture fixture(context);
  const QString id = fixture.document.idForLevel(fixture.level);
  TSceneProperties::CastCollection collection{
      "c1", "Heroes", {fixture.level->getCastId()}};
  fixture.document.scene->getProperties()->setCastCollections({collection});
  QJsonObject details;
  require(refusal(fixture, "level.remove",
                  {{"level_id", id}, {"exposure_policy", "refuse"}},
                  &details) == "in_use",
          "a used level was not refused");
  require(
      details.value("usage").toObject().value("sheets").toInt() == 2 &&
          details.value("usage").toObject().value("rows").toArray().size() == 2,
      "the refusal did not list the nested usage");
  const QJsonObject result =
      call(fixture, "level.remove",
           {{"level_id", id}, {"exposure_policy", "clear"}})
          .toObject();
  require(result.value("cells_cleared").toInt() == 5,
          "clear did not report 5 cells");
  require(result.value("collection_memberships_dormant").toInt() == 1 &&
              fixture.document.scene->getProperties()
                      ->getCastCollections()
                      .front()
                      .members.size() == 1,
          "the collection membership was not kept dormant");
  require(!levelInCast(fixture, L"Art"), "the level is still in the cast");
  require(fixture.document.levels.value(id) == nullptr,
          "the removed level's handle was kept");
  TXsheet& top = *fixture.document.scene->getTopXsheet();
  require(top.getCell(0, 0).isEmpty() && top.getCell(2, 0).isEmpty() &&
              fixture.child->getXsheet()->getCell(5, 0).isEmpty() &&
              top.getCell(0, 1).getChildLevel() == fixture.child,
          "clear left an exposure or touched the sub-xsheet cell");
}

// Replace rewrites every sheet to the same drawings of the replacement and
// refuses a replacement that lacks one or has another format.
void replaceRewritesEverySheet(Context& context) {
  Fixture fixture(context);
  const QString id         = fixture.document.idForLevel(fixture.level);
  TXshSimpleLevel* partial = vectorLevel(fixture, L"Partial", 1);
  QJsonObject details;
  require(
      refusal(fixture, "level.remove",
              {{"level_id", id},
               {"exposure_policy", "replace"},
               {"replacement_level_id", fixture.document.idForLevel(partial)}},
              &details) == "missing_drawing" &&
          details.value("drawings").toArray().size() == 1,
      "a replacement without drawing 2 was not refused");
  TXshSimpleLevel* raster =
      fixture.document.scene->createNewLevel(OVL_XSHLEVEL, L"Raster")
          ->getSimpleLevel();
  const QJsonObject typed{
      {"level_id", id},
      {"exposure_policy", "replace"},
      {"replacement_level_id", fixture.document.idForLevel(raster)}};
  require(refusal(fixture, "level.remove", typed, &details) ==
                  "unsupported_target" &&
              details.value("reason") == "type_change",
          "a replacement of another format was not refused");
  TXshSimpleLevel* alt = vectorLevel(fixture, L"Alt", 2);
  const QJsonObject result =
      call(fixture, "level.remove",
           {{"level_id", id},
            {"exposure_policy", "replace"},
            {"replacement_level_id", fixture.document.idForLevel(alt)}})
          .toObject();
  require(result.value("cells_replaced").toInt() == 5,
          "replace did not report 5 cells");
  TXsheet& top = *fixture.document.scene->getTopXsheet();
  require(top.getCell(1, 0) == TXshCell(alt, TFrameId(2)) &&
              top.getCell(2, 0) == TXshCell(alt, TFrameId(1)) &&
              fixture.child->getXsheet()->getCell(6, 0) ==
                  TXshCell(alt, TFrameId(1)),
          "replace did not keep drawing ids in every sheet");
}

// A child attached to hook 1 of the exposing column blocks clear, and a
// replacement without hook 1.
void hookChildrenBlockRemoval(Context& context) {
  Fixture fixture(context);
  TStageObjectTree* tree =
      fixture.document.scene->getTopXsheet()->getStageObjectTree();
  TStageObject* peg = tree->getStageObject(TStageObjectId::PegbarId(0), true);
  peg->setParent(TStageObjectId::ColumnId(0));
  peg->setParentHandle("H1");
  QJsonObject details;
  require(refusal(fixture, "level.remove",
                  {{"level_id", fixture.document.idForLevel(fixture.level)},
                   {"exposure_policy", "clear"}},
                  &details) == "has_dependents" &&
              details.value("users").toArray().size() == 1,
          "a hook-attached child did not block the removal");
  require(levelInCast(fixture, L"Art"), "a refused removal changed the cast");
}

// The duplicate shares nothing with its source: drawings, palette and hooks
// are copies, and every setting is copied.
void duplicateIsIndependent(Context& context) {
  Fixture fixture(context, false);
  fixture.level->getHookSet()->touchHook(0)->setAPos(TFrameId(1),
                                                     TPointD(1, 2));
  fixture.level->getProperties()->setDoAntialias(7);
  const QJsonObject result =
      call(fixture, "level.duplicate",
           {{"level_id", fixture.document.idForLevel(fixture.level)},
            {"name", "Copy"}})
          .toObject();
  TXshSimpleLevel* copy = fixture.document.scene->getLevelSet()
                              ->getLevel(L"Copy")
                              ->getSimpleLevel();
  require(copy && result.value("hooks_copied").toInt() == 1,
          "the duplicate or its hook is missing");
  require(copy->getFids() == fixture.level->getFids() &&
              copy->getFrame(TFrameId(1), false).getPointer() !=
                  fixture.level->getFrame(TFrameId(1), false).getPointer() &&
              copy->getPalette() != fixture.level->getPalette() &&
              copy->getHookSet()->getHook(0) !=
                  fixture.level->getHookSet()->getHook(0),
          "the duplicate shares drawings, palette or hooks");
  require(copy->getProperties()->antialiasSoftness() == 7 &&
              copy->getCastId() != fixture.level->getCastId(),
          "the duplicate lost a setting or kept the cast id");
  copy->getHookSet()->getHook(0)->setAPos(TFrameId(1), TPointD(5, 5));
  require(fixture.level->getHookSet()->getHook(0)->getAPos(TFrameId(1)) ==
              TPointD(1, 2),
          "editing the duplicate's hook moved the source's");
}

}  // namespace

void runAutomationLevelLifecycleTests(Context& context) {
  usageCoversNestedSheets(context);
  nestedLockCounts(context);
  sheetBoundMakesScanIncomplete(context);
  settingsFollowNativeApplicability(context);
  removeClearsNestedExposures(context);
  replaceRewritesEverySheet(context);
  hookChildrenBlockRemoval(context);
  duplicateIsIndependent(context);
  std::cout << "automation-level-lifecycle-test: usage, settings, removal, "
               "duplicate PASS"
            << std::endl;
}

}  // namespace GroundzeroAutomation
