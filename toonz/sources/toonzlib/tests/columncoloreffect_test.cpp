// Regression test of the per-column Color Effect (doc/animate_color_effect.md):
// the pure math of toonz/columncoloreffect.h (Brightness, Tint, Alpha,
// Advanced, their order, the clamping and the premultiplied pixels), and the
// parameters of the level column fx, which are written to the scene file only
// when used, read back, and skipped when unknown.

#include "toonz/columncoloreffect.h"

#include "toonz/txshlevelcolumn.h"
#include "tdoubleparam.h"
#include "tfx.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "tstream.h"
#include "tfilepath.h"
#include "texception.h"
#include "tconvert.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace CE = ColumnColorEffect;

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

bool isNear(double a, double b) { return std::abs(a - b) < 1e-6; }

// The effect of the values on a straight color
void applied(const CE::Values &v, double r, double g, double b, double a,
             double out[4]) {
  out[0] = r, out[1] = g, out[2] = b, out[3] = a;
  CE::compose(v).applyStraight(out);
}

bool isColor(const double c[4], double r, double g, double b, double a) {
  return isNear(c[0], r) && isNear(c[1], g) && isNear(c[2], b) &&
         isNear(c[3], a);
}

//------------------------------------------------------------------

void testParams() {
  for (int p = 0; p < CE::ParamCount; ++p) {
    require(CE::paramIndex(CE::paramName(p)) == p, "params: name round trip");
    double minValue = 0, maxValue = 0;
    CE::valueRange(p, minValue, maxValue);
    require(minValue <= CE::defaultValue(p) && CE::defaultValue(p) <= maxValue,
            "params: default outside the range");
  }
  require(CE::paramIndex("timeRemap") == -1, "params: unknown name");
  require(CE::paramIndex(CE::tintColorParamName()) == -1,
          "params: tint is apart");
}

void testIdentity() {
  CE::Values v;
  require(CE::compose(v).isIdentity(), "identity: defaults");
  require(CE::Transform().isIdentity(), "identity: default transform");

  // The tint color alone does nothing
  v.setTint(255, 0, 0);
  require(CE::compose(v).isIdentity(), "identity: tint color without amount");

  v.set(CE::Brightness, 1);
  require(!CE::compose(v).isIdentity(), "identity: brightness 1");
}

void testBrightness() {
  double c[4];
  CE::Values v;
  v.set(CE::Brightness, 100);
  applied(v, 10, 100, 200, 255, c);
  require(isColor(c, 255, 255, 255, 255), "brightness +100: white");

  v.set(CE::Brightness, -100);
  applied(v, 10, 100, 200, 128, c);
  require(isColor(c, 0, 0, 0, 128), "brightness -100: black, alpha kept");

  v.set(CE::Brightness, 50);
  applied(v, 100, 0, 255, 255, c);
  require(isColor(c, 177.5, 127.5, 255, 255), "brightness +50");

  v.set(CE::Brightness, -50);
  applied(v, 100, 0, 255, 255, c);
  require(isColor(c, 50, 0, 127.5, 255), "brightness -50");
}

void testTint() {
  double c[4];
  CE::Values v;
  v.setTint(30, 60, 90);
  v.set(CE::TintAmount, 100);
  applied(v, 200, 10, 250, 200, c);
  require(isColor(c, 30, 60, 90, 200), "tint 100%: the tint color");

  v.setTint(0, 100, 200);
  v.set(CE::TintAmount, 50);
  applied(v, 200, 0, 100, 128, c);
  require(isColor(c, 100, 50, 150, 128), "tint 50%: half way");
}

void testAlpha() {
  double c[4];
  CE::Values v;
  v.set(CE::Alpha, 50);
  applied(v, 10, 20, 30, 200, c);
  require(isColor(c, 10, 20, 30, 100), "alpha 50%");

  v.set(CE::Alpha, 0);
  v.set(CE::AlphaAdd, 255);
  applied(v, 10, 20, 30, 255, c);
  require(isNear(c[3], 0), "alpha 0: transparent, whatever the offset");
}

