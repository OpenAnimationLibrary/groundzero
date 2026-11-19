// Group and Ungroup inside an entered group of a vector drawing. The native
// group operations used to act only on the outermost group: inside an entered
// group, Group made a new top-level group and Ungroup dissolved the top-level
// group, and the entered group was lost. They now act at the entered group's
// level, keep it entered, and undo exactly. Drawings without an entered group
// keep their behaviour.

#include "tools/imagegrouping.h"
#include "tools/strokeselection.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "toonz/levelset.h"
#include "toonz/palettecontroller.h"
#include "toonz/studiopalette.h"
#include "toonz/tapplication.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonzqt/icongenerator.h"
#include "toonzqt/tselectionhandle.h"
#include "tenv.h"
#include "timagecache.h"
#include "tlevel_io.h"
#include "tnzimage.h"
#include "tpalette.h"
#include "tstroke.h"
#include "tthread.h"
#include "tundo.h"
#include "tvectorimage.h"

#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

class TestTool final : public TTool {
public:
  TestTool() : TTool("T_EnteredGroupRegression") { bind(VectorImage); }
  ToolType getToolType() const override { return GenericTool; }
};

class TestApplication final : public TApplication {
public:
  TestApplication() {
    scene.setScene(new ToonzScene);
    sheet.setXsheet(scene.getScene()->getXsheet());
    frame.setFrame(0);
    column.setColumnIndex(0);
    object.setObjectId(TStageObjectId::ColumnId(0));
    TTool::setApplication(this);
    tool.onImageChanged(TImage::VECTOR);
    tool.setTool("T_EnteredGroupRegression");
    require(tool.getTool() == &nativeTool,
            "the test tool must use the native tool machinery");
  }
  ~TestApplication() override {
    TUndoManager::manager()->reset();
    level.setLevel(nullptr);
    sheet.setXsheet(nullptr);
    scene.setScene(nullptr);
    TTool::setApplication(nullptr);
  }
  TFrameHandle* getCurrentFrame() const override { return &frame; }
  TXshLevelHandle* getCurrentLevel() const override { return &level; }
  TXsheetHandle* getCurrentXsheet() const override { return &sheet; }
  TObjectHandle* getCurrentObject() const override { return &object; }
  TColumnHandle* getCurrentColumn() const override { return &column; }
  TSceneHandle* getCurrentScene() const override { return &scene; }
  ToolHandle* getCurrentTool() const override { return &tool; }
  TSelectionHandle* getCurrentSelection() const override { return &selection; }
  TOnionSkinMaskHandle* getCurrentOnionSkin() const override { return nullptr; }
  TPaletteHandle* getCurrentPalette() const override {
    return palette.getCurrentPalette();
  }
  TFxHandle* getCurrentFx() const override { return nullptr; }
  PaletteController* getPaletteController() const override { return &palette; }
  TColorStyle* getCurrentLevelStyle() const override { return nullptr; }
  int getCurrentLevelStyleIndex() const override { return 0; }
  void setCurrentLevelStyleIndex(int, bool) override {}

  mutable TFrameHandle frame;
  mutable TXshLevelHandle level;
  mutable TXsheetHandle sheet;
  mutable TObjectHandle object;
  mutable TColumnHandle column;
  mutable TSceneHandle scene;
  mutable ToolHandle tool;
  mutable TSelectionHandle selection;
  mutable PaletteController palette;
  TestTool nativeTool;
};

void initialize(const QString& root, const char* executable) {
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     root + "/settings");
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                     root + "/settings");
  TEnv::setApplicationFileName(executable);
  TEnv::setRootVarName("TOONZROOT");
  TEnv::setSystemVarPrefix("TOONZ");
  TEnv::setArgPathValue("TOONZROOT", root.toStdString());
  for (const auto& entry : TEnv::getSystemPathMap()) {
    const QString path =
        QDir(root).filePath(QString::fromStdString(entry.second));
    require(QDir().mkpath(path), "create the private native profile");
    TEnv::setArgPathValue("TOONZ" + entry.first, path.toStdString());
  }
  require(QDir().mkpath(root + "/cache"), "create the private cache");
  TImageCache::instance()->setRootDir(
      TFilePath((root + "/cache").toStdWString()));
  StudioPalette::enable(false);
  TThread::init();
  initImageIo();
}

