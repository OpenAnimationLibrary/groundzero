#include "../celcolorkeepfx.cpp"
#include "tparamcontainer.h"
#include "tstream.h"
#include "toonzqt/fxsettings.h"
#include "toonzqt/paramfield.h"

#include <QApplication>
#include <QComboBox>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
TPixelParamP color(const TFxP &fx, int index) {
  return TParamP(fx->getParams()->getParam(colorSlotName(index)));
}
TDoubleParamP count(const TFxP &fx) {
  return TParamP(fx->getParams()->getParam("enabledColors"));
}
PixelParamField *fieldFor(ParamsPage &page, int index) {
  for (PixelParamField *field : page.findChildren<PixelParamField *>())
    if (field->property("fxParamName").toString().toStdString() == colorSlotName(index))
      return field;
  throw std::runtime_error("Color entry has no native editor");
}
void testPage(const QString &layout) {
  TFxP actual = new CelColorKeepFx;
  count(actual)->setDefaultValue(100);
  color(actual, 99)->getRed()->setDefaultValue(0.123456789);
  TFxP preview = actual->clone(false);
  ParamsPage page;
  TIStream stream(TFilePath(layout.toStdWString()));
  std::string tag;
  require(stream.matchTag(tag) && tag == "fxlayout", "Missing layout root");
  require(stream.matchTag(tag) && tag == "page", "Missing effect page");
  page.setPage(stream, actual, false);
  page.setFx(preview, actual, 0);
  auto *selector = page.findChild<QComboBox *>("indexedColorSelector");
  require(selector && selector->count() == 100, "Color list is incomplete");
  require(page.findChildren<PixelParamField *>().size() == 100,
          "Not all colors have native bound controls");
  require(color(preview, 99)->getValueD(0).r == 0.123456789,
          "Opening the editor quantized a color");
  for (int index : {0, 49, 99}) {
    const TPixelD before = color(actual, index)->getValueD(0);
    selector->setCurrentIndex(index);
    require(color(actual, index)->getValueD(0) == before,
            "List navigation changed a color");
    auto *field = fieldFor(page, index);
    require(field->isEnabled(), "Active color cannot be edited");
    field->setColor(TPixel32(42, 65, 127));
    require(color(actual, index)->getValue(0) == TPixel32(42, 65, 127),
            "Color picker edited the wrong entry");
    TUndoManager::manager()->undo();
    page.update(0);
    require(color(actual, index)->getValueD(0) == before,
            "Undo lost the original precision");
    TUndoManager::manager()->redo();
    page.update(0);
    require(color(actual, index)->getValue(0) == TPixel32(42, 65, 127),
            "Redo did not restore the sampled color");
    TUndoManager::manager()->undo();
    page.update(0);
  }
  auto *last = fieldFor(page, 99);
  require(QMetaObject::invokeMethod(last, "onKeyToggled", Qt::DirectConnection),
          "Key toggle unavailable");
  require(color(actual, 99)->isKeyframe(0) &&
              color(actual, 99)->getValueD(0).r == 0.123456789,
          "Key insertion quantized a color");
  TUndoManager::manager()->undo();
  page.update(0);
  require(!color(actual, 99)->hasKeyframes(), "Key toggle undo failed");
  require(color(actual, 99)->getValueD(0).r == 0.123456789,
          "Key undo changed default color");
  count(actual)->setDefaultValue(1);
  page.update(0);
  require(!last->isEnabled() && fieldFor(page, 0)->isEnabled(),
          "Enabled count did not update the list");
  count(actual)->setDefaultValue(100);
  page.update(0);
  require(last->isEnabled() && color(actual, 99)->getValueD(0).r == 0.123456789,
          "Inactive entry did not survive count changes");
  count(actual)->setValue(0, 0);
  count(actual)->setValue(10, 100);
  page.update(0);
  require(!fieldFor(page, 0)->isEnabled(), "Animated zero count was ignored");
  page.update(10);
  require(last->isEnabled(), "Animated maximum count was ignored");
  TUndoManager::manager()->reset();
}
}  // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  try {
    require(argc == 2, "Expected the installed effect layout");
    testPage(QString::fromLocal8Bit(argv[1]));
    std::cout << "PASS: full color list, sampling, undo, keys and precision\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
