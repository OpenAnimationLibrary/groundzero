#include "../celcolorkeyfx.cpp"
#include "tenv.h"
#include "tparamcontainer.h"
#include "tstream.h"
#include "tthread.h"
#include "toonz/toonzfolders.h"
#include "toonzqt/fxsettings.h"
#include "toonzqt/paramfield.h"
#include "toonzqt/doublefield.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QTemporaryDir>
#include <QSlider>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
TPixelParamP color(const TFxP &fx, int index) {
  return TParamP(fx->getParams()->getParam(slotName("key", index)));
}
TDoubleParamP count(const TFxP &fx) {
  return TParamP(fx->getParams()->getParam("enabledColors"));
}
PixelParamField *fieldFor(ParamsPage &page, int index) {
  for (PixelParamField *field : page.findChildren<PixelParamField *>())
    if (field->property("fxParamName").toString().toStdString() == slotName("key", index))
      return field;
  throw std::runtime_error("Color entry has no native editor");
}
ParamField *namedField(ParamsPage &page, const std::string &name) {
  for (auto *field : page.findChildren<ParamField *>())
    if (field->property("fxParamName").toString().toStdString() == name)
      return field;
  throw std::runtime_error("Required parameter field is missing");
}
TDoubleParamP numeric(const TFxP &fx, const std::string &name) {
  return TParamP(fx->getParams()->getParam(name));
}
void loadPage(ParamsPage &page, const QString &layout, const TFxP &actual,
              const TFxP &preview) {
  TIStream stream(TFilePath(layout.toStdWString()));
  std::string tag;
  require(stream.matchTag(tag) && tag == "fxlayout", "Missing layout root");
  require(stream.matchTag(tag) && tag == "page", "Missing effect page");
  page.setPage(stream, actual, false);
  page.setFx(preview, actual, 0);
}
void choose(ParamsPage &page, const std::string &name, int value) {
  auto *field = dynamic_cast<AnimatedChoiceParamField *>(namedField(page, name));
  require(field && QMetaObject::invokeMethod(field, "onChoiceChanged",
              Qt::DirectConnection, Q_ARG(int, value)), "Choice is unavailable");
}

