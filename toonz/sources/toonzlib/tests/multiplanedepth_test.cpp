// Regression test of the Multiplane Depth (doc/multiplane_depth.md): the pure
// math (toonz/multiplanedepth.h), the scene setting, saved only when used,
// and the blur and haze the render inserts per column from its Z.

#include "toonz/multiplanedepth.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcolumnfx.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/scenefx.h"
#include "toonz/tstageobject.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshleveltypes.h"
#include "tconvert.h"
#include "tdoubleparam.h"
#include "tenv.h"
#include "tfx.h"
#include "tparamcontainer.h"
#include "tstream.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

DV_IMPORT_API void initStdFx();

namespace {

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
bool approx(double a, double b, double tolerance = 1e-9) {
  return std::abs(a - b) <= tolerance;
}

TFilePath toPath(const QString &path) { return TFilePath(path.toStdWString()); }

//------------------------------------------------------------------

void testMath() {
  using namespace MultiplaneDepth;
  Settings s;
  require(!s.enabled, "math: the depth is on by default");
  Column off = evaluate(s, -500, 0, 0);
  require(!off.hasBlur() && !off.hasColor(), "math: off does something");

  s.enabled             = true;
  s.blurPerUnit         = 0.02;
  s.maxBlur             = 10;
  s.hazeDensity         = 0.004;
  s.desaturationPerUnit = 0.001;
  // In focus: nothing
  Column inFocus = evaluate(s, 120, 120, 0);
  require(!inFocus.hasBlur() && !inFocus.hasColor(),
          "math: a column in focus changes");
  // The blur grows with the distance on both sides, up to the maximum
  require(approx(evaluate(s, 220, 120, 0).blur, 2) &&
              approx(evaluate(s, 20, 120, 0).blur, 2),
          "math: the blur is not linear in the distance");
  require(approx(evaluate(s, -5000, 120, 0).blur, 10),
          "math: the blur exceeds its maximum");
  // Each stop of aperture doubles it, the maximum included
  require(approx(evaluate(s, 220, 120, 1).blur, 4) &&
              approx(evaluate(s, 220, 120, -2).blur, 0.5) &&
              approx(evaluate(s, -5000, 120, 1).blur, 20),
          "math: the aperture does not count in stops");
  // Haze and desaturation only behind the focus
  Column front = evaluate(s, 300, 120, 0), back = evaluate(s, -80, 120, 0);
  require(front.haze == 0 && front.desaturation == 0,
          "math: a column in front of the focus is hazed");
  require(approx(back.haze, 1 - std::exp(-0.004 * 200)) &&
              approx(back.desaturation, 0.2),
          "math: the haze or desaturation behind the focus is wrong");
  require(evaluate(s, -100000, 120, 0).desaturation == 1,
          "math: the desaturation exceeds 1");
  // Invalid values give no effect
  require(!evaluate(s, NAN, 120, 0).hasBlur() &&
              !evaluate(s, 0, INFINITY, 0).hasBlur(),
          "math: invalid positions are not ignored");
  Settings bad    = s;
  bad.blurPerUnit = -3, bad.hazeColor[1] = 7, bad.maxBlur = NAN;
  bad.clamp();
  require(bad.blurPerUnit == 0 && bad.hazeColor[1] == 1 && bad.maxBlur == 0,
          "math: clamp does not make the settings valid");

  require(approx(localBlur(6, 0.5), 12) && localBlur(6, 0) == 0 &&
              localBlur(6, NAN) == 0,
          "math: the local blur is wrong");

  // Colours: alpha unchanged, a transparent pixel stays transparent, full
  // haze is the haze colour times the alpha, full desaturation is grey
  const double haze[3] = {0.2, 0.4, 0.8};
  Column full;
  full.haze     = 1;
  double rgb[3] = {0.5, 0.1, 0.3};
  applyPremultiplied(full, haze, rgb, 0.5);
  require(approx(rgb[0], 0.1) && approx(rgb[1], 0.2) && approx(rgb[2], 0.4),
          "math: full haze is not the haze colour");
  double clear[3] = {0, 0, 0};
  applyPremultiplied(full, haze, clear, 0);
  require(clear[0] == 0 && clear[1] == 0 && clear[2] == 0,
          "math: haze shows on a transparent pixel");
  Column grey;
  grey.desaturation = 1;
  double c[3]       = {0.9, 0.2, 0.1};
  applyPremultiplied(grey, haze, c, 1);
  require(approx(c[0], c[1]) && approx(c[1], c[2]) &&
              approx(c[0], 0.299 * 0.9 + 0.587 * 0.2 + 0.114 * 0.1),
          "math: full desaturation is not the luma");
}

//------------------------------------------------------------------

QString saveProperties(const TSceneProperties &props, const QString &path) {
  {
    TOStream os(toPath(path));
    require(os.checkStatus(), "save: cannot open the file");
    os.openChild("properties");
    props.saveData(os);
    os.closeChild();
  }
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "save: cannot read the file back");
  return QString::fromUtf8(file.readAll());
}

void testProperties(const QString &dir) {
  TSceneProperties props;
  const QString unused = QDir(dir).filePath("unused.xml");
  require(!saveProperties(props, unused).contains("multiplaneDepth"),
          "properties: the default depth is saved");

  MultiplaneDepth::Settings s;
  s.enabled             = true;
  s.blurPerUnit         = 0.125;
  s.maxBlur             = 17;
  s.hazeDensity         = 0.0025;
  s.desaturationPerUnit = 0.0005;
  s.hazeColor[0] = 0.25, s.hazeColor[1] = 0.5, s.hazeColor[2] = 0.75;
  props.setMultiplaneDepth(s);
  const QString used = QDir(dir).filePath("used.xml");
  require(saveProperties(props, used).contains("multiplaneDepth"),
          "properties: the depth is not saved");

  TSceneProperties loaded;
  TIStream is(toPath(used));
  std::string tag;
  require(is.matchTag(tag) && tag == "properties", "properties: no root tag");
  loaded.loadData(is, false);
  require(loaded.getMultiplaneDepth() == s,
          "properties: the depth did not load back");

  // A copy of the properties carries it
  TSceneProperties copy;
  copy.assign(&loaded);
  require(copy.getMultiplaneDepth() == s, "properties: assign lost the depth");
}

