#include "../celdirectionalblurfx.cpp"
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
TDoubleParamP param(const TFxP &fx, const std::string &name) {
  return TParamP(fx->getParams()->getParam(name));
}
void pageTest(const QString &layout) {
  TFxP actual = new CelDirectionalBlurFx, preview = actual->clone(false);
  ParamsPage page; TIStream stream(TFilePath(layout.toStdWString()));
  std::string tag;
  require(stream.matchTag(tag) && tag == "fxlayout", "Layout root missing");
  require(stream.matchTag(tag) && tag == "page", "Layout page missing");
  page.setPage(stream, actual, false); page.setFx(preview, actual, 0);
  const auto numbers = page.findChildren<MeasuredDoubleParamField *>();
  require(numbers.size() == 13, "Not all directional controls have native editors");
  auto *choice = page.findChild<AnimatedChoiceParamField *>();
  auto *combo = page.findChild<QComboBox *>("animatedChoice");
  require(choice && combo && combo->count() == 3, "Noise choices are incomplete");
  for (int mode : {1, 2, 0}) {
    require(QMetaObject::invokeMethod(choice, "onChoiceChanged", Qt::DirectConnection,
                                    Q_ARG(int, mode)), "Noise choice change unavailable");
    require(param(actual, "noiseType")->getValue(0) == mode, "Noise choice edited wrong parameter");
  }
  TUndoManager::manager()->undo(); page.update(0);
  require(param(actual, "noiseType")->getValue(0) == 2, "Choice undo failed");
  TUndoManager::manager()->redo(); page.update(0);
  require(param(actual, "noiseType")->getValue(0) == 0, "Choice redo failed");
  require(QMetaObject::invokeMethod(choice, "onKeyToggled", Qt::DirectConnection), "Choice key unavailable");
  for (auto *number : numbers)
    require(QMetaObject::invokeMethod(number, "onKeyToggled", Qt::DirectConnection), "Numeric key unavailable");
  for (const char *name : directionalNames)
    require(param(actual, name)->isKeyframe(0), "A directional control cannot be animated");
  TUndoManager::manager()->reset();
}
}  // namespace
int main(int argc, char **argv) {
  QApplication app(argc, argv); TThread::init(); int result = 1;
  try {
    require(argc == 2, "Expected effect layout");
    QTemporaryDir environment; require(environment.isValid(), "Temporary directory unavailable");
    const QString stuff = environment.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default") && QDir().mkpath(stuff + "/config"),
            "Cannot prepare widget resources");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
    require(QDir().mkpath(ToonzFolder::getMyModuleDir().getQString()), "Cannot prepare widget preferences");
    pageTest(QString::fromLocal8Bit(argv[1]));
    std::cout << "PASS: all directional controls, noise choice, key insertion and undo\n"; result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  TThread::shutdown(); return result;
}
