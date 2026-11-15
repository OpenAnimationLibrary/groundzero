// SPDX-License-Identifier: BSD-3-Clause

#include "toonz/childstack.h"
#include "toonz/levelset.h"
#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "tfilepath.h"

#include <QCoreApplication>
#include <QPointer>

#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct NestedScene {
  std::unique_ptr<ToonzScene> scene = std::make_unique<ToonzScene>();
  TXsheetP root                     = scene->getTopXsheet();
  TXsheetP child, grandchild;
  QPointer<TXshChildLevel> childLevel, grandchildLevel;
  QPointer<TXshSimpleLevel> leaf;

  NestedScene() {
    scene->setScenePath(TFilePath("childstack-lifetime.tnz"));
    auto* first = new TXshChildLevel(L"Child");
    first->setScene(scene.get());
    scene->getLevelSet()->insertLevel(first);
    childLevel = first;
    child      = first->getXsheet();
    root->setCell(3, 1, TXshCell(first, TFrameId(1)));

    auto* second = new TXshChildLevel(L"Grandchild");
    second->setScene(scene.get());
    scene->getLevelSet()->insertLevel(second);
    grandchildLevel = second;
    grandchild      = second->getXsheet();
    child->setCell(5, 2, TXshCell(second, TFrameId(1)));

    auto* drawing = new TXshSimpleLevel(L"Leaf");
    drawing->setType(PLI_XSHLEVEL);
    drawing->setScene(scene.get());
    scene->getLevelSet()->insertLevel(drawing);
    leaf = drawing;
    grandchild->setCell(7, 0, TXshCell(drawing, TFrameId(1)));
  }

  void enter() {
    auto* stack = scene->getChildStack();
    require(stack->openChild(3, 1), "cannot enter the child sheet");
    require(stack->openChild(5, 2), "cannot enter the grandchild sheet");
    require(stack->getAncestorCount() == 2 &&
                stack->getXsheet() == grandchild.getPointer(),
            "nested navigation did not select the grandchild");
    require(root->getRefCount() == 2 && child->getRefCount() == 2 &&
                grandchild->getRefCount() == 2,
            "navigation changed the owned root or borrowed child references");
  }

  void checkReleasedScene() {
    require(root->getRefCount() == 1,
            "scene destruction retained its owned root sheet reference");
    require(child->getRefCount() == 2 && grandchild->getRefCount() == 2,
            "scene destruction released a borrowed child sheet reference");
    root = nullptr;
    require(!childLevel && child->getRefCount() == 1,
            "releasing the observed root did not destroy its child level");
    child = nullptr;
    require(
        !grandchildLevel && grandchild->getRefCount() == 1,
        "releasing the observed child did not destroy its grandchild level");
    grandchild = nullptr;
    require(!leaf, "releasing the last sheet retained an exposed level");
  }
};

void destroyWhileNested() {
  NestedScene fixture;
  fixture.enter();
  fixture.scene.reset();
  fixture.checkReleasedScene();
}

void closeBeforeDestruction() {
  NestedScene fixture;
  fixture.enter();
  auto* stack = fixture.scene->getChildStack();
  int row = -1, col = -1;
  require(stack->closeChild(row, col) && row == 5 && col == 2,
          "closing the grandchild changed its parent cell");
  require(stack->closeChild(row, col) && row == 3 && col == 1,
          "closing the child changed its parent cell");
  require(!stack->closeChild(row, col) &&
              stack->getXsheet() == fixture.root.getPointer(),
          "closing nested sheets did not return to the original root");
  fixture.scene.reset();
  fixture.checkReleasedScene();
}

void clearNavigationWhileNested() {
  NestedScene fixture;
  fixture.enter();
  auto* stack = fixture.scene->getChildStack();
  stack->clear();
  require(stack->getAncestorCount() == 0 &&
              stack->getXsheet() == fixture.root.getPointer(),
          "clear discarded the owned root while inside a child sheet");
  require(
      fixture.root->getFrameCount() == 0 && fixture.root->getColumnCount() == 0,
      "clear did not empty the root sheet");
  require(fixture.child->getCell(5, 2).getChildLevel() ==
                  fixture.grandchildLevel.data() &&
              fixture.grandchild->getCell(7, 0).getSimpleLevel() ==
                  fixture.leaf.data(),
          "clearing root navigation changed separately owned child contents");
  require(fixture.child->getRefCount() == 2 &&
              fixture.grandchild->getRefCount() == 2,
          "clear consumed a child level's sheet reference");
  fixture.scene.reset();
  require(fixture.root->getRefCount() == 1 && !fixture.childLevel,
          "cleared scene retained the root or its unexposed child level");
  fixture.root = nullptr;
  require(fixture.child->getRefCount() == 1,
          "cleared scene did not release its child level's sheet reference");
  fixture.child = nullptr;
  require(!fixture.grandchildLevel && fixture.grandchild->getRefCount() == 1,
          "cleared scene retained the grandchild level");
  fixture.grandchild = nullptr;
  require(!fixture.leaf, "cleared scene retained its leaf level");
}

void clearSceneWhileNested() {
  NestedScene fixture;
  fixture.enter();
  fixture.scene->clear();
  require(fixture.scene->getXsheet() == fixture.root.getPointer() &&
              fixture.scene->getChildStack()->getAncestorCount() == 0,
          "scene clear lost the original root sheet");
  require(fixture.scene->getLevelSet()->getLevelCount() == 0 &&
              fixture.root->getColumnCount() == 0 && !fixture.childLevel,
          "scene clear retained its cast or root exposures");
  fixture.scene->setScenePath(TFilePath("childstack-lifetime.tnz"));
  fixture.scene->clear();
  require(fixture.root->getRefCount() == 2,
          "repeated scene clear changed root ownership");
  fixture.scene.reset();
  require(
      fixture.root->getRefCount() == 1 && fixture.child->getRefCount() == 1,
      "scene destruction after clear lost the root/child ownership balance");
  fixture.root  = nullptr;
  fixture.child = nullptr;
  require(!fixture.grandchildLevel && fixture.grandchild->getRefCount() == 1,
          "scene clear retained nested navigation ownership");
  fixture.grandchild = nullptr;
  require(!fixture.leaf, "scene clear retained its last exposed level");
}
}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  try {
    for (int cycle = 0; cycle < 32; ++cycle) {
      destroyWhileNested();
      closeBeforeDestruction();
      clearNavigationWhileNested();
      clearSceneWhileNested();
    }
    std::cout
        << "childstack_lifetime: PASS 32 nested destroy/close/clear cycles"
        << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "childstack_lifetime: " << error.what() << std::endl;
    return 1;
  }
}