void testAdvanced() {
  CE::Values v;
  const double mul[4] = {50, 25, -100, 80}, add[4] = {10, -20, 255, 5};
  for (int i = 0; i < 4; ++i) {
    v.set(CE::RedMul + i, mul[i]);
    v.set(CE::RedAdd + i, add[i]);
  }
  const CE::Transform t = CE::compose(v);
  for (int i = 0; i < 4; ++i) {
    require(isNear(t.m[i], mul[i] / 100.0), "advanced: multiplier");
    require(isNear(t.c[i], add[i]), "advanced: offset");
  }

  double c[4];
  applied(v, 100, 100, 100, 100, c);
  require(isColor(c, 60, 5, 155, 85), "advanced: applied");
}

void testOrder() {
  double c[4];

  // Tint, then Brightness: the tinted color is brightened
  CE::Values v;
  v.setTint(0, 0, 0);
  v.set(CE::TintAmount, 100);
  v.set(CE::Brightness, 50);
  applied(v, 200, 10, 90, 255, c);
  require(isColor(c, 127.5, 127.5, 127.5, 255), "order: tint, brightness");

  // Advanced, then Tint: a full tint covers the advanced offset
  CE::Values w;
  w.set(CE::RedAdd, 100);
  w.setTint(40, 50, 60);
  w.set(CE::TintAmount, 100);
  applied(w, 10, 20, 30, 255, c);
  require(isColor(c, 40, 50, 60, 255), "order: advanced, tint");

  // Advanced, then Alpha: the alpha offset is scaled by the alpha
  CE::Values x;
  x.set(CE::AlphaAdd, 100);
  x.set(CE::Alpha, 50);
  applied(x, 0, 0, 0, 100, c);
  require(isNear(c[3], 100), "order: advanced, alpha");

  // then() is the sequence of the two, without clamping in between
  CE::Transform a, b;
  a.m[0] = 0.5, a.c[0] = 20, a.m[3] = 0.8, a.c[3] = -10;
  b.m[0] = 1.5, b.c[0] = -30, b.m[3] = 0.5, b.c[3] = 40;
  double seq[4] = {100, 50, 25, 200}, once[4] = {100, 50, 25, 200};
  a.applyStraight(seq);
  b.applyStraight(seq);
  a.then(b).applyStraight(once);
  for (int i = 0; i < 4; ++i) require(isNear(seq[i], once[i]), "order: then()");
}

void testClamping() {
  double c[4];
  CE::Values v, w;
  v.set(CE::Brightness, 300);
  w.set(CE::Brightness, 100);
  applied(v, 10, 20, 30, 40, c);
  double d[4];
  applied(w, 10, 20, 30, 40, d);
  require(isColor(c, d[0], d[1], d[2], d[3]), "clamp: brightness 300");

  CE::Values none;
  none.set(CE::TintAmount, -20);
  none.set(CE::Alpha, 150);
  none.set(CE::RedMul, std::numeric_limits<double>::quiet_NaN());
  require(CE::compose(none).isIdentity(), "clamp: out of range values");

  CE::Values big;
  big.set(CE::RedMul, 500);
  big.set(CE::GreenAdd, 1000);
  const CE::Transform t = CE::compose(big);
  require(isNear(t.m[0], 1.0) && isNear(t.c[1], 255), "clamp: advanced ranges");

  CE::Values over;
  over.set(CE::RedAdd, 255);
  over.set(CE::BlueAdd, -255);
  applied(over, 200, 0, 100, 255, c);
  require(isColor(c, 255, 0, 0, 255), "clamp: output in 0..255");
}

