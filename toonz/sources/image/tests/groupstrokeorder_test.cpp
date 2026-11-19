// Stroke order and group tree of vector drawings through a PLI save and load.
// The loader builds every group with TVectorImage::group(), which moves a
// subgroup when it splits its parent's own strokes: inside a group, the
// group's own strokes must form one run. A drawing that keeps this rule loads
// as it was saved. Merging drawings into an entered group, as the Type tool
// does, broke the rule: an ungrouped stroke after a subgroup split the entered
// group's own strokes, and below depth 1 it got only the innermost id of the
// entered group, so it left that group.

#include "tlevel_io.h"
#include "tnzimage.h"
#include "tenv.h"
#include "tpalette.h"
#include "tstroke.h"
#include "tthread.h"
#include "tvectorimage.h"

#include <QCoreApplication>
#include <QDir>
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

void requireStructure(const TVectorImageP& actual, const TVectorImageP& wanted,
                      const std::string& what) {
  require(structure(actual) == structure(wanted),
          what + ":\n  got    " + structure(actual) + "\n  wanted " +
              structure(wanted));
}

int indexOf(const TVectorImageP& image, int name) {
  for (int i = 0; i < (int)image->getStrokeCount(); i++)
    if (image->getStroke(i)->getControlPoint(0).x == name) return i;
  throw std::runtime_error("no stroke " + std::to_string(name));
}

struct Files {
  QString directory;
  TPaletteP palette = new TPalette;
  int saved         = 0;
};

TVectorImageP saveAndLoad(Files& files, const TVectorImageP& image) {
  const QString path =
      files.directory + "/drawing" + QString::number(++files.saved) + ".pli";
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

// Above depth 1, each ancestor of the inner group holds one stroke before it,
// and strokes 0 and 30 are outside every group.
std::vector<Node> nest(int depth, const Node& inner) {
  Node node = inner;
  for (int k = depth - 1; k >= 1; k--) node = g({s(k), node});
  return {s(0), node, s(30)};
}

void requireRoundTrip(Files& files, const std::vector<Node>& drawing,
                      const std::string& what) {
  const TVectorImageP image = build(files.palette, drawing);
  requireStructure(saveAndLoad(files, image), image, what);
  std::cout << "groupstrokeorder-test: " << what << " PASS" << std::endl;
}

// Drawings that keep the rule load as they were saved, with and without
// nesting.
void unchangedRoundTrips(Files& files) {
  requireRoundTrip(files, {s(0), s(1), s(2)}, "ungrouped strokes");
  requireRoundTrip(
      files, {s(0), g({s(1), s(2)}), s(3), g({s(4)}), g({s(5), s(6)}), s(7)},
      "groups without nesting");
  for (int depth = 1; depth <= 3; depth++) {
    const std::string at = " at depth " + std::to_string(depth);
    requireRoundTrip(
        files, nest(depth, g({s(10), s(11), g({s(12), s(13)}), g({s(14)})})),
        "own strokes before subgroups" + at);
    requireRoundTrip(
        files, nest(depth, g({g({s(12), s(13)}), g({s(14)}), s(10), s(11)})),
        "own strokes after subgroups" + at);
    requireRoundTrip(
        files, nest(depth, g({g({s(12), g({s(13)})}), s(10), g({s(14)})})),
        "own strokes between subgroups" + at);
  }
}

// Merges an ungrouped stroke 15 and a grouped pair {16, 17}, as the Type tool
// adds an ungrouped glyph and a glyph of two strokes, into a group entered at
// `depth` that holds stroke 10 and a subgroup {11, 12}.
void mergeIntoEnteredGroup(Files& files, int depth) {
  const std::string what =
      "merge into a group entered at depth " + std::to_string(depth);
  TVectorImageP image =
      build(files.palette, nest(depth, g({s(10), g({s(11), s(12)})})));
  for (int i = 0; i < depth; i++)
    require(image->enterGroup(indexOf(image, 10)), "enter the group");
  const TVectorImageP glyph  = build(files.palette, {s(15)});
  const TVectorImageP glyphs = build(files.palette, {g({s(16), s(17)})});
  image->mergeImage(std::vector<const TVectorImage*>{glyph.getPointer(),
                                                     glyphs.getPointer()});

  const TVectorImageP wanted = build(
      files.palette,
      nest(depth, g({s(10), s(15), g({s(11), s(12)}), g({s(16), s(17)})})));
  requireStructure(image, wanted, what);
  require(image->isInsideGroup() == depth, what + " left the entered group");
  for (int name : {10, 11, 12, 15, 16, 17})
    require(image->isEnteredGroupStroke(indexOf(image, name)),
            what + " left stroke " + std::to_string(name) +
                " outside the entered group");
  requireStructure(saveAndLoad(files, image), image,
                   what + ", saved and loaded");
  std::cout << "groupstrokeorder-test: " << what << " PASS" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir directory;
    require(directory.isValid(), "create a temporary directory");
    const QString stuff = directory.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default"),
            "create isolated settings");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
    TEnv::setApplicationFullName("groupstrokeorder_test");
    TThread::init();
    initImageIo();
    Files files{directory.path()};
    unchangedRoundTrips(files);
    for (int depth = 1; depth <= 3; depth++)
      mergeIntoEnteredGroup(files, depth);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "groupstrokeorder-test: " << error.what() << '\n';
    return 1;
  }
}