// A drawing as the PLI loader builds one: the children of a group first, then
// the group. A stroke is named by the x of its first point.
struct Node {
  int name = -1;
  std::vector<Node> children;
};

Node s(int name) {
  Node node;
  node.name = name;
  return node;
}

Node g(std::vector<Node> children) {
  Node node;
  node.children = std::move(children);
  return node;
}

void add(TVectorImage& image, const Node& node) {
  if (node.children.empty()) {
    const double x = node.name;
    auto* stroke   = new TStroke(std::vector<TThickPoint>{
        {x, 0, 1}, {x + 0.25, 0.5, 1}, {x + 0.5, 0, 1}});
    stroke->setStyle(1);
    image.addStroke(stroke);
    return;
  }
  const int first = image.getStrokeCount();
  for (const Node& child : node.children) add(image, child);
  image.group(first, image.getStrokeCount() - first);
}

TVectorImageP build(const TPaletteP& palette, const std::vector<Node>& root) {
  TVectorImageP image = new TVectorImage;
  image->setPalette(palette.getPointer());
  for (const Node& node : root) add(*image, node);
  return image;
}

// Stroke order and the group tree: the depth of every stroke and the depth of
// the deepest group shared by every two strokes.
std::string structure(const TVectorImageP& image) {
  std::ostringstream out;
  const int count = image->getStrokeCount();
  for (int i = 0; i < count; i++) {
    out << image->getStroke(i)->getControlPoint(0).x << ':'
        << image->getGroupDepth(i) << '[';
    for (int j = 0; j < count; j++)
      out << std::min({image->getCommonGroupDepth(i, j),
                       image->getGroupDepth(i), image->getGroupDepth(j)});
    out << "] ";
  }
  return out.str();
}

int indexOf(const TVectorImageP& image, int name) {
  for (int i = 0; i < (int)image->getStrokeCount(); i++)
    if (image->getStroke(i)->getControlPoint(0).x == name) return i;
  throw std::runtime_error("no stroke " + std::to_string(name));
}

void requireStructure(const TVectorImageP& actual, const TVectorImageP& wanted,
                      const std::string& what) {
  require(structure(actual) == structure(wanted),
          what + ":\n  got    " + structure(actual) + "\n  wanted " +
              structure(wanted));
}

TVectorImageP saveAndLoad(const TVectorImageP& image, const QString& path) {
  {
    TLevelWriterP writer{TFilePath(path.toStdWString())};
    writer->getFrameWriter(TFrameId(1))->save(TImageP(image.getPointer()));
  }
  TLevelReaderP reader{TFilePath(path.toStdWString())};
  reader->loadInfo();
  TVectorImageP loaded = reader->getFrameReader(TFrameId(1))->load();
  require(loaded, "the saved drawing does not load");
  return loaded;
}

struct Fixture {
  TestApplication& app;
  QString directory;
  TPaletteP palette = new TPalette;
};

// Shows the drawing in the current cell and enters, `depth` times, the group
// holding stroke `toward`. Returns the image the editor's commands edit.
TVectorImageP show(Fixture& f, const std::vector<Node>& root, int depth,
                   int toward) {
  static int number            = 0;
  const TXshSimpleLevelP level = new TXshSimpleLevel;
  level->setType(PLI_XSHLEVEL);
  level->setName(L"Groups" + std::to_wstring(++number));
  level->setScene(f.app.scene.getScene());
  level->setPalette(f.palette.getPointer());
  f.app.scene.getScene()->getLevelSet()->insertLevel(level.getPointer());
  level->setFrame(TFrameId(1), build(f.palette, root));
  f.app.sheet.getXsheet()->setCell(0, 0,
                                   TXshCell(level.getPointer(), TFrameId(1)));
  f.app.level.setLevel(level.getPointer());
  TVectorImageP image = f.app.tool.getTool()->getImage(true);
  require(image && image.getPointer() == f.app.tool.getTool()->getImage(true),
          "the editor must edit one image of the current cell");
  for (int i = 0; i < depth; i++)
    require(image->enterGroup(indexOf(image, toward)), "enter the group");
  require(image->isInsideGroup() == depth, "the group is entered");
  return image;
}