void testConditionalControls(const QString &layout) {
  TFxP actual = new CelColorKeyFx;
  count(actual)->setDefaultValue(25);
  numeric(actual, "threshold025")->setDefaultValue(0.123456789);
  numeric(actual, "component1025")->setValue(10, 0.25);
  TPixelParamP replacement = TParamP(actual->getParams()->getParam("replacement025"));
  replacement->getRed()->setDefaultValue(0.7654321);
  TFxP preview = actual->clone(false);
  ParamsPage page;
  loadPage(page, layout, actual, preview);
  auto *selector = page.findChild<QComboBox *>("indexedColorSelector");
  require(selector, "Missing entry selector");
  auto visible = [&](const std::string &name) {
    return namedField(page, name)->isVisibleTo(&page);
  };
  // Each strategy exposes exactly the thresholds that drive matching, across
  // both ends of the indexed list. Hiding a field must preserve its curves.
  for (int perColor : {0, 1}) {
    choose(page, "perColor", perColor);
    for (int perComponent : {0, 1}) {
      choose(page, "perComponent", perComponent);
      page.update(0);
      require(visible("threshold") == (!perColor && !perComponent),
              "Global scalar threshold visibility is incorrect");
      for (int component = 1; component <= 3; ++component)
        require(visible("component" + std::to_string(component)) ==
                    (!perColor && perComponent),
                "Global component threshold visibility is incorrect");
      for (int index : {0, 24}) {
        selector->setCurrentIndex(index);
        require(visible(slotName("use", index)) && visible(slotName("key", index)),
                "Threshold modes hid a color's selection controls");
        require(visible(slotName("threshold", index)) == (perColor && !perComponent),
                "Entry scalar threshold visibility is incorrect");
        for (int component = 1; component <= 3; ++component) {
          const std::string prefix = "component" + std::to_string(component);
          require(visible(slotName(prefix.c_str(), index)) ==
                      (perColor && perComponent),
                  "Entry component threshold visibility is incorrect");
        }
      }
    }
  }
  require(numeric(actual, "threshold025")->getValue(0) == 0.123456789 &&
              numeric(actual, "component1025")->isKeyframe(10),
          "Threshold visibility changed stored values or animation");

  require(!visible("enableReplace") && !visible("replace025") &&
              !visible("replacement025"), "Remove mode exposed replacement controls");
  choose(page, "keep", 1);
  require(visible("enableReplace") && !visible("replace025") &&
              !visible("replacement025"), "Disabled replacement controls were shown");
  choose(page, "enableReplace", 1);
  require(visible("replace025") && visible("replacement025"),
          "Enabled replacement controls were hidden");
  choose(page, "enableReplace", 0);
  require(!visible("replace025") && !visible("replacement025"),
          "Disabling replacement did not hide its controls");
  TUndoManager::manager()->undo();
  page.update(0);
  require(visible("replace025") && visible("replacement025") &&
              replacement->getValueD(0).r == 0.7654321,
          "Replacement undo lost visibility or stored precision");
  choose(page, "keep", 0);
  require(!visible("enableReplace") && !visible("replacement025"),
          "Changing to remove mode left replacement visible");
  choose(page, "keep", 1);
  numeric(actual, "enableReplace")->setValue(0, 0);
  numeric(actual, "enableReplace")->setValue(10, 1);
  page.update(0);
  require(!visible("replacement025"), "Animated replacement off was ignored");
  page.update(10);
  require(visible("replacement025"), "Animated replacement on was ignored");
  TUndoManager::manager()->reset();
}

