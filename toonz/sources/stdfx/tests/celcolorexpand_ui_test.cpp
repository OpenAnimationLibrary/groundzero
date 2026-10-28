#include "../celcolorexpandfx.cpp"
#include "tenv.h"
#include "tparamcontainer.h"
#include "tstream.h"
#include "tthread.h"
#include "toonz/toonzfolders.h"
#include "toonzqt/fxsettings.h"
#include "toonzqt/paramfield.h"
#include <QApplication>
#include <QDir>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
TDoubleParamP radius(const TFxP &fx) {
  return TParamP(fx->getParams()->getParam("radius"));
}
void checkPage(const QString &layout) {
  TFxP actual = new CelColorExpandFx;
  radius(actual)->setDefaultValue(4.1234567890123);
  TFxP preview = actual->clone(false);
  ParamsPage page; TIStream stream(TFilePath(layout.toStdWString()));
  std::string tag;
  require(stream.matchTag(tag) && tag == "fxlayout", "Missing layout root");
  require(stream.matchTag(tag) && tag == "page", "Missing page");
  page.setPage(stream, actual, false); page.setFx(preview, actual, 0);
  auto controls = page.findChildren<MeasuredDoubleParamField *>();
  require(controls.size() == 1, "Search radius is missing or duplicated");
  require(radius(actual)->getValue(0) == 4.1234567890123,
          "Opening page changed radius precision");
  auto *field = controls.front();
  require(QMetaObject::invokeMethod(field, "onKeyToggled", Qt::DirectConnection),
          "Native key control unavailable");
  require(radius(actual)->isKeyframe(0), "Radius did not gain keyframe");
  TUndoManager::manager()->undo(); page.update(0);
  require(!radius(actual)->hasKeyframes() && radius(actual)->getValue(0) == 4.1234567890123,
          "Key undo changed radius");
  TUndoManager::manager()->redo(); page.update(0);
  require(radius(actual)->isKeyframe(0), "Radius key redo failed");
  radius(actual)->setValue(10, 12.5); page.update(10);
  require(radius(preview)->getValue(10) == 12.5, "Preview ignored animated radius");
  TUndoManager::manager()->reset();
}
}
int main(int argc, char **argv) {
  QApplication app(argc, argv); TThread::init(); int result = 1;
  try {
    require(argc == 2, "Expected effect layout");
    QTemporaryDir environment; require(environment.isValid(), "Cannot create environment");
    QString stuff = environment.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default") &&
                QDir().mkpath(stuff + "/config"), "Cannot create resource directories");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
    require(QDir().mkpath(ToonzFolder::getMyModuleDir().getQString()), "Cannot create settings");
    checkPage(QString::fromLocal8Bit(argv[1]));
    std::cout << "PASS: native radius control, keys, undo and preview\n"; result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  TThread::shutdown(); return result;
}
