#include "../starglowfx.cpp"
#include "tenv.h"
#include "tparamcontainer.h"
#include "tstream.h"
#include "tthread.h"
#include "toonz/toonzfolders.h"
#include "toonzqt/fxsettings.h"
#include "toonzqt/paramfield.h"
#include "toonzqt/spectrumfield.h"
#include <QApplication>
#include <QDir>
#include <QLineEdit>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
TSpectrumParamP ramp(const TFxP &fx) { return TParamP(fx->getParams()->getParam("verticalRamp")); }
void check(const QString &layout) {
  TFxP actual = new StarGlowFx;
  ramp(actual)->getColor(0)->getRed()->setDefaultValue(0.123456789);
  TFxP preview = actual->clone(false);
  ParamsPage page;
  TIStream stream(TFilePath(layout.toStdWString())); std::string tag;
  require(stream.matchTag(tag) && tag == "fxlayout", "Missing layout");
  require(stream.matchTag(tag) && tag == "page", "Missing page");
  page.setPage(stream, actual, false); page.setFx(preview, actual, 0);
  SpectrumParamField *field = nullptr;
  for (auto *candidate : page.findChildren<SpectrumParamField *>())
    if (candidate->property("fxParamName").toString() == "verticalRamp") field = candidate;
  require(field && field->property("preserveColorPrecision").toBool(), "Ramp editor lacks precision mode");
  require(ramp(preview)->getColor(0)->getValueD(0).r == 0.123456789, "Opening editor quantized preview");
  auto *preciseRed = field->findChild<QLineEdit *>("spectrumPreciseRed");
  require(preciseRed, "Missing precise ramp input");
  preciseRed->setText("1.23456789123");
  require(QMetaObject::invokeMethod(preciseRed, "editingFinished", Qt::DirectConnection), "Cannot edit precise ramp value");
  require(ramp(actual)->getColor(0)->getValueD(0).r == 1.23456789123,
          "Precise HDR ramp value was not stored");
  TUndoManager::manager()->undo(); page.update(0);
  require(ramp(actual)->getColor(0)->getValueD(0).r == 0.123456789,
          "Precise ramp undo did not restore the original value");
  TUndoManager::manager()->redo(); page.update(0);
  require(ramp(actual)->getColor(0)->getValueD(0).r == 1.23456789123,
          "Precise ramp redo did not restore the HDR value");
  TUndoManager::manager()->undo(); page.update(0); TUndoManager::manager()->reset();
  preciseRed->setText("1e300");
  require(QMetaObject::invokeMethod(preciseRed, "editingFinished", Qt::DirectConnection), "Cannot edit extreme HDR ramp value");
  page.update(0);
  require(ramp(actual)->getColor(0)->getValueD(0).r == 1e300, "Extreme HDR value was not retained by the editor");
  TUndoManager::manager()->undo(); page.update(0); TUndoManager::manager()->reset();
  preciseRed->setText("not a number");
  require(QMetaObject::invokeMethod(preciseRed, "editingFinished", Qt::DirectConnection), "Cannot validate precise input");
  require(ramp(actual)->getColor(0)->getValueD(0).r == 0.123456789,
          "Invalid precise input changed the ramp");
  auto *editor = field->findChild<DVGui::SpectrumField *>(); require(editor, "Missing ramp widget");
  TSpectrum changed = editor->getSpectrum(); changed.setKey(1, {1.0, TPixel32(42, 65, 127)}); editor->setSpectrum(changed);
  require(QMetaObject::invokeMethod(field, "onChange", Qt::DirectConnection, Q_ARG(bool, false)), "Cannot edit ramp");
  require(ramp(actual)->getColor(1)->getValue(0) == TPixel32(42, 65, 127), "Wrong ramp value");
  require(ramp(actual)->getColor(0)->getValueD(0).r == 0.123456789, "Editing another stop quantized a color");
  TUndoManager::manager()->undo(); page.update(0);
  require(ramp(actual)->getColor(0)->getValueD(0).r == 0.123456789 && ramp(actual)->getColor(1)->getValue(0) == TPixel32::White, "Ramp undo lost precision");
  TUndoManager::manager()->redo(); page.update(0);
  require(ramp(actual)->getColor(0)->getValueD(0).r == 0.123456789 && ramp(actual)->getColor(1)->getValue(0) == TPixel32(42, 65, 127), "Ramp redo lost values");
  require(QMetaObject::invokeMethod(field, "onKeyToggled", Qt::DirectConnection), "Cannot key ramp");
  require(ramp(actual)->isKeyframe(0) && ramp(actual)->getColor(0)->getValueD(0).r == 0.123456789, "Key insertion quantized ramp");
  TUndoManager::manager()->undo(); page.update(0);
  require(!ramp(actual)->hasKeyframes() && ramp(actual)->getColor(0)->getValueD(0).r == 0.123456789, "Key undo lost precision");
  TSpectrum added = editor->getSpectrum(); added.addKey({0.5, TPixel32::Red}); editor->setSpectrum(added);
  require(QMetaObject::invokeMethod(field, "onKeyAdded", Qt::DirectConnection, Q_ARG(int, 2)), "Cannot add ramp stop");
  require(ramp(actual)->getKeyCount() == 3, "Stop was not added");
  TUndoManager::manager()->undo(); page.update(0);
  require(ramp(actual)->getKeyCount() == 2 && ramp(actual)->getColor(0)->getValueD(0).r == 0.123456789, "Stop undo lost precision");
  TUndoManager::manager()->redo(); page.update(0);
  require(ramp(actual)->getKeyCount() == 3 && ramp(actual)->getColor(0)->getValueD(0).r == 0.123456789, "Stop redo lost precision");
  ramp(actual)->getColor(2)->getRed()->setDefaultValue(0.234567891);
  ramp(preview)->getColor(2)->getRed()->setDefaultValue(0.234567891);
  TSpectrum removed = editor->getSpectrum(); removed.removeKey(2); editor->setSpectrum(removed);
  require(QMetaObject::invokeMethod(field, "onKeyRemoved", Qt::DirectConnection, Q_ARG(int, 2)), "Cannot remove ramp stop");
  require(ramp(actual)->getKeyCount() == 2, "Stop was not removed");
  TUndoManager::manager()->undo(); page.update(0);
  require(ramp(actual)->getKeyCount() == 3 && ramp(actual)->getColor(2)->getValueD(0).r == 0.234567891, "Removing a stop lost its precise color");
  TUndoManager::manager()->reset();
  ParamsPage samePage;
  TIStream sameStream(TFilePath(layout.toStdWString()));
  require(sameStream.matchTag(tag) && tag == "fxlayout", "Missing same-parameter layout");
  require(sameStream.matchTag(tag) && tag == "page", "Missing same-parameter page");
  samePage.setPage(sameStream, actual, false); samePage.setFx(actual, actual, 0);
  SpectrumParamField *sameField = nullptr;
  for (auto *candidate : samePage.findChildren<SpectrumParamField *>())
    if (candidate->property("fxParamName").toString() == "verticalRamp") sameField = candidate;
  require(sameField && ramp(actual)->getKeyCount() == 3, "Shared preview erased ramp stops");
  require(QMetaObject::invokeMethod(sameField, "onKeyToggled", Qt::DirectConnection), "Cannot key shared ramp");
  require(ramp(actual)->getKeyCount() == 3 && ramp(actual)->getColor(2)->getValueD(0).r == 0.234567891, "Shared keying lost ramp values");
  auto *sameEditor = sameField->findChild<DVGui::SpectrumField *>(); require(sameEditor, "Missing shared ramp widget");
  TSpectrum sameAdded = sameEditor->getSpectrum(); sameAdded.addKey({0.25, TPixel32::Blue}); sameEditor->setSpectrum(sameAdded);
  require(QMetaObject::invokeMethod(sameField, "onKeyAdded", Qt::DirectConnection, Q_ARG(int, 3)), "Cannot add shared stop");
  require(ramp(actual)->getKeyCount() == 4, "Shared ramp added a stop twice");
  TUndoManager::manager()->reset();
}
}
int main(int argc, char **argv) {
  QApplication app(argc, argv); TThread::init(); int result = 1;
  try {
    require(argc == 2, "Expected layout path"); QTemporaryDir temp; require(temp.isValid(), "No temporary directory");
    const QString stuff = temp.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default") && QDir().mkpath(stuff + "/config"), "Cannot create resources");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString())); require(QDir().mkpath(ToonzFolder::getMyModuleDir().getQString()), "Cannot create settings");
    check(QString::fromLocal8Bit(argv[1])); result = 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; }
  catch (const TException &e) { std::cerr << QString::fromStdWString(e.getMessage()).toStdString() << '\n'; }
  TThread::shutdown(); return result;
}
