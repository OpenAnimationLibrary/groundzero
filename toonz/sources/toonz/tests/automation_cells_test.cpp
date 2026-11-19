#include "automation_cell_retime.h"
#include "automation_cells.h"

#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"

#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>

namespace GroundzeroAutomation {
namespace {

constexpr unsigned Seed = 20261119;
constexpr int Cases     = 10000;

void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error("cells: " + message);
}

struct Fixture {
  std::unique_ptr<ToonzScene> scene;
  TXshLevel* level = nullptr;

  explicit Fixture(Context& context) : scene(std::make_unique<ToonzScene>()) {
    scene->setProject(context.project());
    scene->setScenePath(context.project()->getScenesPath() + "cells.tnz");
    level = scene->createNewLevel(PLI_XSHLEVEL, L"Cells");
    require(level != nullptr, "cannot create the fixture level");
  }
  TXsheet& sheet() { return *scene->getXsheet(); }
};

// A random primitive near the cells, blanks and drawings 1 to 3 included.
CellOp randomOp(std::mt19937& random, TXshLevel* level) {
  std::uniform_int_distribution<int> kind(0, 3), row(0, 40), count(1, 12),
      drawing(0, 3);
  CellOp op{CellOp::Kind(kind(random)), row(random), count(random), {}};
  if (op.kind == CellOp::Write)
    for (int index = 0; index < op.count; ++index) {
      const int number = drawing(random);
      op.cells.push_back(number ? TXshCell(level, TFrameId(number))
                                : TXshCell());
    }
  return op;
}

void applyDirect(TXsheet& sheet, const CellOp& op) {
  switch (op.kind) {
  case CellOp::Write:
    sheet.setCells(op.row, 0, op.count, op.cells.data());
    break;
  case CellOp::Clear:
    sheet.clearCells(op.row, 0, op.count);
    break;
  case CellOp::Remove:
    sheet.removeCells(op.row, 0, op.count);
    break;
  case CellOp::Insert:
    sheet.insertCells(op.row, 0, op.count);
    break;
  case CellOp::Composite:
    op.native(sheet, 0);
    break;
  }
}

// P-WS-1: the model of each primitive equals the native column on
// generated sequences; the seed is fixed and printed on failure.
void testModelMatchesNative(Context& context) {
  std::mt19937 random(Seed);
  Fixture fixture(context);
  CellPlan plan;
  for (int index = 0; index < Cases; ++index) {
    if (index % 50 == 0) {
      fixture.sheet().clearCells(0, 0, 100);
      plan.after = CellModel::read(fixture.sheet(), 0);
    }
    const CellOp op = randomOp(random, fixture.level);
    plan.before     = plan.after;
    plan.ops        = {op};
    modelCellPlan(plan);
    applyDirect(fixture.sheet(), op);
    const int row =
        CellModel::read(fixture.sheet(), 0).firstDifference(plan.after);
    require(row < 0, "model and native differ at case " +
                         std::to_string(index) + " row " + std::to_string(row) +
                         " (seed " + std::to_string(Seed) + ", kind " +
                         std::to_string(int(op.kind)) + ")");
  }
}

void testEdges() {
  CellModel cells;
  cells.insert(0, 5);
  require(cells.empty(), "insert into an empty column changes nothing");
  TXshCell held;
  cells.write(10, std::vector<TXshCell>(3, held));
  require(cells.empty(), "writing blanks stores nothing");
}

// SCN-07 F3: a native write that refuses is reported, never success.
void testRefusedWriteIsReported(Context& context) {
  Fixture fixture(context);
  Document& document = context.addDocument(std::move(fixture.scene));
  const QString id   = document.id;
  TXsheet& sheet     = *document.scene->getXsheet();
  sheet.setCell(0, 0, TXshCell(fixture.level, TFrameId(1)));
  sheet.getColumn(0)->lock(true);
  CellPlan plan;
  plan.column   = sheet.getColumn(0);
  plan.index    = 0;
  plan.columnId = "locked";
  plan.before   = CellModel::read(sheet, 0);
  plan.ops = {{CellOp::Write, 4, 2,
               std::vector<TXshCell>(2, TXshCell(fixture.level, TFrameId(2)))}};
  modelCellPlan(plan);
  std::vector<CellPlan> plans{plan};
  bool refused = false;
  try {
    applyCellPlans(document, plans);
  } catch (const Error& error) {
    refused = error.code == "native_error";
  }
  context.removeDocument(id);
  require(refused, "a refused native write must be a native_error");
}

// A native result that differs from the model is reported even when every
// native call returned true.
void testDivergenceIsReported(Context& context) {
  Fixture fixture(context);
  Document& document = context.addDocument(std::move(fixture.scene));
  const QString id   = document.id;
  TXsheet& sheet     = *document.scene->getXsheet();
  sheet.setCell(0, 0, TXshCell(fixture.level, TFrameId(1)));
  CellPlan plan;
  plan.column   = sheet.getColumn(0);
  plan.index    = 0;
  plan.columnId = "diverging";
  plan.before   = CellModel::read(sheet, 0);
  plan.ops      = {{CellOp::Clear, 0, 1, {}}};
  modelCellPlan(plan);
  plan.after = plan.before;
  std::vector<CellPlan> plans{plan};
  bool reported = false;
  try {
    applyCellPlans(document, plans);
  } catch (const Error& error) {
    reported = error.code == "native_error" &&
               error.details.value("frame").toInt() == 0;
  }
  context.removeDocument(id);
  require(reported, "a native result unlike the model must be reported");
}

RetimeRequest randomRetime(std::mt19937& random) {
  std::uniform_int_distribution<int> kind(0, 11), start(0, 20), length(1, 12),
      factor(2, 4), blank(-1, 2), coin(0, 1), extra(1, 10);
  RetimeRequest request;
  request.kind           = RetimeKind(kind(random));
  request.start          = start(random);
  request.length         = length(random);
  request.factor         = factor(random);
  request.withBlank      = blank(random);
  request.extendOverHold = coin(random) == 1;
  request.until          = request.start + request.length - 1 + extra(random);
  request.newLength      = length(random);
  if (request.kind == RetimeKind::Reframe) request.factor -= 1;
  if (request.kind == RetimeKind::Swing && request.length < 2)
    request.length = 2;
  return request;
}

// A column of holds, blanks and drawings 1 to 3 over rows 0 to 40.
void randomColumn(std::mt19937& random, TXsheet& sheet, TXshLevel* level) {
  std::uniform_int_distribution<int> drawing(0, 3), hold(1, 4);
  sheet.clearCells(0, 0, 200);
  for (int row = 0; row < 40;) {
    const int number = drawing(random);
    const int length = hold(random);
    for (int index = 0; index < length; ++index, ++row)
      sheet.setCell(row, 0,
                    number ? TXshCell(level, TFrameId(number)) : TXshCell());
  }
}

// P-WS-2: each retime port equals its native range function on one column.
void testRetimeMatchesNative(Context& context) {
  std::mt19937 random(Seed + 1);
  Fixture fixture(context);
  for (int index = 0; index < 3000; ++index) {
    randomColumn(random, fixture.sheet(), fixture.level);
    const RetimeRequest request = randomRetime(random);
    CellPlan plan;
    plan.before = CellModel::read(fixture.sheet(), 0);
    plan.ops    = {retimeOp(request, plan.before)};
    modelCellPlan(plan);
    plan.ops.front().native(fixture.sheet(), 0);
    const int row =
        CellModel::read(fixture.sheet(), 0).firstDifference(plan.after);
    require(row < 0, "retime port and native differ at case " +
                         std::to_string(index) + " kind " +
                         std::to_string(int(request.kind)) + " row " +
                         std::to_string(row) + " (seed " +
                         std::to_string(Seed + 1) + ")");
  }
}

}  // namespace

void runAutomationCellsTests(Context& context) {
  testEdges();
  testModelMatchesNative(context);
  testRefusedWriteIsReported(context);
  testDivergenceIsReported(context);
  testRetimeMatchesNative(context);
  std::cout << "automation-cells-test: model and native PASS" << std::endl;
}

}  // namespace GroundzeroAutomation
