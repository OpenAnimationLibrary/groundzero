// Regression test of the Lottie export (toonz/lottieexporter.h) on a scene
// built in memory, as scenebench_test builds one: a vector level with two
// drawings (a filled square and two strokes each), a column with a keyframed
// x position and rotation, a sub-xsheet column and a raster column (with a
// stand-in renderer). The exported document is checked for its structure:
// the header, the layer counts and order, valid parent indices, the keys of
// the column transform, the precomposition and image assets, the shapes
// (region holes, strokes and outlines) and finite numbers only.

#include "toonz/lottieexporter.h"
#include "toonz/lottiemath.h"

#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/preferences.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshlevel.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/levelproperties.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectid.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/tcamera.h"
#include "tvectorimage.h"
#include "tstroke.h"
#include "tpalette.h"
#include "tdoubleparam.h"
#include "tfilepath.h"
#include "tenv.h"
#include "tconvert.h"
#include "texception.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QTemporaryDir>

#ifdef __linux__
#include <execinfo.h>
#endif

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

// Progress lines, so that a crash shows how far the test got
void step(const char *what) { std::cout << "ok: " << what << std::endl; }

#ifdef __linux__
void onCrash(int sig) {
  void *frames[64];
  const int count = backtrace(frames, 64);
  std::fprintf(stderr, "lottieexport_test: signal %d, backtrace:\n", sig);
  backtrace_symbols_fd(frames, count, 2);
  std::_Exit(128 + sig);
}
#endif

bool near(double a, double b, double eps = 1e-6) {
  return std::abs(a - b) <= eps;
}

//------------------------------------------------------------------
// Drawings

// A closed square of half side s, as a self loop stroke
TStroke *squareStroke(double s, double thick, int style) {
  std::vector<TThickPoint> points = {
      TThickPoint(-s, -s, thick), TThickPoint(0, -s, thick),
      TThickPoint(s, -s, thick),  TThickPoint(s, 0, thick),
      TThickPoint(s, s, thick),   TThickPoint(0, s, thick),
      TThickPoint(-s, s, thick),  TThickPoint(-s, 0, thick),
      TThickPoint(-s, -s, thick)};
  TStroke *stroke = new TStroke(points);
  stroke->setSelfLoop(true);
  stroke->setStyle(style);
  return stroke;
}

// An open arc above the square, of constant or varying thickness
TStroke *arcStroke(double s, bool varying, int style) {
  std::vector<TThickPoint> points = {TThickPoint(-s, 2 * s, 1.0),
                                     TThickPoint(0, 3 * s, varying ? 4.0 : 1.0),
                                     TThickPoint(s, 2 * s, 1.0)};
  TStroke *stroke                 = new TStroke(points);
  stroke->setStyle(style);
  return stroke;
}

TVectorImageP makeDrawing(TPalette *palette, int fillStyle, double s,
                          bool varying) {
  TVectorImageP vi = new TVectorImage();
  vi->setPalette(palette);
  vi->addStroke(squareStroke(s, 1.0, 1));
  vi->addStroke(arcStroke(s, varying, 1));
  vi->fill(TPointD(0, 0), fillStyle);
  return vi;
}

//------------------------------------------------------------------
// JSON helpers

QJsonArray groupsNamed(const QJsonArray &shapes, const QString &name) {
  QJsonArray groups;
  for (int i = 0; i < shapes.size(); ++i) {
    QJsonObject shape = shapes.at(i).toObject();
    if (shape.value("ty").toString() == "gr" &&
        shape.value("nm").toString() == name)
      groups.append(shape);
  }
  return groups;
}

QJsonArray itemsOfType(const QJsonObject &group, const QString &ty) {
  QJsonArray items = group.value("it").toArray(), result;
  for (int i = 0; i < items.size(); ++i)
    if (items.at(i).toObject().value("ty").toString() == ty)
      result.append(items.at(i));
  return result;
}