// Runs an editor command on the strokes named, as the Selection tool's menu
// does, and checks that it made one undo step.
void run(const TVectorImageP& image, const std::vector<int>& names,
         void (TGroupCommand::*command)()) {
  StrokeSelection selection;
  selection.setImage(image);
  for (int name : names) selection.select(indexOf(image, name), true);
  TUndoManager::manager()->reset();
  (selection.getGroupCommand()->*command)();
  require(TUndoManager::manager()->getHistoryCount() == 1,
          "the command made one undo step");
}

// The entered group holds strokes 10 and 11, a subgroup {12, 13} and a
// subgroup {14}. Above depth 1, each ancestor holds one stroke before it, and
// strokes 0 and 20 are outside every group.
std::vector<Node> nest(int depth, const Node& entered) {
  Node node = entered;
  for (int k = depth - 1; k >= 1; k--) node = g({s(k), node});
  return {s(0), node, s(20)};
}

const Node kEntered = g({s(10), s(11), g({s(12), s(13)}), g({s(14)})});

// Every stroke of the entered group is still in it, and no other stroke is.
void requireEntered(const TVectorImageP& image, int depth,
                    const std::string& what) {
  require(image->isInsideGroup() == depth, what + " left the entered group");
  for (int i = 0; i < (int)image->getStrokeCount(); i++) {
    const double name = image->getStroke(i)->getControlPoint(0).x;
    require(image->isEnteredGroupStroke(i) == (name >= 10 && name < 20),
            what + " moved a stroke in or out of the entered group");
  }
}

void insideCase(Fixture& f, int depth, const std::string& what,
                const std::vector<int>& names, void (TGroupCommand::*command)(),
                const Node& wantedEntered) {
  static int saved = 0;
  const std::string label =
      what + " inside a group entered at depth " + std::to_string(depth);
  const TVectorImageP image  = show(f, nest(depth, kEntered), depth, 10);
  const TVectorImageP before = build(f.palette, nest(depth, kEntered));
  const TVectorImageP wanted = build(f.palette, nest(depth, wantedEntered));
  run(image, names, command);
  for (int pass = 0; pass < 2; pass++) {
    requireStructure(image, wanted, label);
    requireEntered(image, depth, label);
    TUndoManager::manager()->undo();
    requireStructure(image, before, label + ", undone");
    requireEntered(image, depth, label + ", undone");
    TUndoManager::manager()->redo();
  }
  const QString path =
      f.directory + "/inside" + QString::number(++saved) + ".pli";
  requireStructure(saveAndLoad(image, path), wanted,
                   label + ", saved and loaded");
  std::cout << "enteredgroup-test: " << label << " PASS" << std::endl;
}

// Ungrouping {14} would leave the entered group's own strokes 10 to 13 and 14
// apart, which native code reorders: Ungroup is not offered and does nothing.
void refusedUngroup(Fixture& f, int depth) {
  const std::string label =
      "Ungroup that would split the entered group's own strokes at depth " +
      std::to_string(depth);
  const std::vector<Node> drawing =
      nest(depth, g({s(10), s(11), s(12), s(13), g({s(15)}), g({s(14)})}));
  const TVectorImageP image = show(f, drawing, depth, 10);
  StrokeSelection selection;
  selection.setImage(image);
  selection.select(indexOf(image, 14), true);
  require(!(selection.getGroupCommand()->getGroupingOptions() &
            TGroupCommand::UNGROUP),
          label + " is offered");
  TUndoManager::manager()->reset();
  selection.getGroupCommand()->ungroup();
  require(TUndoManager::manager()->getHistoryCount() == 0,
          label + " made an undo step");
  requireStructure(image, build(f.palette, drawing), label + " changed it");
  requireEntered(image, depth, label);
  std::cout << "enteredgroup-test: " << label << " refused PASS" << std::endl;
}

