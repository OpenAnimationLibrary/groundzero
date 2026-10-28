#include "../celboundaryblurfx.cpp"
#include "tenv.h"
#include "tparamcontainer.h"
#include "tstream.h"
#include "tthread.h"
#include "toonz/toonzfolders.h"
#include "toonzqt/fxsettings.h"
#include "toonzqt/paramfield.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
TDoubleParamP parameter(const TFxP &fx, const std::string &name) {
  return TParamP(fx->getParams()->getParam(name));
}
void pageTest(const QString &layout) {
  TFxP actual = new CelBoundaryBlurFx, preview = actual->clone(false);
  ParamsPage page;
  TIStream stream(TFilePath(layout.toStdWString()));
  std::string tag;
  require(stream.matchTag(tag) && tag == "fxlayout", "Layout root missing");
  require(stream.matchTag(tag) && tag == "page", "Layout page missing");
  page.setPage(stream, actual, false);
  page.setFx(preview, actual, 0);
  require(page.findChildren<MeasuredDoubleParamField *>().size() == 3,
          "A blur parameter has no native numeric editor");
  auto *field = page.findChild<AnimatedChoiceParamField *>();
  auto *combo = page.findChild<QComboBox *>("animatedChoice");
  require(field && combo && combo->count() == 2, "Bias choices are incomplete");
  require(parameter(actual, "bias")->getValue(0) == 0, "Opening UI changed bias");
  require(QMetaObject::invokeMethod(field, "onChoiceChanged", Qt::DirectConnection,
                                  Q_ARG(int, 1)), "Choice change unavailable");
  require(parameter(actual, "bias")->getValue(0) == 1, "Choice edited wrong parameter");
  TUndoManager::manager()->undo(); page.update(0);
  require(parameter(actual, "bias")->getValue(0) == 0, "Bias undo failed");
  TUndoManager::manager()->redo(); page.update(0);
  require(parameter(actual, "bias")->getValue(0) == 1, "Bias redo failed");
  require(QMetaObject::invokeMethod(field, "onKeyToggled", Qt::DirectConnection),
          "Bias key toggle unavailable");
  require(parameter(actual, "bias")->isKeyframe(0), "Bias cannot be animated");
  TUndoManager::manager()->undo(); page.update(0);
  require(!parameter(actual, "bias")->hasKeyframes(), "Bias key undo failed");
  for (auto *numeric : page.findChildren<MeasuredDoubleParamField *>()) {
    require(QMetaObject::invokeMethod(numeric, "onKeyToggled", Qt::DirectConnection),
            "Numeric key toggle unavailable");
  }
  for (const std::string name : {"amount", "smoothness", "repeats"})
    require(parameter(actual, name)->isKeyframe(0), "A numeric control cannot be animated");
  TUndoManager::manager()->reset();
}
}  // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  TThread::init();
  int result = 1;
  try {
    require(argc == 2, "Expected effect layout");
    QTemporaryDir environment;
    require(environment.isValid(), "Temporary directory unavailable");
    const QString stuff = environment.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default") &&
                QDir().mkpath(stuff + "/config"), "Cannot prepare widget resources");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
    require(QDir().mkpath(ToonzFolder::getMyModuleDir().getQString()),
            "Cannot prepare widget preferences");
    pageTest(QString::fromLocal8Bit(argv[1]));
    std::cout << "PASS: all native blur controls, bias choice, key insertion and undo\n";
    result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  TThread::shutdown();
  return result;
}