void testPremultiplied() {
  // Half transparent, premultiplied dark red, brightened to white
  CE::Values v;
  v.set(CE::Brightness, 100);
  double c[4] = {64, 0, 0, 128};
  CE::compose(v).applyPremultiplied(c, 255);
  require(isColor(c, 128, 128, 128, 128), "premultiplied: brightness");

  // Fully transparent stays transparent
  CE::Values w;
  w.set(CE::AlphaAdd, 255);
  w.set(CE::RedAdd, 255);
  double t[4] = {0, 0, 0, 0};
  CE::compose(w).applyPremultiplied(t, 255);
  require(isColor(t, 0, 0, 0, 0), "premultiplied: transparent kept");

  // 16 bit channels, tint and alpha
  CE::Values x;
  x.setTint(255, 0, 0);
  x.set(CE::TintAmount, 100);
  x.set(CE::Alpha, 50);
  double p[4] = {32768, 0, 0, 65535};
  CE::compose(x).applyPremultiplied(p, 65535);
  require(isColor(p, 32767.5, 0, 0, 32767.5), "premultiplied: 16 bit");

  // The color never exceeds the alpha
  CE::Values y;
  y.set(CE::AlphaMul, 50);
  y.set(CE::Brightness, 100);
  double q[4] = {100, 100, 100, 100};
  CE::compose(y).applyPremultiplied(q, 255);
  require(q[0] <= q[3] + 1e-9 && isNear(q[3], 50), "premultiplied: matte");
}

//------------------------------------------------------------------
// The parameters of the level column fx

TFilePath toPath(const QString &path) { return TFilePath(path.toStdWString()); }

QByteArray contents(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "file: cannot read");
  return file.readAll();
}

void writeBytes(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
          "file: cannot write");
  file.write(bytes);
}

const TParamVar *paramVar(TFx *fx, const std::string &name) {
  TParamContainer *params = fx->getParams();
  for (int i = 0; i < params->getParamCount(); ++i)
    if (params->getParamName(i) == name) return params->getParamVar(i);
  return nullptr;
}

TDoubleParam *doubleParam(TFx *fx, const std::string &name) {
  return dynamic_cast<TDoubleParam *>(fx->getParams()->getParam(name));
}

TPixelParam *pixelParam(TFx *fx, const std::string &name) {
  return dynamic_cast<TPixelParam *>(fx->getParams()->getParam(name));
}

bool isSaved(TFx *fx, const std::string &name) {
  const TParamVar *var = paramVar(fx, name);
  require(var != nullptr, "fx: a parameter is missing");
  return fx->isParamSaved(var);
}

void saveColumn(TXshLevelColumn &column, const QString &path) {
  TOStream os(toPath(path));
  require(os.checkStatus(), "save: cannot open the file");
  os.openChild("column");
  column.saveData(os);
  os.closeChild();
}

void loadColumn(TXshLevelColumn &column, const QString &path) {
  TIStream is(toPath(path));
  require(bool(is), "load: cannot open the file");
  std::string tag;
  require(is.matchTag(tag) && tag == "column", "load: no column tag");
  column.loadData(is);
  require(is.matchEndTag(), "load: the column tag is not closed");
}

bool hasAnyEffectTag(const QByteArray &text) {
  for (int p = 0; p < CE::ParamCount; ++p)
    if (text.contains(QByteArray("<") + CE::paramName(p) + ">")) return true;
  return text.contains(QByteArray("<") + CE::tintColorParamName() + ">");
}

