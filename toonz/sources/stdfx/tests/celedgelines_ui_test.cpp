#include "../celedgelinesfx.cpp"
#include "tenv.h"
#include "tparamcontainer.h"
#include "tstream.h"
#include "tthread.h"
#include "tundo.h"
#include "toonz/toonzfolders.h"
#include "toonzqt/fxsettings.h"
#include "toonzqt/paramfield.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
ParamField *namedField(ParamsPage &page, const std::string &name) {
  for (auto *field : page.findChildren<ParamField *>())
    if (field->property("fxParamName").toString().toStdString() == name)
      return field;
  throw std::runtime_error("No editor for parameter " + name);
}
TDoubleParamP numeric(const TFxP &fx, const std::string &name) {
  return TParamP(fx->getParams()->getParam(name));
}
void choose(ParamsPage &page, const std::string &name, int value) {
  auto *field =
      dynamic_cast<AnimatedChoiceParamField *>(namedField(page, name));
  require(field && QMetaObject::invokeMethod(field, "onChoiceChanged",
                                             Qt::DirectConnection,
                                             Q_ARG(int, value)),
          "Choice " + name + " is unavailable");
}

void checkPage(const QString &layout) {
  TFxP actual = new CelEdgeLinesFx;
  numeric(actual, "enabledRules")->setDefaultValue(8);
  TFxP preview = actual->clone(false);
  ParamsPage page;
  TIStream stream(TFilePath(layout.toStdWString()));
  std::string tag;
  require(stream.matchTag(tag) && tag == "fxlayout", "Missing layout root");
  require(stream.matchTag(tag) && tag == "page", "Missing page");
  page.setPage(stream, actual, false);
  page.setFx(preview, actual, 0);

  // Every parameter has exactly one editor
  TParamContainer *params = actual->getParams();
  for (int i = 0; i < params->getParamCount(); ++i) {
    const std::string name = params->getParamName(i);
    int count              = 0;
    for (auto *field : page.findChildren<ParamField *>())
      if (field->property("fxParamName").toString().toStdString() == name)
        ++count;
    require(count == 1,
            "Parameter " + name + " has " + std::to_string(count) + " editors");
  }

  // A rule's colour shows only when its end is a colour. Only the rule
  // chosen in the group's selector is shown.
  auto visible = [&](const std::string &name) {
    return namedField(page, name)->isVisibleTo(&page);
  };
  auto *selector = page.findChild<QComboBox *>("indexedColorSelector");
  require(selector && selector->count() == 8, "Missing rule selector");
  selector->setCurrentIndex(7);
  require(visible("use008") && visible("weight008") && visible("lineColor008"),
          "The chosen rule's controls are hidden");
  require(!visible("weight001"), "An unchosen rule is shown");
  require(!visible("firstColor008") && !visible("secondColor008"),
          "Colours of Any Colour ends are shown");
  choose(page, "firstKind008", 1);
  page.update(0);
  require(visible("firstColor008") && !visible("secondColor008"),
          "A Colour end did not show its colour");
  choose(page, "secondKind008", 2);
  page.update(0);
  require(!visible("secondColor008"), "A Transparent end shows a colour");

  // Keys, undo and redo on a rule's weight
  auto *weight = namedField(page, "weight003");
  require(
      QMetaObject::invokeMethod(weight, "onKeyToggled", Qt::DirectConnection),
      "No key control on the weight");
  require(numeric(actual, "weight003")->isKeyframe(0), "Weight not keyed");
  TUndoManager::manager()->undo();
  page.update(0);
  require(!numeric(actual, "weight003")->hasKeyframes(), "Key undo failed");
  TUndoManager::manager()->redo();
  page.update(0);
  require(numeric(actual, "weight003")->isKeyframe(0), "Key redo failed");
  numeric(actual, "weight003")->setValue(10, 7.5);
  page.update(10);
  require(numeric(preview, "weight003")->getValue(10) == 7.5,
          "The preview ignored an animated weight");
  TUndoManager::manager()->reset();
}
}  // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  TThread::init();
  int result = 1;
  try {
    require(argc == 2, "Expected the effect layout");
    QTemporaryDir environment;
    require(environment.isValid(), "Cannot create environment");
    QString stuff = environment.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default") &&
                QDir().mkpath(stuff + "/config"),
            "Cannot create resource directories");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
    require(QDir().mkpath(ToonzFolder::getMyModuleDir().getQString()),
            "Cannot create settings");
    checkPage(QString::fromLocal8Bit(argv[1]));
    std::cout << "PASS: every edge line control, conditional colours, keys "
                 "and undo\n";
    result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  TThread::shutdown();
  return result;
}
