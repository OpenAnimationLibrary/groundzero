#include "../celsmoother2fx.cpp"
#include "tenv.h"
#include "tparamcontainer.h"
#include "tstream.h"
#include "tthread.h"
#include "toonz/toonzfolders.h"
#include "toonzqt/fxsettings.h"
#include "toonzqt/paramfield.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
TPixelParamP color(const TFxP &fx, int index) {
  return TParamP(fx->getParams()->getParam(gammaSlot(index)));
}
TDoubleParamP count(const TFxP &fx) {
  return TParamP(fx->getParams()->getParam("numberOfGammaColors"));
}
PixelParamField *fieldFor(ParamsPage &page, int index) {
  for (PixelParamField *field : page.findChildren<PixelParamField *>())
    if (field->property("fxParamName").toString().toStdString() == gammaSlot(index))
      return field;
  throw std::runtime_error("Color entry has no native editor");
}
AnimatedChoiceParamField *choiceFor(ParamsPage &page, const QString &name) {
  for (auto *field : page.findChildren<AnimatedChoiceParamField *>())
    if (field->property("fxParamName").toString() == name) return field;
  throw std::runtime_error("Discrete control has no native animated editor");
}
void testPage(const QString &layout) {
  std::cout << "Creating effect and preview" << std::endl;
  TFxP actual = new CelSmoother2Fx;
  count(actual)->setDefaultValue(5);
  TDoubleParamP mode = TParamP(actual->getParams()->getParam("gammaCorrection"));
  mode->setDefaultValue(1);
  color(actual, 4)->getRed()->setDefaultValue(0.123456789);
  TFxP preview = actual->clone(false);
  std::cout << "Creating parameter page" << std::endl;
  ParamsPage page;
  TIStream stream(TFilePath(layout.toStdWString()));
  std::string tag;
  require(stream.matchTag(tag) && tag == "fxlayout", "Missing layout root");
  require(stream.matchTag(tag) && tag == "page", "Missing effect page");
  std::cout << "Loading native controls" << std::endl;
  page.setPage(stream, actual, false);
  std::cout << "Binding native controls" << std::endl;
  page.setFx(preview, actual, 0);
  auto *selector = page.findChild<QComboBox *>("indexedColorSelector");
  require(selector && selector->count() == 5, "Color list is incomplete");
  require(page.findChildren<PixelParamField *>().size() == 6,
          "Not all colors have native bound controls");
  require(color(preview, 4)->getValueD(0).r == 0.123456789,
          "Opening the editor quantized a color");
  require(page.findChildren<AnimatedChoiceParamField *>().size() == 4,
          "A key toggle or color-space choice is missing");
  auto *version = choiceFor(page, "smootherVersion");
  auto *versionCombo = version->findChild<QComboBox *>("animatedChoice");
  require(versionCombo && versionCombo->count() == 2 &&
              versionCombo->currentIndex() == 1,
          "Version selector has the wrong choices or default");
  require(QMetaObject::invokeMethod(version, "onChoiceChanged", Qt::DirectConnection,
                                    Q_ARG(int, 0)), "Version choice cannot be edited");
  TDoubleParamP versionParam = TParamP(actual->getParams()->getParam("smootherVersion"));
  require(versionParam->getValue(0) == 0, "Version selector did not update the effect");
  TUndoManager::manager()->undo();
  page.update(0);
  require(versionParam->getValue(0) == 1 && versionCombo->currentIndex() == 1,
          "Version undo did not refresh the selector");
  require(QMetaObject::invokeMethod(version, "onKeyToggled", Qt::DirectConnection),
          "Version selector cannot be keyed");
  require(versionParam->isKeyframe(0), "Version key was not committed");
  TUndoManager::manager()->undo();
  page.update(0);
  require(!versionParam->hasKeyframes(), "Version key undo failed");
  auto *key = choiceFor(page, "useColorKey");
  auto *checkbox = key->findChild<QCheckBox *>("animatedCheckbox");
  require(checkbox && !checkbox->isChecked(), "Color key toggle has the wrong default");
  checkbox->click();
  TDoubleParamP keyParam = TParamP(actual->getParams()->getParam("useColorKey"));
  require(keyParam->getValue(0) == 1, "Color key checkbox cannot change the effect");
  TUndoManager::manager()->undo();
  page.update(0);
  require(keyParam->getValue(0) == 0 && !checkbox->isChecked(), "Checkbox undo failed");
  for (int index : {0, 2, 4}) {
    std::cout << "Checking color entry " << index + 1 << std::endl;
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
  auto *last = fieldFor(page, 4);
  std::cout << "Checking key insertion and undo" << std::endl;
  require(QMetaObject::invokeMethod(last, "onKeyToggled", Qt::DirectConnection),
          "Key toggle unavailable");
  require(color(actual, 4)->isKeyframe(0) &&
              color(actual, 4)->getValueD(0).r == 0.123456789,
          "Key insertion quantized a color");
  TUndoManager::manager()->undo();
  page.update(0);
  require(!color(actual, 4)->hasKeyframes(), "Key toggle undo failed");
  require(color(actual, 4)->getValueD(0).r == 0.123456789,
          "Key undo changed default color");
  std::cout << "Checking enabled count and animation" << std::endl;
  count(actual)->setDefaultValue(1);
  page.update(0);
  require(!last->isEnabled() && fieldFor(page, 0)->isEnabled(),
          "Enabled count did not update the list");
  count(actual)->setDefaultValue(5);
  page.update(0);
  require(last->isEnabled() && color(actual, 4)->getValueD(0).r == 0.123456789,
          "Inactive entry did not survive count changes");
  count(actual)->setValue(0, 0);
  count(actual)->setValue(10, 5);
  page.update(0);
  require(!fieldFor(page, 0)->isEnabled(), "Animated zero count was ignored");
  page.update(10);
  require(last->isEnabled(), "Animated maximum count was ignored");
  mode->setDefaultValue(2);
  page.update(10);
  require(!last->isEnabled(), "All Colors still exposes inactive palette controls");
  mode->setDefaultValue(1);
  page.update(10);
  require(last->isEnabled(), "Gamma Colors did not restore its palette controls");
  TUndoManager::manager()->reset();
  std::cout << "Closing parameter page" << std::endl;
}
}  // namespace

int main(int argc, char **argv) {
  std::cout << "Initializing Qt application" << std::endl;
  QApplication app(argc, argv);
  // ParamsPage owns a histogram renderer, whose executor requires the same
  // main-thread initialization used by the application and render tests.
  TThread::init();
  int result = 1;
  try {
    require(argc == 2, "Expected the installed effect layout");
    // Native color widgets load preferences. Give them an isolated settings
    // tree just as the application does before constructing its widgets.
    QTemporaryDir environment;
    require(environment.isValid(), "Cannot create the test environment");
    const QString stuff = environment.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default") &&
                QDir().mkpath(stuff + "/config"),
            "Cannot create test resource directories");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
    require(QDir().mkpath(ToonzFolder::getMyModuleDir().getQString()),
            "Cannot create test settings directory");
    testPage(QString::fromLocal8Bit(argv[1]));
    std::cout << "PASS: complete gamma list, sampling, undo, keys and precision\n";
    result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  TThread::shutdown();
  return result;
}
