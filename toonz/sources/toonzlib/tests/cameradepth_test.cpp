// Regression test of the camera's depth channels (doc/multiplane_depth.md):
// Focus Z and Aperture, which are saved only when used, take part in the
// global keys only when used, and are read by expressions.

#include "toonz/tstageobject.h"
#include "toonz/txsheet.h"
#include "tdoubleparam.h"
#include "tconvert.h"
#include "tdoublekeyframe.h"
#include "tstream.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

TFilePath toPath(const QString &path) { return TFilePath(path.toStdWString()); }

QString saveXsheet(TXsheet *xsh, const QString &path) {
  {
    TOStream os(toPath(path));
    require(os.checkStatus(), "save: cannot open the file");
    os.openChild("xsheet");
    os << *xsh;
    os.closeChild();
  }
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "save: cannot read the file back");
  return QString::fromUtf8(file.readAll());
}

TXsheet *loadXsheet(const QString &path) {
  TXsheet *xsh = new TXsheet();
  TIStream is(toPath(path));
  require(bool(is), "load: cannot open the file");
  std::string tag;
  require(is.matchTag(tag) && tag == "xsheet", "load: no xsheet tag");
  xsh->loadData(is);
  return xsh;
}

TStageObject *camera(TXsheet *xsh) {
  return xsh->getStageObject(TStageObjectId::CameraId(0));
}

void testUnused(const QString &dir) {
  TXsheetP xsh      = new TXsheet();
  TStageObject *cam = camera(xsh.getPointer());
  require(cam->getParam(TStageObject::T_FocusZ)->getValue(0) == 0,
          "unused: the focus is not at the table");
  require(cam->getParam(TStageObject::T_Aperture)->getValue(0) == 0,
          "unused: the aperture is not 0 stops");
  require(!cam->isFullKeyframeChannel(TStageObject::T_FocusZ) &&
              !cam->isFullKeyframeChannel(TStageObject::T_Aperture),
          "unused: the depth channels take part in the keys");
  // A key on an unused camera keys the standard channels only
  cam->setKeyframeWithoutUndo(4);
  require(cam->isFullKeyframe(4), "unused: the key is not full");
  require(!cam->getParam(TStageObject::T_FocusZ)->isKeyframe(4) &&
              !cam->getParam(TStageObject::T_Aperture)->isKeyframe(4),
          "unused: the key keyed the depth channels");
  const QString text =
      saveXsheet(xsh.getPointer(), QDir(dir).filePath("unused.xsh"));
  require(!text.contains("<focus") && !text.contains("<aperture"),
          "unused: the depth channels are saved");
}

void testUsed(const QString &dir) {
  TXsheetP xsh           = new TXsheet();
  TStageObject *cam      = camera(xsh.getPointer());
  TDoubleParam *focus    = cam->getParam(TStageObject::T_FocusZ);
  TDoubleParam *aperture = cam->getParam(TStageObject::T_Aperture);
  // A rack focus: keys at 0 and 10
  focus->setValue(0, -50);
  focus->setValue(10, 200);
  aperture->setDefaultValue(2.5);
  require(cam->isFullKeyframeChannel(TStageObject::T_FocusZ) &&
              cam->isFullKeyframeChannel(TStageObject::T_Aperture),
          "used: the depth channels are not in the keys");
  require(std::abs(focus->getValue(5) - 75) < 1e-9,
          "used: the focus does not interpolate");
  // A global key now keys them too
  cam->setKeyframeWithoutUndo(20);
  require(focus->isKeyframe(20) && aperture->isKeyframe(20),
          "used: a global key does not key the depth channels");
  require(cam->isFullKeyframe(20), "used: the key is not full");
  cam->removeKeyframeWithoutUndo(20);
  require(!focus->isKeyframe(20) && !aperture->isKeyframe(20),
          "used: removing the key left the depth channels keyed");

  const QString path = QDir(dir).filePath("used.xsh");
  const QString text = saveXsheet(xsh.getPointer(), path);
  require(text.contains("<focus") && text.contains("<aperture"),
          "used: the depth channels are not saved");
  TXsheetP loaded    = loadXsheet(path);
  TStageObject *back = camera(loaded.getPointer());
  for (double frame : {0.0, 3.0, 10.0, 15.0})
    require(back->getParam(TStageObject::T_FocusZ)->getValue(frame) ==
                focus->getValue(frame),
            "used: the focus did not load back");
  require(back->getParam(TStageObject::T_Aperture)->getValue(7) == 2.5,
          "used: the aperture did not load back");

  // Copying the camera keeps them
  TStageObject *other        = xsh->getStageObject(TStageObjectId::CameraId(1));
  TStageObjectParams *params = cam->getParams();
  other->assignParams(params);
  delete params;
  require(other->getParam(TStageObject::T_FocusZ)->getValue(5) == 75,
          "used: assigning the camera lost the focus");
}

void testExpression() {
  // camera1.focus reads the focus as camera1.z reads the Z: same units
  TXsheetP xsh      = new TXsheet();
  TStageObject *cam = camera(xsh.getPointer());
  cam->getParam(TStageObject::T_FocusZ)->setDefaultValue(120);
  cam->getParam(TStageObject::T_Z)->setDefaultValue(120);
  cam->getParam(TStageObject::T_Aperture)->setDefaultValue(1.5);
  TStageObject *a = xsh->getStageObject(TStageObjectId::PegbarId(0));
  TStageObject *b = xsh->getStageObject(TStageObjectId::PegbarId(1));
  auto expression = [](TStageObject *object, const std::string &text) {
    // An expression segment, from the key at 0 to the one at 10
    TDoubleParam *x = object->getParam(TStageObject::T_X);
    x->deleteKeyframe(0);
    TDoubleKeyframe k0(0, 0), k1(10, 0);
    k0.m_type           = TDoubleKeyframe::Expression;
    k0.m_expressionText = text;
    x->setKeyframe(k0);
    x->setKeyframe(k1);
    return x->getValue(3);
  };
  const double viaFocus = expression(a, "camera1.focus");
  const double viaZ     = expression(b, "camera1.z");
  require(viaZ != 0 && viaFocus == viaZ,
          "expression: camera1.focus does not read the focus");
  // The aperture has no unit: it reads as the plain number
  require(expression(a, "camera1.aperture") == expression(b, "1.5"),
          "expression: camera1.aperture does not read the aperture");
}

}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir dir;
    require(dir.isValid(), "no temporary directory");
    testUnused(dir.path());
    testUsed(dir.path());
    testExpression();
  } catch (const TException &e) {
    std::cerr << "cameradepth_test: " << ::to_string(e.getMessage())
              << std::endl;
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "cameradepth_test: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "ok: camera focus and aperture" << std::endl;
  return 0;
}
