#include "../seexprfx.cpp"
#include "tenv.h"
#include "tthread.h"
#include "toonz/toonzfolders.h"
#include "toonzqt/fxsettings.h"
#include "toonzqt/paramfield.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
void apply(const TFxP &fx, const QJsonArray &controls) {
  auto *native = dynamic_cast<TFxSeExprSource *>(fx.getPointer());
  require(native, "native expression interface is missing");
  const QJsonObject object{{"version", 1},
                           {"rgb", "[motion(1),0,0]"},
                           {"alpha", "1"},
                           {"controls", controls}};
  native->seExprDocumentParam()->setValue(
      QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact))
          .toStdWString());
}
// Fields of the page set the viewer shows; replaced page sets are only
// scheduled for deletion.
bool hasField(ParamViewer &viewer, const std::string &name) {
  auto *pageSets =
      viewer.findChild<QStackedWidget *>(QString(), Qt::FindDirectChildrenOnly);
  require(pageSets && pageSets->currentWidget(),
          "parameter viewer has no current page set");
  for (auto *field : pageSets->currentWidget()->findChildren<ParamField *>())
    if (field->property("fxParamName").toString().toStdString() == name)
      return true;
  return false;
}
// Applying a declaration that adds or removes an animation key changes the
// native fields but not the control's ID, label or type. The cached page was
// keyed by those alone, so the new key's fields never appeared.
void checkAnimationKeyFields() {
  TFxP actual = new SeExprGeneratorFx;
  const QJsonObject left{{"id", "left"}, {"time", 0}, {"value", 0}};
  const QJsonObject middle{{"id", "middle"}, {"time", 1}, {"value", .5}};
  const QJsonObject right{{"id", "right"}, {"time", 2}, {"value", 1}};
  QJsonObject motion{{"id", "motion"},
                     {"name", "motion"},
                     {"type", "animcurve"},
                     {"keys", QJsonArray{left, right}}};
  apply(actual, {motion});
  ParamViewer viewer;
  auto show = [&] { viewer.setFx(actual->clone(false), actual, 0, nullptr); };
  show();
  require(hasField(viewer, "seexprControl_motion__kright_value"),
          "an animation key has no field");
  require(!hasField(viewer, "seexprControl_motion__kmiddle_value"),
          "an absent animation key has a field");
  motion["keys"] = QJsonArray{left, middle, right};
  apply(actual, {motion});
  show();
  require(hasField(viewer, "seexprControl_motion__kmiddle_value"),
          "an added animation key has no field after Apply");
  motion["keys"] = QJsonArray{left, right};
  apply(actual, {motion});
  show();
  require(!hasField(viewer, "seexprControl_motion__kmiddle_value") &&
              hasField(viewer, "seexprControl_motion__kright_value"),
          "a removed animation key kept its field after Apply");
}
}  // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  TThread::init();
  int result = 1;
  try {
    require(argc == 2, "Expected the Expression Generator layout");
    QTemporaryDir environment;
    require(environment.isValid(), "Cannot create environment");
    const QString stuff   = environment.filePath("stuff");
    const QString layouts = stuff + "/profiles/layouts/fxs";
    require(QDir().mkpath(layouts) &&
                QDir().mkpath(stuff + "/profiles/layouts/rooms/Default") &&
                QDir().mkpath(stuff + "/config"),
            "Cannot create resource directories");
    require(QFile::copy(QString::fromLocal8Bit(argv[1]),
                        layouts + "/STD_seexprGeneratorFx.xml"),
            "Cannot install the Expression Generator layout");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
    require(QDir().mkpath(ToonzFolder::getMyModuleDir().getQString()),
            "Cannot create settings");
    checkAnimationKeyFields();
    std::cout << "seexpr-fx-ui: PASS\n";
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
