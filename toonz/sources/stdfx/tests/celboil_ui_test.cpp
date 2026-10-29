#include "../celboilfx.cpp"
#include "tenv.h"
#include "tparamcontainer.h"
#include "tstream.h"
#include "tthread.h"
#include "tundo.h"
#include "toonz/toonzfolders.h"
#include "toonzqt/fxsettings.h"
#include "toonzqt/paramfield.h"
#include <QApplication>
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
  TFxP actual  = new CelBoilFx;
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

  // The line threshold shows only when only lines boil
  auto visible = [&](const std::string &name) {
    return namedField(page, name)->isVisibleTo(&page);
  };
  require(!visible("lineThreshold"), "The line threshold shows for the image");
  choose(page, "target", 1);
  page.update(0);
  require(visible("lineThreshold"), "The line threshold is hidden for lines");

  // Keys, undo and redo on the amplitude
  auto *amplitude = namedField(page, "amplitude");
  require(QMetaObject::invokeMethod(amplitude, "onKeyToggled",
                                    Qt::DirectConnection),
          "No key control on the amplitude");
  require(numeric(actual, "amplitude")->isKeyframe(0), "Amplitude not keyed");
  TUndoManager::manager()->undo();
  page.update(0);
  require(!numeric(actual, "amplitude")->hasKeyframes(), "Key undo failed");
  TUndoManager::manager()->redo();
  page.update(0);
  require(numeric(actual, "amplitude")->isKeyframe(0), "Key redo failed");
  numeric(actual, "cycle")->setValue(10, 4);
  page.update(10);
  require(numeric(preview, "cycle")->getValue(10) == 4,
          "The preview ignored an animated cycle");
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
    std::cout << "PASS: every boil control, the line threshold, keys and "
                 "undo\n";
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
