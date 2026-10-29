#include "tdoubleparam.h"
#include "tenv.h"
#include "tfx.h"
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
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

DV_IMPORT_API void initStdFx();

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

void checkPages(const QString &layout) {
  TFxP actual  = TFx::create("STD_particlesFx");
  require(bool(actual), "Particles FX is not registered");
  TFxP preview = actual->clone(false);
  // No parameter has two editors, and the weather ones are on their page
  std::map<std::string, int> editors;
  std::vector<std::unique_ptr<ParamsPage>> pages;
  ParamsPage *weather = nullptr;
  TIStream stream(TFilePath(layout.toStdWString()));
  std::string tag;
  require(stream.matchTag(tag) && tag == "fxlayout", "Missing layout root");
  while (stream.matchTag(tag)) {
    require(tag == "page", "Unexpected layout tag " + tag);
    const std::string name = stream.getTagAttribute("name");
    pages.emplace_back(new ParamsPage);
    pages.back()->setPage(stream, actual, false);
    pages.back()->setFx(preview, actual, 0);
    if (name == "Weather") weather = pages.back().get();
    for (auto *field : pages.back()->findChildren<ParamField *>())
      ++editors[field->property("fxParamName").toString().toStdString()];
  }
  require(weather, "No Weather page");
  for (const auto &entry : editors)
    require(entry.second == 1, "Parameter " + entry.first + " has " +
                                   std::to_string(entry.second) + " editors");
  for (const char *name :
       {"collision_ctrl", "collision_mode", "turbulence", "turbulence_size",
        "turbulence_speed", "depth_layers", "depth_far_scale",
        "depth_far_opacity"}) {
    require(actual->getParams()->getParam(name),
            std::string("No parameter ") + name);
    namedField(*weather, name);
  }

  // Keys, undo and redo on the turbulence
  auto *turbulence = namedField(*weather, "turbulence");
  require(QMetaObject::invokeMethod(turbulence, "onKeyToggled",
                                    Qt::DirectConnection),
          "No key control on the turbulence");
  require(numeric(actual, "turbulence")->isKeyframe(0), "Turbulence not keyed");
  TUndoManager::manager()->undo();
  weather->update(0);
  require(!numeric(actual, "turbulence")->hasKeyframes(), "Key undo failed");
  TUndoManager::manager()->redo();
  weather->update(0);
  require(numeric(actual, "turbulence")->isKeyframe(0), "Key redo failed");
  numeric(actual, "depth_far_scale")->setValue(10, 0.2);
  weather->update(10);
  require(numeric(preview, "depth_far_scale")->getValue(10) == 0.2,
          "The preview ignored an animated far size");
  TUndoManager::manager()->reset();
}
}  // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  TThread::init();
  int result = 1;
  try {
    initStdFx();
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
    checkPages(QString::fromLocal8Bit(argv[1]));
    std::cout << "PASS: the weather page, one editor per particle control, "
                 "keys and undo\n";
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
