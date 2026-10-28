#include "../celradialblurfx.cpp"
#include "tdoublekeyframe.h"
#include "tenv.h"
#include "tparamcontainer.h"
#include "tstream.h"
#include "tthread.h"
#include "toonz/toonzfolders.h"
#include "toonzqt/fxsettings.h"
#include "toonzqt/paramfield.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
ParamField *find(ParamsPage &page, const char *name) {
  for (ParamField *field : page.findChildren<ParamField *>())
    if (field->property("fxParamName").toString() == QString::fromLatin1(name)) return field;
  throw std::runtime_error("A radial control is absent from the page");
}
TDoubleParamP number(const TFxP &fx, const char *name) { return TParamP(fx->getParams()->getParam(name)); }
void exercise(const QString &layout) {
  TFxP actual = new CelRadialBlurFx;
  TFxP preview = actual->clone(false);
  ParamsPage page; TIStream stream(TFilePath(layout.toStdWString())); std::string tag;
  require(stream.matchTag(tag) && tag == "fxlayout", "Missing layout root");
  require(stream.matchTag(tag) && tag == "page", "Missing layout page");
  page.setPage(stream, actual, false); page.setFx(preview, actual, 0);
  for (const char *name : {"blurType", "center", "outerStrength", "outerOffsetMode", "outerOffset", "outerFade",
      "innerStrength", "innerOffsetMode", "innerOffset", "innerFade", "repeatBorder", "ellipseRatio", "ellipseAngle",
      "quality", "brightness", "sizeVariation", "noiseVariation", "noiseType", "seed", "noiseOffset", "thickness"})
    require(find(page, name) != nullptr, "Missing radial parameter editor");
  ParamField *field = find(page, "blurType");
  auto *combo = field->findChild<QComboBox *>("animatedChoice");
  require(combo && combo->count() == 2, "Blur type choices are unavailable");
  require(QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, 1)), "Choice signal unavailable");
  require(number(actual, "blurType")->getDefaultValue() == 1, "Rotation choice was not committed");
  TUndoManager::manager()->undo(); page.update(0);
  require(number(actual, "blurType")->getDefaultValue() == 0, "Choice undo failed");
  TUndoManager::manager()->redo(); page.update(0);
  require(number(actual, "blurType")->getDefaultValue() == 1, "Choice redo failed");
  require(QMetaObject::invokeMethod(field, "onKeyToggled", Qt::DirectConnection), "Choice key toggle unavailable");
  auto choice = number(actual, "blurType");
  require(choice->isKeyframe(0) && choice->getKeyframe(0).m_type == TDoubleKeyframe::Constant,
          "Mode key is not stepped");
  TUndoManager::manager()->undo(); page.update(0);
  require(!choice->hasKeyframes() && choice->getValue(0) == 1, "Choice key undo lost its default");
  auto *repeat = find(page, "repeatBorder")->findChild<QCheckBox *>("animatedCheckbox");
  require(repeat && repeat->isChecked(), "Repeat Border default is missing");
  repeat->click();
  require(number(actual, "repeatBorder")->getDefaultValue() == 0, "Repeat Border edit was not committed");
  TUndoManager::manager()->undo(); page.update(0);
  require(number(actual, "repeatBorder")->getDefaultValue() == 1, "Repeat Border undo failed");
  ParamsPage pinned; pinned.setPinnedPage(true);
  pinned.addExposedParam(0, actual, "noiseType");
  auto *pinnedChoice = pinned.findChild<AnimatedChoiceParamField *>();
  require(pinnedChoice && pinnedChoice->findChild<QComboBox *>("animatedChoice") &&
              pinnedChoice->findChild<QComboBox *>("animatedChoice")->count() == 3,
          "Pinned noise mode lost its choices");
  TUndoManager::manager()->reset();
}
}
int main(int argc, char **argv) {
  QApplication app(argc, argv); TThread::init(); int result = 1;
  try {
    require(argc == 2, "Expected the effect layout");
    QTemporaryDir environment; require(environment.isValid(), "Cannot create test environment");
    const QString stuff = environment.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default") && QDir().mkpath(stuff + "/config"), "Cannot create test resources");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
    require(QDir().mkpath(ToonzFolder::getMyModuleDir().getQString()), "Cannot create test settings");
    exercise(QString::fromLocal8Bit(argv[1]));
    std::cout << "PASS: radial native controls, undo and animated modes\n"; result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  TThread::shutdown(); return result;
}
