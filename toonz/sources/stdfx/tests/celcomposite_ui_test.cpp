#include "../lightwrapfx.cpp"
#include "../edgeextendfx.cpp"
#include "../mattechokefx.cpp"
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

// Every parameter has one editor, and a value keys, undoes and redoes
void checkPage(const QString &layout, TFxP actual, const std::string &keyed) {
  TFxP preview = actual->clone(false);
  ParamsPage page;
  TIStream stream(TFilePath(layout.toStdWString()));
  std::string tag;
  require(stream.matchTag(tag) && tag == "fxlayout", "Missing layout root");
  require(stream.matchTag(tag) && tag == "page", "Missing page");
  page.setPage(stream, actual, false);
  page.setFx(preview, actual, 0);
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
  auto *field = namedField(page, keyed);
  require(
      QMetaObject::invokeMethod(field, "onKeyToggled", Qt::DirectConnection),
      "No key control on " + keyed);
  require(numeric(actual, keyed)->isKeyframe(0), keyed + " not keyed");
  TUndoManager::manager()->undo();
  page.update(0);
  require(!numeric(actual, keyed)->hasKeyframes(), "Key undo failed");
  TUndoManager::manager()->redo();
  page.update(0);
  require(numeric(actual, keyed)->isKeyframe(0), "Key redo failed");
  numeric(actual, keyed)->setValue(10, 3.25);
  page.update(10);
  require(numeric(preview, keyed)->getValue(10) == 3.25,
          "The preview ignored an animated " + keyed);
  TUndoManager::manager()->reset();
}
}  // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  TThread::init();
  int result = 1;
  try {
    require(argc == 4, "Expected the three effect layouts");
    QTemporaryDir environment;
    require(environment.isValid(), "Cannot create environment");
    QString stuff = environment.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default") &&
                QDir().mkpath(stuff + "/config"),
            "Cannot create resource directories");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
    require(QDir().mkpath(ToonzFolder::getMyModuleDir().getQString()),
            "Cannot create settings");
    checkPage(QString::fromLocal8Bit(argv[1]), new LightWrapFx, "width");
    checkPage(QString::fromLocal8Bit(argv[2]), new EdgeExtendFx, "radius");
    checkPage(QString::fromLocal8Bit(argv[3]), new MatteChokeFx, "amount");
    std::cout << "PASS: light wrap, edge extend and matte choke pages, keys "
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