int vertexCount(const QJsonValue &sh) {
  QJsonObject k = sh.toObject().value("ks").toObject().value("k").toObject();
  int v         = k.value("v").toArray().size();
  if (k.value("i").toArray().size() != v || k.value("o").toArray().size() != v)
    return -1;
  return v;
}

std::vector<QJsonObject> layersOfType(const QJsonArray &layers, int ty) {
  std::vector<QJsonObject> result;
  for (int i = 0; i < layers.size(); ++i)
    if (layers.at(i).toObject().value("ty").toInt() == ty)
      result.push_back(layers.at(i).toObject());
  return result;
}

QJsonObject layerWithInd(const QJsonArray &layers, int ind) {
  for (int i = 0; i < layers.size(); ++i)
    if (layers.at(i).toObject().value("ind").toInt() == ind)
      return layers.at(i).toObject();
  return QJsonObject();
}

// Unique indices, parents among them, ip before op
void checkLayers(const QJsonArray &layers, const char *what) {
  std::set<int> indices;
  for (int i = 0; i < layers.size(); ++i) {
    QJsonObject layer = layers.at(i).toObject();
    require(layer.contains("ind") && layer.contains("ty") &&
                layer.contains("ks") && layer.contains("ip") &&
                layer.contains("op"),
            what);
    require(indices.insert(layer.value("ind").toInt()).second, what);
    require(layer.value("ip").toDouble() < layer.value("op").toDouble(), what);
  }
  for (int i = 0; i < layers.size(); ++i) {
    QJsonObject layer = layers.at(i).toObject();
    if (layer.contains("parent"))
      require(indices.count(layer.value("parent").toInt()) == 1, what);
  }
}

QJsonObject assetWithId(const QJsonArray &assets, const QString &id) {
  for (int i = 0; i < assets.size(); ++i)
    if (assets.at(i).toObject().value("id").toString() == id)
      return assets.at(i).toObject();
  return QJsonObject();
}

QJsonArray keysOf(const QJsonObject &ks, const char *property) {
  QJsonObject p = ks.value(property).toObject();
  return p.value("a").toInt() == 1 ? p.value("k").toArray() : QJsonArray();
}

double keyValue(const QJsonValue &key, int index) {
  return key.toObject().value("s").toArray().at(index).toDouble();
}

//------------------------------------------------------------------

// Holes: a filled ring between two squares, the inner one left empty, is
// one group with both outlines and an even-odd fill.
void testRegionHoles() {
  TPaletteP palette(new TPalette());
  TPalette::Page *page = palette->getPage(0);
  int red = page->getStyleId(page->addStyle(TPixel32(255, 0, 0, 255)));

  TVectorImageP vi = new TVectorImage();
  vi->setPalette(palette.getPointer());
  vi->addStroke(squareStroke(40, 1.0, 1));
  vi->addStroke(squareStroke(10, 1.0, 1));
  vi->fill(TPointD(25, 0), red);
  require(vi->getRegionCount() >= 1, "holes: regions computed");

  QStringList notes;
  QJsonArray shapes = LottieExport::vectorShapes(
      vi.getPointer(), palette.getPointer(), TAffine(), &notes);
  QJsonArray regions = groupsNamed(shapes, "Region");
  require(regions.size() == 1, "holes: one filled region");
  QJsonObject ring = regions.at(0).toObject();
  QJsonArray paths = itemsOfType(ring, "sh");
  require(paths.size() == 2, "holes: outer and inner outlines");
  require(vertexCount(paths.at(0)) >= 3 && vertexCount(paths.at(1)) >= 3,
          "holes: outline vertices");
  QJsonArray fills = itemsOfType(ring, "fl");
  require(fills.size() == 1 && fills.at(0).toObject().value("r").toInt() == 2,
          "holes: even-odd fill");
  require(groupsNamed(shapes, "Stroke").size() == 2, "holes: two strokes");
  // The region is drawn under the strokes: last, as shapes are top first
  require(shapes.at(shapes.size() - 1).toObject().value("nm").toString() ==
              "Region",
          "holes: region under the strokes");
  require(LottieMath::allFinite(shapes), "holes: finite numbers");
  require(notes.isEmpty(), "holes: solid styles only");
}

