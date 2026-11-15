#pragma once

// The settings-page test shared by Motion Lines and Burst Lines: every
// parameter has one editor, and keys, undo and the preview copy work.

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
#include <stdexcept>
#include <string>
#include <vector>

namespace ComicLinesFxUiTest {

inline void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
inline ParamField *namedField(ParamsPage &page, const std::string &name) {
  for (auto *field : page.findChildren<ParamField *>())
    if (field->property("fxParamName").toString().toStdString() == name)
      return field;
  throw std::runtime_error("No editor for parameter " + name);
}
inline TDoubleParamP numeric(const TFxP &fx, const std::string &name) {
  return TParamP(fx->getParams()->getParam(name));
}

template <class Fx>
void checkPage(const QString &layout) {
  TFxP actual  = new Fx;
  TFxP preview = actual->clone(false);
  // Every parameter has exactly one editor across the two pages
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
  require(pages.size() == 2, "Expected the lines page and the motion page");
  TParamContainer *params = actual->getParams();
  for (int i = 0; i < params->getParamCount(); ++i) {
    const std::string name = params->getParamName(i);
    require(editors[name] == 1, "Parameter " + name + " has " +
                                    std::to_string(editors[name]) + " editors");
  }

  // Keys, undo and redo on the thickness
  ParamsPage &lines = *pages.front();
  auto *thickness   = namedField(lines, "thickness");
  require(QMetaObject::invokeMethod(thickness, "onKeyToggled",
                                    Qt::DirectConnection),
          "No key control on the thickness");
  require(numeric(actual, "thickness")->isKeyframe(0), "Thickness not keyed");
  TUndoManager::manager()->undo();
  lines.update(0);
  require(!numeric(actual, "thickness")->hasKeyframes(), "Key undo failed");
  TUndoManager::manager()->redo();
  lines.update(0);
  require(numeric(actual, "thickness")->isKeyframe(0), "Key redo failed");
  numeric(actual, "hold")->setValue(10, 4);
  pages[1]->update(10);
  require(numeric(preview, "hold")->getValue(10) == 4,
          "The preview ignored an animated hold");
  TUndoManager::manager()->reset();
}

template <class Fx>
int run(int argc, char **argv, const char *passed) {
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
    checkPage<Fx>(QString::fromLocal8Bit(argv[1]));
    std::cout << passed << '\n';
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

}  // namespace ComicLinesFxUiTest
