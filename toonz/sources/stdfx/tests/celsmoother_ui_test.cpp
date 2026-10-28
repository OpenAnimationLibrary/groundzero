#include "../celsmootherfx.cpp"
#include "tenv.h"
#include "tdoublekeyframe.h"
#include "tparamcontainer.h"
#include "tstream.h"
#include "tthread.h"
#include "toonz/toonzfolders.h"
#include "toonzqt/fxsettings.h"
#include "toonzqt/paramfield.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
TDoubleParamP number(const TFxP &fx, const char *name) {
  return TParamP(fx->getParams()->getParam(name));
}
TPixelParamP color(const TFxP &fx) {
  return TParamP(fx->getParams()->getParam("colorKey"));
}
ParamField *field(ParamsPage &page, const char *name) {
  for (auto *item : page.findChildren<ParamField *>())
    if (item->getParamName() == QString::fromLatin1(name)) return item;
  throw std::runtime_error("Native parameter field missing");
}
void testPage(const QString &layout) {
  TFxP actual = new CelSmootherFx;
  color(actual)->getRed()->setDefaultValue(.123456789123);
  TFxP preview = actual->clone(false);
  ParamsPage page;
  TIStream stream(TFilePath(layout.toStdWString()));
  std::string tag;
  require(stream.matchTag(tag) && tag == "fxlayout", "Layout missing");
  require(stream.matchTag(tag) && tag == "page", "Page missing");
  page.setPage(stream, actual, false);
  page.setFx(preview, actual, 0);
  auto *use = field(page,"useColorKey");
  auto *key = dynamic_cast<PixelParamField *>(field(page,"colorKey"));
  auto *range = field(page,"smoothRange");
  auto *check = use->findChild<QCheckBox *>("animatedCheckbox");
  require(check && key && range, "Complete control set missing");
  require(!check->isChecked() && number(actual,"smoothRange")->getValue(0) == 6,
          "Defaults changed when opening editor");
  require(color(actual)->getValueD(0).r == .123456789123,
          "Opening color editor quantized data");
  double minimum, maximum, step;
  number(actual,"smoothRange")->getValueRange(minimum, maximum, step);
  require(minimum == 0 && maximum == 255, "Full typed range unavailable");
  check->click();
  require(number(actual,"useColorKey")->getValue(0) == 1, "Checkbox did not edit scene");
  TUndoManager::manager()->undo(); page.update(0);
  require(!check->isChecked() && number(actual,"useColorKey")->getValue(0) == 0,
          "Checkbox undo failed");
  TUndoManager::manager()->redo(); page.update(0);
  require(check->isChecked(), "Checkbox redo failed");
  require(QMetaObject::invokeMethod(use,"onKeyToggled",Qt::DirectConnection),
          "Checkbox cannot be animated");
  require(number(actual,"useColorKey")->getKeyframeAt(0).m_type == TDoubleKeyframe::Constant,
          "Checkbox inserted a nonconstant key");
  TUndoManager::manager()->undo(); page.update(0);
  require(!number(actual,"useColorKey")->hasKeyframes(), "Checkbox key undo failed");
  key->setColor(TPixel32(12,34,56));
  require(color(actual)->getValue(0) == TPixel32(12,34,56), "Color picker failed");
  TUndoManager::manager()->undo(); page.update(0);
  require(color(actual)->getValueD(0).r == .123456789123, "Color undo lost precision");
  for (auto *control : {static_cast<ParamField *>(key), range}) {
    require(QMetaObject::invokeMethod(control,"onKeyToggled",Qt::DirectConnection),
            "Native key toggle missing");
    TUndoManager::manager()->undo(); page.update(0);
  }
  number(actual,"smoothRange")->setValue(0,0);
  number(actual,"smoothRange")->setValue(10,255);
  page.update(10);
  require(number(preview,"smoothRange")->getValue(10) == 255,
          "Maximum animated range failed in editor");
  TUndoManager::manager()->reset();
}
}  // namespace
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  TThread::init();
  int result = 1;
  try {
    require(argc == 2, "Expected packaged effect layout");
    QTemporaryDir temporary;
    require(temporary.isValid(), "Temporary settings unavailable");
    const QString stuff = temporary.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default") &&
                QDir().mkpath(stuff + "/config"), "Cannot create settings");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
    require(QDir().mkpath(ToonzFolder::getMyModuleDir().getQString()),
            "Cannot create module settings");
    testPage(QString::fromLocal8Bit(argv[1]));
    std::cout << "PASS: smoother controls, undo, animation and precision\n";
    result = 0;
  } catch (const TException &e) {
    std::cerr << QString::fromStdWString(e.getMessage()).toStdString() << '\n';
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; }
  TThread::shutdown();
  return result;
}