//------------------------------------------------------------------

struct SceneSetup {
  int m_red = 0;
  TPointD m_cameraDpi;
  QString m_columnName;
};

// Builds the scene. Rows 0-4:
//  column 0: drawings A1 A1 A2 A2 A1, x 0 -> 2 inches and angle 0 -> 90
//            degrees from frame 0 to frame 4
//  column 1: a sub-xsheet (A1 on its rows 0-2) at child frames 1 2 3 3 3
//  column 2: a raster level, frame 1 on rows 0-1
void buildScene(ToonzScene &scene, SceneSetup &setup) {
  TXsheet *xsh = scene.getXsheet();

  TPalette *palette    = new TPalette();
  TPalette::Page *page = palette->getPage(0);
  setup.m_red = page->getStyleId(page->addStyle(TPixel32(255, 0, 0, 255)));

  TXshLevel *xl = scene.createNewLevel(PLI_XSHLEVEL, L"A");
  require(xl && xl->getSimpleLevel(), "scene: vector level");
  TXshSimpleLevel *sl = xl->getSimpleLevel();
  sl->setPalette(palette);
  sl->setFrame(TFrameId(1), makeDrawing(palette, setup.m_red, 20, false));
  sl->setFrame(TFrameId(2), makeDrawing(palette, setup.m_red, 30, true));

  const int drawings[5] = {1, 1, 2, 2, 1};
  for (int r = 0; r < 5; ++r)
    xsh->setCell(r, 0, TXshCell(xl, TFrameId(drawings[r])));

  TStageObject *column = xsh->getStageObject(TStageObjectId::ColumnId(0));
  column->getParam(TStageObject::T_X)->setValue(0, 0.0);
  column->getParam(TStageObject::T_X)->setValue(4, 2.0);
  column->getParam(TStageObject::T_Angle)->setValue(0, 0.0);
  column->getParam(TStageObject::T_Angle)->setValue(4, 90.0);
  setup.m_columnName = QString::fromStdString(column->getName());

  // The sub-xsheet
  TXshLevel *childLevel = scene.createNewLevel(CHILD_XSHLEVEL, L"sub");
  require(childLevel && childLevel->getChildLevel(), "scene: child level");
  TXsheet *child = childLevel->getChildLevel()->getXsheet();
  for (int r = 0; r < 3; ++r) child->setCell(r, 0, TXshCell(xl, TFrameId(1)));
  child->updateFrameCount();
  const int childFrames[5] = {1, 2, 3, 3, 3};
  for (int r = 0; r < 5; ++r)
    xsh->setCell(r, 1, TXshCell(childLevel, TFrameId(childFrames[r])));

  // A raster level at the camera dpi; the test renderer gives its image
  setup.m_cameraDpi = xsh->getStageObjectTree()->getCurrentCamera()->getDpi();
  TXshSimpleLevel *raster = new TXshSimpleLevel(L"R");
  raster->setScene(&scene);
  raster->setType(OVL_XSHLEVEL);
  raster->getProperties()->setDpiPolicy(LevelProperties::DP_CustomDpi);
  raster->getProperties()->setDpi(setup.m_cameraDpi);
  for (int r = 0; r < 2; ++r) xsh->setCell(r, 2, TXshCell(raster, TFrameId(1)));

  xsh->updateFrameCount();
}

// A stand-in for the PNG renderer of the application: 40 x 20 pixels
bool renderRaster(TXshSimpleLevel *, const TFrameId &,
                  LottieExport::RasterImage &image) {
  image.m_png    = QByteArray("not really a png");
  image.m_width  = 40;
  image.m_height = 20;
  image.m_offset = TPointD();
  return true;
}

