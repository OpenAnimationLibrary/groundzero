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

}  // namespace

void runAutomationCellsTests(Context& context) {
  testEdges();
  testModelMatchesNative(context);
  testRefusedWriteIsReported(context);
  std::cout << "automation-cells-test: model and native PASS" << std::endl;
}

}  // namespace GroundzeroAutomation