// Without an entered group, and with an entered group that has no subgroups,
// Group and Ungroup do what they did before.
void unchangedCases(Fixture& f) {
  const std::vector<Node> drawing = {s(0), s(1), g({s(2), s(3)}), s(4)};
  TVectorImageP image             = show(f, drawing, 0, 0);
  run(image, {0, 1}, &TGroupCommand::group);
  requireStructure(image,
                   build(f.palette, {g({s(0), s(1)}), g({s(2), s(3)}), s(4)}),
                   "Group at the top level");
  run(image, {2, 3}, &TGroupCommand::ungroup);
  requireStructure(image, build(f.palette, {g({s(0), s(1)}), s(2), s(3), s(4)}),
                   "Ungroup at the top level");
  TUndoManager::manager()->undo();
  requireStructure(image,
                   build(f.palette, {g({s(0), s(1)}), g({s(2), s(3)}), s(4)}),
                   "Ungroup at the top level, undone");

  image = show(f, drawing, 1, 2);
  run(image, {2, 3}, &TGroupCommand::ungroup);
  requireStructure(image, build(f.palette, {s(0), s(1), s(2), s(3), s(4)}),
                   "Ungroup of an entered group's own strokes");
  require(image->isInsideGroup() == 0,
          "Ungroup of an entered group's own strokes leaves the group");
  TUndoManager::manager()->undo();
  requireStructure(image, build(f.palette, drawing),
                   "Ungroup of an entered group's own strokes, undone");
  std::cout << "enteredgroup-test: drawings without nesting unchanged PASS"
            << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QTemporaryDir root;
  if (!root.isValid()) {
    std::cerr << "enteredgroup-test: cannot create the private test profile\n";
    return 1;
  }
  qputenv("TOONZROOT", root.path().toUtf8());
  qputenv("TOONZPROFILES", (root.path() + "/profiles").toUtf8());
  QApplication application(argc, argv);
  int result = 0;
  try {
    require(QDir().mkpath(root.path() + "/profiles"),
            "create the private profile path");
    initialize(root.path(), argv[0]);
    TestApplication native;
    Fixture fixture{native, root.path()};
    try {
      for (int depth = 1; depth <= 3; depth++) {
        insideCase(fixture, depth, "Group of two subgroups", {12, 13, 14},
                   &TGroupCommand::group,
                   g({s(10), s(11), g({g({s(12), s(13)}), g({s(14)})})}));
        insideCase(fixture, depth, "Group of the group's own strokes", {10, 11},
                   &TGroupCommand::group,
                   g({g({s(10), s(11)}), g({s(12), s(13)}), g({s(14)})}));
        insideCase(fixture, depth, "Ungroup of a subgroup", {12, 13},
                   &TGroupCommand::ungroup,
                   g({s(10), s(11), s(12), s(13), g({s(14)})}));
        refusedUngroup(fixture, depth);
      }
      unchangedCases(fixture);
    } catch (const std::exception& error) {
      std::cerr << "enteredgroup-test: " << error.what() << '\n';
      result = 1;
    }
    // The commands queue drawing icons: finish them while the application
    // and the scene are alive
    IconGenerator::instance()->shutdown();
    native.level.setLevel(nullptr);
  } catch (const std::exception& error) {
    std::cerr << "enteredgroup-test: " << error.what() << '\n';
    result = 1;
  }
  TImageCache::instance()->clear(true);
  return result;
}