void testColumnFx(const QString &dir) {
  // Unused: nothing is saved
  // (columns are reference counted: they live in smart pointers)
  TXshLevelColumnP column(new TXshLevelColumn());
  TFx *fx = column->getFx();
  require(fx != nullptr, "fx: no column fx");
  for (int p = 0; p < CE::ParamCount; ++p)
    require(!isSaved(fx, CE::paramName(p)), "fx: a default parameter is saved");
  require(!isSaved(fx, CE::tintColorParamName()),
          "fx: the default tint is saved");
  require(!column->isColorEffectUsed(), "fx: used by default");

  const QString plainPath = QDir(dir).filePath("plain.xml");
  saveColumn(*column, plainPath);
  require(!hasAnyEffectTag(contents(plainPath)), "file: tags when not used");

  // Used: only the changed parameters are saved
  doubleParam(fx, "brightness")->setDefaultValue(-40);
  require(isSaved(fx, "brightness"), "fx: a changed parameter is not saved");
  require(!isSaved(fx, "alpha"), "fx: an unchanged parameter is saved");
  require(column->isColorEffectUsed(), "fx: not used when changed");

  // A key counts as a use, even at the default value
  doubleParam(fx, "alpha")->setValue(10, 100.0);
  require(isSaved(fx, "alpha"), "fx: a keyed parameter is not saved");

  pixelParam(fx, CE::tintColorParamName())
      ->setDefaultValue(TPixel32(255, 0, 0));
  require(isSaved(fx, CE::tintColorParamName()),
          "fx: a changed tint not saved");

  // The time remap is still unused
  require(!isSaved(fx, "timeRemap"), "fx: the time remap is saved");

  const QString usedPath = QDir(dir).filePath("used.xml");
  saveColumn(*column, usedPath);
  const QByteArray used = contents(usedPath);
  require(used.contains("<brightness>"), "file: no brightness");
  require(used.contains("<alpha>"), "file: no alpha");
  require(used.contains("<tintColor>"), "file: no tint color");
  require(!used.contains("<redMul>"), "file: an unchanged parameter");

  // Read back
  TXshLevelColumnP loaded(new TXshLevelColumn());
  loadColumn(*loaded, usedPath);
  TFx *loadedFx = loaded->getFx();
  require(loadedFx != nullptr && loadedFx != fx, "load: no column fx");
  require(isNear(doubleParam(loadedFx, "brightness")->getDefaultValue(), -40),
          "load: brightness");
  require(doubleParam(loadedFx, "alpha")->getKeyframeCount() == 1,
          "load: alpha key");
  require(isNear(doubleParam(loadedFx, "redMul")->getDefaultValue(), 100),
          "load: a parameter not saved keeps its default");
  require(pixelParam(loadedFx, CE::tintColorParamName())->getDefaultValue() ==
              TPixel32(255, 0, 0),
          "load: tint color");
  require(loaded->isColorEffectUsed(), "load: not used");

  // A column without the effect reads back without it
  TXshLevelColumnP plain(new TXshLevelColumn());
  loadColumn(*plain, plainPath);
  require(!plain->isColorEffectUsed(), "load: used when not saved");

  // A parameter this build does not know is skipped by the loader, as older
  // builds skip the parameters of the Color Effect
  QByteArray future = contents(plainPath);
  require(future.contains("<params>"), "file: no params tag");
  future.replace("<params>",
                 "<params><futureParam><default>3</default></futureParam>");
  const QString futurePath = QDir(dir).filePath("future.xml");
  writeBytes(futurePath, future);
  TXshLevelColumnP skipped(new TXshLevelColumn());
  loadColumn(*skipped, futurePath);
  require(!skipped->isColorEffectUsed(), "load: unknown parameter");
}

}  // namespace

int main() {
  try {
    testParams();
    testIdentity();
    testBrightness();
    testTint();
    testAlpha();
    testAdvanced();
    testOrder();
    testClamping();
    testPremultiplied();
    std::cout << "ok: color effect math" << std::endl;

    QTemporaryDir dir;
    require(dir.isValid(), "no temporary directory");
    testColumnFx(dir.path());
    std::cout << "ok: column fx parameters" << std::endl;
  } catch (const TException &e) {
    std::cerr << "columncoloreffect_test failed: "
              << ::to_string(e.getMessage()) << std::endl;
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "columncoloreffect_test failed: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "columncoloreffect_test passed" << std::endl;
  return 0;
}