//------------------------------------------------------------------

void testScene() {
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  SceneSetup setup;
  buildScene(scene, setup);
  step("scene built");

  TXsheet *xsh = scene.getXsheet();
  LottieExport::Options options;
  options.m_frameRate         = 24;
  options.m_name              = "test";
  options.m_renderRaster      = renderRaster;
  LottieExport::Result result = LottieExport::exportXsheet(xsh, options);
  for (const QString &skipped : result.m_skipped)
    std::cout << "  skipped: " << skipped.toStdString() << std::endl;
  step("scene exported");

  // Through text and back, as a player reads it: no NaN or infinity tokens
  QByteArray text = QJsonDocument(result.m_animation).toJson();
  QJsonParseError error;
  QJsonDocument doc = QJsonDocument::fromJson(text, &error);
  require(error.error == QJsonParseError::NoError && doc.isObject(),
          "json: parses");
  QJsonObject root = doc.object();
  require(LottieMath::allFinite(root), "json: finite numbers");

  // Header
  require(root.value("v").toString().startsWith("5."), "header: version");
  require(near(root.value("fr").toDouble(), 24), "header: frame rate");
  require(root.value("ip").toInt() == 0, "header: in point");
  require(root.value("op").toInt() == 5, "header: out point");
  require(root.value("w").toInt() > 0 && root.value("h").toInt() > 0,
          "header: size");
  double width  = root.value("w").toDouble(),
         height = root.value("h").toDouble();

  // Layers: top first, the raster column (2), the sub-xsheet (1), then the
  // three runs of column 0, then the null layers
  QJsonArray layers = root.value("layers").toArray();
  checkLayers(layers, "layers: indices and parents");
  require(layersOfType(layers, 4).size() == 3, "layers: 3 drawing runs");
  require(layersOfType(layers, 0).size() == 1, "layers: 1 precomposition");
  require(layersOfType(layers, 2).size() == 1, "layers: 1 image");
  require(layersOfType(layers, 3).size() >= 3, "layers: column null layers");
  require(layers.size() == 5 + (int)layersOfType(layers, 3).size(),
          "layers: no other layer");
  require(result.m_contentLayers == 5, "layers: content count");
  require(layers.at(0).toObject().value("ty").toInt() == 2 &&
              layers.at(1).toObject().value("ty").toInt() == 0 &&
              layers.at(2).toObject().value("ty").toInt() == 4,
          "layers: stacking order");

  // The drawing runs: frames [0, 2), [2, 4), [4, 5), in one column layer
  std::vector<QJsonObject> runs = layersOfType(layers, 4);
  std::set<std::pair<int, int>> ranges;
  for (const QJsonObject &run : runs)
    ranges.insert(
        std::make_pair(run.value("ip").toInt(), run.value("op").toInt()));
  require(ranges.count(std::make_pair(0, 2)) &&
              ranges.count(std::make_pair(2, 4)) &&
              ranges.count(std::make_pair(4, 5)),
          "runs: visibility ranges");
  int columnInd = runs[0].value("parent").toInt();
  for (const QJsonObject &run : runs)
    require(run.value("parent").toInt() == columnInd, "runs: same parent");

  // The column layer: x and rotation keys from frame 0 to frame 4
  QJsonObject columnLayer = layerWithInd(layers, columnInd);
  require(columnLayer.value("ty").toInt() == 3, "column: null layer");
  require(columnLayer.value("nm").toString() == setup.m_columnName,
          "column: named after the column");
  QJsonObject ks = columnLayer.value("ks").toObject();
  QJsonArray p   = keysOf(ks, "p");
  QJsonArray r   = keysOf(ks, "r");
  require(p.size() >= 2 && r.size() >= 2, "column: position and angle keys");
  require(p.size() <= 5 && r.size() <= 5, "column: one key per frame at most");
  QJsonValue p0 = p.at(0), p4 = p.at(p.size() - 1);
  require(near(p0.toObject().value("t").toDouble(), 0) &&
              near(p4.toObject().value("t").toDouble(), 4),
          "column: key times");
  // At the camera center, then 2 inches to the right; the y axis is flipped
  require(near(keyValue(p0, 0), 0.5 * width, 1e-2) &&
              near(keyValue(p0, 1), 0.5 * height, 1e-2),
          "column: starts at the camera center");
  require(
      near(keyValue(p4, 0) - keyValue(p0, 0), 2 * setup.m_cameraDpi.x, 1e-2) &&
          near(keyValue(p4, 1), keyValue(p0, 1), 1e-2),
      "column: moves 2 inches right");
  // 90 degrees counterclockwise on the stage is -90 in Lottie
  require(near(keyValue(r.at(0), 0), 0, 1e-3) &&
              near(keyValue(r.at(r.size() - 1), 0), -90, 1e-3),
          "column: rotation keys");

  // Shapes of the drawings
  for (const QJsonObject &run : runs) {
    QJsonArray shapes  = run.value("shapes").toArray();
    QJsonArray regions = groupsNamed(shapes, "Region");
    require(regions.size() == 1, "shapes: one filled region");
    QJsonObject region = regions.at(0).toObject();
    QJsonArray paths   = itemsOfType(region, "sh");
    require(paths.size() == 1 && vertexCount(paths.at(0)) >= 3,
            "shapes: region outline");
    QJsonArray fills = itemsOfType(region, "fl");
    require(fills.size() == 1 && fills.at(0).toObject().value("r").toInt() == 2,
            "shapes: region fill");
    QJsonArray strokes = groupsNamed(shapes, "Stroke");
    require(strokes.size() == 2, "shapes: two strokes");
    int lines = 0, outlines = 0;
    for (int i = 0; i < strokes.size(); ++i) {
      QJsonObject stroke = strokes.at(i).toObject();
      QJsonArray st      = itemsOfType(stroke, "st");
      if (!st.isEmpty()) {
        require(
            st.at(0).toObject().value("w").toObject().value("k").toDouble() > 0,
            "shapes: stroke width");
        ++lines;
      }
      QJsonArray fl = itemsOfType(stroke, "fl");
      if (!fl.isEmpty()) {
        require(fl.at(0).toObject().value("r").toInt() == 1,
                "shapes: outline fill rule");
        ++outlines;
      }
      require(vertexCount(itemsOfType(stroke, "sh").at(0)) >= 2,
              "shapes: stroke path");
    }
    // A1 (frames 0 and 4): constant thickness, two Lottie strokes; A2: the
    // varying arc is an outline
    bool a2 = run.value("ip").toInt() == 2;
    require(a2 ? (lines == 1 && outlines == 1) : (lines == 2 && outlines == 0),
            "shapes: strokes or outlines");
  }

  // The precomposition
  QJsonArray assets   = root.value("assets").toArray();
  QJsonObject precomp = layersOfType(layers, 0)[0];
  QJsonObject comp    = assetWithId(assets, precomp.value("refId").toString());
  require(!comp.isEmpty() && comp.contains("layers"),
          "precomp: refId is an asset");
  require(precomp.value("w").toInt() > 0 && precomp.value("h").toInt() > 0,
          "precomp: size");
  require(near(precomp.value("ip").toDouble(), 0) &&
              near(precomp.value("op").toDouble(), 5),
          "precomp: range");
  QJsonArray childLayers = comp.value("layers").toArray();
  checkLayers(childLayers, "precomp: indices and parents");
  std::vector<QJsonObject> childRuns = layersOfType(childLayers, 4);
  require(childRuns.size() == 1 &&
              near(childRuns[0].value("ip").toDouble(), 0) &&
              near(childRuns[0].value("op").toDouble(), 3),
          "precomp: child drawing run");
  // Child frames 1 2 3 3 3: hold keys at frames 0, 1 and 2, in seconds
  QJsonObject tm = precomp.value("tm").toObject();
  require(tm.value("a").toInt() == 1, "precomp: time remap animated");
  QJsonArray tmKeys = tm.value("k").toArray();
  require(tmKeys.size() == 3, "precomp: time remap keys");
  for (int i = 0; i < 3; ++i) {
    QJsonObject key = tmKeys.at(i).toObject();
    require(near(key.value("t").toDouble(), i) && key.value("h").toInt() == 1,
            "precomp: hold keys");
    require(near(keyValue(key, 0), i / 24.0, 1e-5), "precomp: child times");
  }

  // The image: an embedded asset, centered on the column
  QJsonObject imageLayer = layersOfType(layers, 2)[0];
  QJsonObject image = assetWithId(assets, imageLayer.value("refId").toString());
  require(!image.isEmpty(), "image: refId is an asset");
  require(image.value("w").toInt() == 40 && image.value("h").toInt() == 20,
          "image: size");
  require(image.value("e").toInt() == 1 &&
              image.value("p").toString().startsWith("data:image/png;base64,"),
          "image: embedded");
  QJsonObject imageKs = imageLayer.value("ks").toObject();
  QJsonArray imageP   = imageKs.value("p").toObject().value("k").toArray();
  require(imageP.size() >= 2 && near(imageP.at(0).toDouble(), -20, 1e-2) &&
              near(imageP.at(1).toDouble(), -10, 1e-2),
          "image: centered on the column origin");
  QJsonArray imageS = imageKs.value("s").toObject().value("k").toArray();
  require(imageS.size() >= 2 && near(imageS.at(0).toDouble(), 100, 1e-2) &&
              near(imageS.at(1).toDouble(), 100, 1e-2),
          "image: at the camera resolution");
  require(near(imageLayer.value("ip").toDouble(), 0) &&
              near(imageLayer.value("op").toDouble(), 2),
          "image: range");
  step("scene checked");

  // Images as files
  options.m_embedImages  = false;
  options.m_imagesFolder = "test_images/";
  result                 = LottieExport::exportXsheet(xsh, options);
  assets                 = result.m_animation.value("assets").toArray();
  require(result.m_imageFiles.size() == 1, "files: one image file");
  bool found = false;
  for (int i = 0; i < assets.size(); ++i) {
    QJsonObject asset = assets.at(i).toObject();
    if (!asset.contains("w") || asset.contains("layers")) continue;
    require(
        asset.value("e").toInt() == 0 &&
            asset.value("u").toString() == "test_images/" &&
            asset.value("p").toString() == result.m_imageFiles[0].m_fileName,
        "files: asset path");
    found = true;
  }
  require(found, "files: image asset");

  // A range of the rows
  options.m_r0 = 2;
  options.m_r1 = 3;
  result       = LottieExport::exportXsheet(xsh, options);
  require(result.m_animation.value("op").toInt() == 2, "range: frames");
  require(
      layersOfType(result.m_animation.value("layers").toArray(), 4).size() == 1,
      "range: one drawing run");
  step("options checked");

  scene.clear();
}

}  // namespace

int main(int argc, char *argv[]) {
#ifdef __linux__
  std::signal(SIGSEGV, onCrash);
  std::signal(SIGABRT, onCrash);
#endif
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir dir;
    require(dir.isValid(), "no temporary directory");
    // An empty stuff folder, so that the defaults are used
    const QString stuffPath = QDir(dir.path()).filePath("stuff");
    QDir().mkpath(QDir(stuffPath).filePath("profiles/layouts/rooms/Default"));
    TEnv::setStuffDir(TFilePath(stuffPath.toStdWString()));
    Preferences::instance();
    step("environment set up");

    testRegionHoles();
    step("region holes");
    testScene();
  } catch (const TException &e) {
    std::cerr << "lottieexport_test failed: " << ::to_string(e.getMessage())
              << std::endl;
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "lottieexport_test failed: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "lottieexport_test passed" << std::endl;
  return 0;
}