void testSliderRanges(const QString &layout) {
  TFxP actual = new CelColorKeyFx;
  TFxP preview = actual->clone(false);
  ParamsPage page;
  loadPage(page, layout, actual, preview);
  for (const std::string name : {"thin", "blur"}) {
    const double hardMin = name == "thin" ? -4000.0 : 0.0;
    const double softMin = name == "thin" ? -100.0 : 0.0;
    double minimum, maximum, step;
    auto param = numeric(actual, name);
    require(param->getUISliderRange(minimum, maximum) && minimum == softMin &&
                maximum == 100.0, "Parameter lost its slider bounds");
    require(numeric(preview, name)->getUISliderRange(minimum, maximum) &&
                minimum == softMin && maximum == 100.0,
            "Cloning lost the slider bounds");
    TDoubleParamP copy(0.0);
    copy->copy(param.getPointer());
    require(copy->getUISliderRange(minimum, maximum) && minimum == softMin &&
                maximum == 100.0, "Copying lost the slider bounds");
    require(param->getValueRange(minimum, maximum, step) &&
                minimum == hardMin && maximum == 4000.0,
            "Slider bounds restricted typed parameter values");
    // XML precision is applied before preview/scene parameter binding.
    std::unique_ptr<ParamField> unbound(ParamField::create(nullptr, "Range", param));
    unbound->setPrecision(4);
    auto *unboundWidget = unbound->findChild<DVGui::MeasuredDoubleField *>();
    require(unboundWidget && unboundWidget->getSliderRange(minimum, maximum) &&
                minimum == softMin && maximum == 100.0,
            "Unbound precision setup lost slider bounds");
    auto *field = namedField(page, name);
    auto *widget = field->findChild<DVGui::MeasuredDoubleField *>();
    auto *line = widget ? widget->findChild<DVGui::MeasuredDoubleLineEdit *>() : nullptr;
    auto *slider = widget ? widget->findChild<QSlider *>() : nullptr;
    require(widget && line && slider, "Missing numeric editor components");
    auto type = [&](double value) {
      line->setText(QString::number(value, 'g', 16));
      line->postSetText();
    };
    type(250.0);
    require(param->getValue(0) == 250.0 && widget->getValue() == 250.0,
            "Slider endpoint overwrote a larger typed value");
    TUndoManager::manager()->undo(); page.update(0);
    require(param->getValue(0) == 0.0, "Typed value undo failed");
    type(4000.0);
    require(param->getValue(0) == 4000.0, "Maximum typed value was rejected");
    type(4001.0);
    require(param->getValue(0) == 4000.0, "Typed value exceeded its hard maximum");
    type(hardMin);
    require(param->getValue(0) == hardMin, "Minimum typed value was rejected");
    type(hardMin - 1.0);
    require(param->getValue(0) == hardMin, "Typed value exceeded its hard minimum");
    for (int position : {slider->maximum(), slider->minimum()}) {
      slider->setValue(position);
      require(QMetaObject::invokeMethod(slider, "sliderReleased", Qt::DirectConnection),
              "Slider release is unavailable");
      require(std::abs(param->getValue(0) -
                  (position == slider->maximum() ? 100.0 : softMin)) < 1e-8,
              "Slider did not use its independent bounds");
    }
    auto *roller = widget->findChild<DVGui::RollerField *>();
    require(roller, "Missing numeric roller");
    roller->setValue(250.0);
    require(QMetaObject::invokeMethod(widget, "onRollerValueChanged", Qt::DirectConnection,
                                      Q_ARG(bool, false)), "Roller update unavailable");
    require(param->getValue(0) == 250.0 && widget->getValue() == 250.0,
            "Slider endpoint overwrote a larger roller value");
    field->setPrecision(4);
    require(widget->getValue() == 250.0, "Precision refresh changed a typed value");
    slider->setValue(slider->minimum());
    slider->setValue(slider->maximum());
    QMetaObject::invokeMethod(slider, "sliderReleased", Qt::DirectConnection);
    require(std::abs(param->getValue(0) - 100.0) < 1e-8,
            "Precision refresh lost slider bounds");
    param->setValue(0, 250.0);
    param->setValue(10, 2000.0);
    page.update(10);
    require(widget->getValue() == 2000.0 && slider->value() == slider->maximum(),
            "Animated value was limited to the slider window");
    page.update(0);
    require(widget->getValue() == 250.0 && param->getValue(10) == 2000.0,
            "Frame changes altered values outside the slider window");
  }
  // Parameters without the opt-in retain their existing numeric/slider range.
  double minimum, maximum;
  require(!numeric(actual, "threshold")->getUISliderRange(minimum, maximum),
          "Ordinary parameter acquired optional slider bounds");
  DVGui::MeasuredDoubleField ordinary;
  ordinary.setDecimals(3);
  ordinary.setRange(-250.0, 250.0);
  auto *slider = ordinary.findChild<QSlider *>();
  slider->setValue(slider->maximum());
  require(std::abs(ordinary.getValue() - 250.0) < 1e-8,
          "Ordinary slider maximum changed");
  slider->setValue(slider->minimum());
  require(std::abs(ordinary.getValue() + 250.0) < 1e-8,
          "Ordinary slider minimum changed");
  require(!ordinary.setSliderRange(-300, 300), "Invalid slider bounds accepted");
  require(ordinary.setSliderRange(-100, 100), "Valid slider bounds rejected");
  ordinary.setValue(250.0);
  require(ordinary.getValue() == 250.0, "Optional range clamped numeric updates");
  ordinary.setRange(-250.0, 250.0);
  require(!ordinary.getSliderRange(minimum, maximum),
          "Ordinary range did not clear optional slider bounds");
  slider->setValue(slider->minimum());
  slider->setValue(slider->maximum());
  require(std::abs(ordinary.getValue() - 250.0) < 1e-8,
          "Restoring the ordinary range lost its slider maximum");
  TUndoManager::manager()->reset();
}
void testPage(const QString &layout) {
  std::cout << "Creating effect and preview" << std::endl;
  TFxP actual = new CelColorKeyFx;
  count(actual)->setDefaultValue(25);
  color(actual, 24)->getRed()->setDefaultValue(0.123456789);
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
  require(selector && selector->count() == 25, "Color list is incomplete");
  require(page.findChildren<PixelParamField *>().size() == 50,
          "Not all colors have native bound controls");
  require(color(preview, 24)->getValueD(0).r == 0.123456789,
          "Opening the editor quantized a color");
  AnimatedChoiceParamField *use = nullptr;
  for (auto *choice : page.findChildren<AnimatedChoiceParamField *>())
    if (choice->property("fxParamName").toString() == "use001") use = choice;
  require(use != nullptr, "The animated enable control is missing");
  TDoubleParamP useActual = TParamP(actual->getParams()->getParam("use001"));
  TDoubleParamP usePreview = TParamP(preview->getParams()->getParam("use001"));
  require(QMetaObject::invokeMethod(use, "onChoiceChanged", Qt::DirectConnection,
                                    Q_ARG(int, 1)), "Enable control is unavailable");
  require(useActual->getValue(0) == 1, "Enable control did not change the entry");
  TUndoManager::manager()->undo(); page.update(0);
  require(useActual->getValue(0) == 0, "Enable control undo failed");
  useActual->setValue(0, 0); useActual->setValue(10, 1); page.update(5);
  require(QMetaObject::invokeMethod(use, "onChoiceChanged", Qt::DirectConnection,
                                    Q_ARG(int, 1)), "Animated enable control is unavailable");
  require(useActual->getValue(5) == 0 && usePreview->getValue(5) == 1,
          "Between-key choice did not remain a preview");
  require(QMetaObject::invokeMethod(use, "onKeyToggled", Qt::DirectConnection),
          "Animated enable key toggle unavailable");
  require(useActual->isKeyframe(5) && useActual->getValue(5) == 1,
          "Animated enable preview was not committed");
  TUndoManager::manager()->undo(); page.update(0);
  require(!useActual->isKeyframe(5), "Animated enable key undo failed");
  for (int index : {0, 12, 24}) {
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
  auto *last = fieldFor(page, 24);
  std::cout << "Checking key insertion and undo" << std::endl;
  require(QMetaObject::invokeMethod(last, "onKeyToggled", Qt::DirectConnection),
          "Key toggle unavailable");
  require(color(actual, 24)->isKeyframe(0) &&
              color(actual, 24)->getValueD(0).r == 0.123456789,
          "Key insertion quantized a color");
  TUndoManager::manager()->undo();
  page.update(0);
  require(!color(actual, 24)->hasKeyframes(), "Key toggle undo failed");
  require(color(actual, 24)->getValueD(0).r == 0.123456789,
          "Key undo changed default color");
  std::cout << "Checking enabled count and animation" << std::endl;
  count(actual)->setDefaultValue(1);
  page.update(0);
  require(!last->isEnabled() && fieldFor(page, 0)->isEnabled(),
          "Enabled count did not update the list");
  count(actual)->setDefaultValue(25);
  page.update(0);
  require(last->isEnabled() && color(actual, 24)->getValueD(0).r == 0.123456789,
          "Inactive entry did not survive count changes");
  count(actual)->setValue(0, 0);
  count(actual)->setValue(10, 25);
  page.update(0);
  require(!fieldFor(page, 0)->isEnabled(), "Animated zero count was ignored");
  page.update(10);
  require(last->isEnabled(), "Animated maximum count was ignored");
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
    testConditionalControls(QString::fromLocal8Bit(argv[1]));
    testSliderRanges(QString::fromLocal8Bit(argv[1]));
    std::cout << "PASS: full color list, sampling, undo, keys and precision\n";
    result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  TThread::shutdown();
  return result;
}