//------------------------------------------------------------------

// What the render puts over a column: the blur value (0 for none) and whether
// it is hazed
struct Inserted {
  double blur = 0;
  bool hazed  = false;
};

// Walks the render tree; an fx above a column's fx records itself for it
int columnBelow(TFx *fx) {
  if (!fx) return -1;
  if (TLevelColumnFx *column = dynamic_cast<TLevelColumnFx *>(fx))
    return column->getColumn() ? column->getColumn()->getIndex() : -1;
  for (int i = 0; i < fx->getInputPortCount(); ++i) {
    int c = columnBelow(fx->getInputPort(i)->getFx());
    if (c >= 0) return c;
  }
  return -1;
}

void collect(TFx *fx, std::map<int, Inserted> &found) {
  if (!fx) return;
  if (fx->getFxType() == "STD_blurFx") {
    TDoubleParamP value         = TParamP(fx->getParams()->getParam("value"));
    found[columnBelow(fx)].blur = value->getDefaultValue();
  } else if (fx->getFxType() == "columnDepthFx")
    found[columnBelow(fx)].hazed = true;
  for (int i = 0; i < fx->getInputPortCount(); ++i)
    collect(fx->getInputPort(i)->getFx(), found);
}

std::map<int, Inserted> render(ToonzScene &scene) {
  TFxP root = buildSceneFx(&scene, 0, 1, false);
  require(root.getPointer() != nullptr, "render: no render tree");
  std::map<int, Inserted> found;
  collect(root.getPointer(), found);
  return found;
}

void testRender() {
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  TXsheet *xsh     = scene.getXsheet();
  TXshLevel *level = scene.createNewLevel(PLI_XSHLEVEL, L"A");
  require(level != nullptr, "render: cannot create a level");
  // Three columns: in focus (Z 0), behind (Z -300) and in front (Z 200)
  const double zs[3] = {0, -300, 200};
  for (int c = 0; c < 3; ++c) {
    xsh->setCell(0, c, TXshCell(level, TFrameId(1)));
    xsh->getStageObject(TStageObjectId::ColumnId(c))
        ->getParam(TStageObject::T_Z)
        ->setDefaultValue(zs[c]);
  }

  // Off: nothing inserted
  require(render(scene).empty(), "render: the depth is off but applied");

  MultiplaneDepth::Settings s;
  s.enabled     = true;
  s.blurPerUnit = 0.02;
  s.hazeDensity = 0.002;
  scene.getProperties()->setMultiplaneDepth(s);
  std::map<int, Inserted> on = render(scene);
  require(!on.count(0), "render: the column in focus is changed");
  // The blur is divided by the column's scale on screen: 1000 / (1000 - z)
  require(on.count(1) && approx(on[1].blur, 0.02 * 300 * 1300 / 1000.0, 1e-6),
          "render: the column behind has the wrong blur");
  require(on[1].hazed, "render: the column behind is not hazed");
  require(on.count(2) && approx(on[2].blur, 0.02 * 200 * 800 / 1000.0, 1e-6),
          "render: the column in front has the wrong blur");
  require(!on[2].hazed, "render: the column in front is hazed");

  // Rack focus: the camera's focus on the back column makes it sharp
  TStageObject *camera = xsh->getStageObject(TStageObjectId::CameraId(0));
  camera->getParam(TStageObject::T_FocusZ)->setDefaultValue(-300);
  std::map<int, Inserted> rack = render(scene);
  require(!rack.count(1), "render: the column in focus is still blurred");
  require(rack.count(0) && rack[0].blur > 0 && rack[0].hazed == false,
          "render: the column at the table is not blurred by the rack focus");
  // One stop more doubles the blur
  camera->getParam(TStageObject::T_Aperture)->setDefaultValue(1);
  std::map<int, Inserted> wide = render(scene);
  require(approx(wide[0].blur, rack[0].blur * 2, 1e-9),
          "render: the aperture does not double the blur");
  camera->getParam(TStageObject::T_FocusZ)->setDefaultValue(0);
  camera->getParam(TStageObject::T_Aperture)->setDefaultValue(0);

  // An excluded column is left alone
  xsh->getColumn(1)->setDepthExcluded(true);
  std::map<int, Inserted> excluded = render(scene);
  require(!excluded.count(1) && excluded.count(2),
          "render: the excluded column is changed");
  require(xsh->getColumn(1)->isDepthExcluded() &&
              !xsh->getColumn(1)->isOutlineView(),
          "render: the exclusion bit changed another flag");
}

}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir dir;
    require(dir.isValid(), "no temporary directory");
    // The scenes need a stuff folder: an empty one gives the defaults
    QString stuffPath = QDir(dir.path()).filePath("stuff");
    QDir().mkpath(QDir(stuffPath).filePath("profiles/layouts/rooms/Default"));
    TEnv::setStuffDir(TFilePath(stuffPath));
    initStdFx();

    testMath();
    testProperties(dir.path());
    testRender();
  } catch (const TException &e) {
    std::cerr << "multiplanedepth_test: " << ::to_string(e.getMessage())
              << std::endl;
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "multiplanedepth_test: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "ok: multiplane depth" << std::endl;
  return 0;
}
