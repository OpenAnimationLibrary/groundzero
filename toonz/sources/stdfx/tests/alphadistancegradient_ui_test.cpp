#include "../alphadistancegradientfx.cpp"
#include "tenv.h"
#include "tdoublekeyframe.h"
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
#include <set>
#include <stdexcept>
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
TDoubleParamP number(const TFxP &fx, const std::string &name) {
  return TParamP(fx->getParams()->getParam(name));
}
void checkPage(const QString &layout) {
  TFxP actual = new AlphaDistanceGradientFx, preview = actual->clone(false);
  ParamsPage page; TIStream stream(TFilePath(layout.toStdWString())); std::string tag;
  require(stream.matchTag(tag) && tag == "fxlayout", "Missing layout root");
  require(stream.matchTag(tag) && tag == "page", "Missing page");
  page.setPage(stream, actual, false); page.setFx(preview, actual, 0);
  std::set<std::string> names;
  for (auto *field : page.findChildren<ParamField *>())
    names.insert(field->property("fxParamName").toString().toStdString());
  for (const char *name : {"region", "insideThreshold", "outsideThreshold", "renderMode",
       "useBackground", "gradationColor", "backgroundColor", "invert", "interpolation",
       "power", "blurMode", "blurSize"})
    require(names.count(name) == 1, "A required native control is missing");
  auto choices = page.findChildren<AnimatedChoiceParamField *>();
  require(choices.size() == 6, "A discrete control is not keyable");
  for (auto *choice : choices) {
    const std::string name = choice->property("fxParamName").toString().toStdString();
    const auto param = number(actual, name);
    const double old = param->getValue(0);
    const int last = int(param->getUIChoices().size()) - 1;
    require(last >= 1, "Choice list is incomplete");
    require(QMetaObject::invokeMethod(choice, "onChoiceChanged", Qt::DirectConnection,
                                     Q_ARG(int, last)), "Choice editing unavailable");
    require(param->getValue(0) == last, "Choice changed the wrong parameter");
    TUndoManager::manager()->undo(); page.update(0);
    require(param->getValue(0) == old, "Choice undo failed");
    TUndoManager::manager()->redo(); page.update(0);
    require(param->getValue(0) == last, "Choice redo failed");
    require(QMetaObject::invokeMethod(choice, "onKeyToggled", Qt::DirectConnection),
            "Choice key control unavailable");
    require(param->isKeyframe(0) && param->getKeyframeAt(0).m_type == TDoubleKeyframe::Constant,
            "Choice key did not use constant interpolation");
    TUndoManager::manager()->undo(); page.update(0);
    require(!param->hasKeyframes(), "Choice key undo failed");
    TUndoManager::manager()->reset();
  }
  auto colors = page.findChildren<PixelParamField *>();
  require(colors.size() == 2, "Color controls missing");
  for (auto *field : colors) {
    const std::string name = field->property("fxParamName").toString().toStdString();
    TPixelParamP parameter = TParamP(actual->getParams()->getParam(name));
    const TPixelD before = parameter->getValueD(0);
    field->setColor(TPixel32(37, 67, 131));
    require(parameter->getValue(0) == TPixel32(37, 67, 131), "Color edited wrong control");
    TUndoManager::manager()->undo(); page.update(0);
    require(parameter->getValueD(0) == before, "Color undo changed precision");
  }
  const auto measured = page.findChildren<MeasuredDoubleParamField *>();
  require(measured.size() == 4, "Threshold, power or blur size missing");
  for (auto *field : measured) {
    const std::string name = field->property("fxParamName").toString().toStdString();
    require(QMetaObject::invokeMethod(field, "onKeyToggled", Qt::DirectConnection),
            "Numeric key control unavailable");
    require(number(actual, name)->isKeyframe(0), "Numeric control did not gain key");
    TUndoManager::manager()->undo(); page.update(0);
    require(!number(actual, name)->hasKeyframes(), "Numeric key undo failed");
  }
  TUndoManager::manager()->reset();
}
}
int main(int argc, char **argv) {
  QApplication app(argc, argv); TThread::init(); int result = 1;
  try {
    require(argc == 2, "Expected effect layout"); QTemporaryDir environment;
    require(environment.isValid(), "Cannot create environment");
    QString stuff = environment.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default") &&
                QDir().mkpath(stuff + "/config"), "Cannot create resource directories");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
    require(QDir().mkpath(ToonzFolder::getMyModuleDir().getQString()), "Cannot create settings");
    checkPage(QString::fromLocal8Bit(argv[1]));
    std::cout << "PASS: all twelve controls, choices, colors, keys and undo\n"; result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  TThread::shutdown(); return result;
}
