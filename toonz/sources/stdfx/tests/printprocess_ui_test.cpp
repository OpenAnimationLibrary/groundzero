#include "../printprocessfx.cpp"
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
#include <map>
#include <memory>
#include <vector>
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
  TFxP actual  = new PrintProcessFx;
  TFxP preview = actual->clone(false);
  // Every parameter has exactly one editor across the three pages
  std::map<std::string, int> editors;
  std::vector<std::unique_ptr<ParamsPage>> pages;
  TIStream stream(TFilePath(layout.toStdWString()));
  std::string tag;
  require(stream.matchTag(tag) && tag == "fxlayout", "Missing layout root");
  while (stream.matchTag(tag)) {
    require(tag == "page", "Unexpected layout tag " + tag);
    pages.emplace_back(new ParamsPage);
    pages.back()->setPage(stream, actual, false);
    pages.back()->setFx(preview, actual, 0);
    for (auto *field : pages.back()->findChildren<ParamField *>())
      ++editors[field->property("fxParamName").toString().toStdString()];
  }
  require(pages.size() == 3, "Expected the Screen, Plates and Paper pages");
  TParamContainer *params = actual->getParams();
  for (int i = 0; i < params->getParamCount(); ++i) {
    const std::string name = params->getParamName(i);
    require(editors[name] == 1, "Parameter " + name + " has " +
                                    std::to_string(editors[name]) + " editors");
  }

  // Keys, undo and redo on the cell size
  ParamsPage &screen = *pages.front();
  auto *cell         = namedField(screen, "cellSize");
  require(QMetaObject::invokeMethod(cell, "onKeyToggled", Qt::DirectConnection),
          "No key control on the cell size");
  require(numeric(actual, "cellSize")->isKeyframe(0), "Cell size not keyed");
  TUndoManager::manager()->undo();
  screen.update(0);
  require(!numeric(actual, "cellSize")->hasKeyframes(), "Key undo failed");
  TUndoManager::manager()->redo();
  screen.update(0);
  require(numeric(actual, "cellSize")->isKeyframe(0), "Key redo failed");
  numeric(actual, "offsetBlackX")->setValue(10, 2.5);
  pages[1]->update(10);
  require(numeric(preview, "offsetBlackX")->getValue(10) == 2.5,
          "The preview ignored animated misregistration");
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
    std::cout << "PASS: every print control on three pages, keys and undo\n";
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
