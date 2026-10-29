

#include "scriptconsolepanel.h"
#include "toonzqt/scriptconsole.h"
#include "toonz/scriptengine.h"
#include "toonz/scriptbinding.h"
#include "toonz/scriptbinding_level.h"
#include "iocommand.h"
#include "tapp.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/txshlevel.h"
#include "toonz/txshsimplelevel.h"
#include "toonzqt/selection.h"
#include "toonzqt/tselectionhandle.h"

#include "flipbook.h"
#include "tvectorimage.h"

#include <QJSEngine>
#include <QJSValue>
#include <QFile>
#include <QTextStream>

#include <climits>
#include <cmath>

static QJSValue loadSceneFun(QJSEngine *engine, const QJSValueList &args) {
  if (args.size() > 0) {
    QString fpArg = args[0].toString();
    TFilePath fp(fpArg.toStdWString());
    IoCmd::loadScene(fp);
  }
  return QJSValue();
}

static QJSValue saveSceneFun(QJSEngine *engine, const QJSValueList &args) {
  if (args.size() > 0) {
    QString fpArg = args[0].toString();
    TFilePath fp(fpArg.toStdWString());
    IoCmd::saveScene(fp, IoCmd::SILENTLY_OVERWRITE);
  }
  return QJSValue();
}

static QJSValue loadLevelFun(QJSEngine *engine, const QJSValueList &args) {
  if (args.size() > 0) {
    QString fpArg = args[0].toString();
    TFilePath fp(fpArg.toStdWString());
    int row = 0, col = 0;
    if (args.size() == 3) {
      row = args[1].toInt();
      col = args[1].toInt();
    }

    TApp *app         = TApp::instance();
    ToonzScene *scene = app->getCurrentScene()->getScene();
    TXsheet *xsh      = scene->getXsheet();

    TFilePath actualPath = scene->decodeFilePath(fp);
    TXshLevel *xl        = scene->loadLevel(actualPath);
    if (xl) {
      scene->getXsheet()->exposeLevel(row, col, xl);
    }
    app->getCurrentScene()->notifyCastChange();
    app->getCurrentScene()->notifySceneChanged();
    app->getCurrentXsheet()->notifyXsheetChanged();
  }
  return QJSValue();
}

QVariantMap getCurrentSceneInfo() {
  TApp *app         = TApp::instance();
  ToonzScene *scene = app->getCurrentScene()->getScene();
  QVariantMap info;
  if (!scene) return info;
  info["name"] = QString::fromStdWString(scene->getSceneName());
  info["path"] = QString::fromStdWString(
      scene->decodeFilePath(scene->getScenePath()).getWideString());
  info["untitled"]    = scene->isUntitled();
  info["dirty"]       = app->getCurrentScene()->getDirtyFlag();
  info["frameCount"]  = scene->getFrameCount();
  info["columnCount"] = scene->getXsheet()->getColumnCount();
  // 0-based, as the frames of the scripts
  info["currentFrame"]  = app->getCurrentFrame()->getFrame();
  info["currentColumn"] = app->getCurrentColumn()->getColumnIndex();
  return info;
}

QString setCurrentSceneFrame(int frame) {
  if (frame < 0)
    return QObject::tr("Bad frame (%1): expected an integer >= 0").arg(frame);
  TApp::instance()->getCurrentFrame()->setFrame(frame);
  return "";
}

QString setCurrentSceneColumn(int column) {
  TApp *app    = TApp::instance();
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  int count    = xsh ? xsh->getColumnCount() : 0;
  if (column < 0 || column >= count)
    return QObject::tr("Bad column (%1): the xsheet has %2 columns")
        .arg(column)
        .arg(count);
  app->getCurrentColumn()->setColumnIndex(column);
  return "";
}

// A non negative integer argument of a script function
static bool getIndexArgument(const QJSValueList &args, int &value) {
  if (args.size() != 1 || !args[0].isNumber()) return false;
  double d = args[0].toNumber();
  if (!std::isfinite(d) || d != std::floor(d) || d < 0 || d > INT_MAX)
    return false;
  value = (int)d;
  return true;
}

static QJSValue dummyFun(QJSEngine *engine, const QJSValueList &args) {
  return QJSValue(0);
}

static QJSValue viewFun(ScriptEngine *teng, QJSEngine *engine,
                        const QJSValueList &args) {
  TScriptBinding::Image *image = 0;
  TScriptBinding::Level *level = 0;

  if (args.size() == 1) {
    image = TScriptBinding::cast<TScriptBinding::Image>(args[0]);
    level = TScriptBinding::cast<TScriptBinding::Level>(args[0]);
  }
  QString err;
  if (image) {
    if (!image->getImg()) err = "Can't view an empty image";
  } else if (level) {
    if (!level->getSimpleLevel()) err = "Can't view an empty level";
  } else {
    err = "expected one argument : an image or a level";
  }
  if (!err.isEmpty()) {
    engine->throwError(err);
    return QJSValue();
  }

  // the flipbook must be used by the main thread
  teng->runOnMainThread([image, level]() {
    FlipBook *flipBook;
    flipBook = FlipBookPool::instance()->pop();
    if (image) {
      ImageViewer *imageViewer = flipBook->getImageViewer();
      imageViewer->setImage(image->getImg());
    } else {
      // Show a detached copy: the script can keep editing the level, or drop
      // it together with its scene, while the flipbook is still open.
      TXshSimpleLevel *src  = level->getSimpleLevel();
      TXshSimpleLevelP copy = new TXshSimpleLevel(src->getName());
      copy->setScene(src->getScene());
      copy->setType(src->getType());
      copy->clonePropertiesFrom(src);
      if (TPalette *palette = src->getPalette())
        copy->setPalette(palette->clone());
      std::vector<TFrameId> fids;
      src->getFids(fids);
      for (const TFrameId &fid : fids)
        if (TImageP img = src->getFrame(fid, false))
          copy->setFrame(fid, img->cloneImage());
      copy->setScene(nullptr);
      flipBook->setLevel(copy.getPointer());
    }
  });
  return teng->voidValue();
}

// Defines a script function that runs on the main thread (while the script
// waits for it). fun must not use the engine.
static void def(ScriptEngine *teng, const QString &name,
                QJSValue (*fun)(QJSEngine *, const QJSValueList &)) {
  teng->defineFunction(
      name, [teng, fun](QJSEngine *engine, const QJSValueList &args) {
        QJSValue result;
        teng->runOnMainThread([&]() { result = fun(engine, args); });
        return result;
      });
}

ScriptConsolePanel::ScriptConsolePanel(QWidget *parent, Qt::WindowFlags flags)
    : TPanel(parent) {
  setPanelType("ScriptConsole");
  setIsMaximizable(false);
  setWindowTitle(QObject::tr("Script Console"));

  m_scriptConsole = new ScriptConsole(this);

  ScriptEngine *teng = m_scriptConsole->getEngine();

  /*
def(teng, "saveScene", saveSceneFun);
def(teng, "loadScene", loadSceneFun);
def(teng, "loadLevel", loadLevelFun);
*/

  teng->defineFunction("view",
                       [teng](QJSEngine *engine, const QJSValueList &args) {
                         return viewFun(teng, engine, args);
                       });
  def(teng, "dummy", dummyFun);

  // The scene open in the UI (see getCurrentSceneInfo()). The UI is used
  // by the main thread, while the script waits.
  teng->defineFunction(
      "currentSceneInfo",
      [teng](QJSEngine *engine, const QJSValueList &args) -> QJSValue {
        if (!args.isEmpty()) {
          engine->throwError(
              QObject::tr("currentSceneInfo() takes no arguments"));
          return QJSValue();
        }
        QVariantMap info;
        teng->runOnMainThread([&info]() { info = getCurrentSceneInfo(); });
        return engine->toScriptValue(info);
      });
  teng->defineFunction(
      "setCurrentFrame",
      [teng](QJSEngine *engine, const QJSValueList &args) -> QJSValue {
        int frame = 0;
        if (!getIndexArgument(args, frame)) {
          engine->throwError(
              QObject::tr("setCurrentFrame() expects a frame (an integer >= "
                          "0, the first frame is 0)"));
          return QJSValue();
        }
        QString err;
        teng->runOnMainThread(
            [&err, frame]() { err = setCurrentSceneFrame(frame); });
        if (!err.isEmpty()) {
          engine->throwError(err);
          return QJSValue();
        }
        return teng->voidValue();
      });
  teng->defineFunction(
      "setCurrentColumn",
      [teng](QJSEngine *engine, const QJSValueList &args) -> QJSValue {
        int column = 0;
        if (!getIndexArgument(args, column)) {
          engine->throwError(
              QObject::tr("setCurrentColumn() expects a column (an integer "
                          ">= 0, the first column is 0)"));
          return QJSValue();
        }
        QString err;
        teng->runOnMainThread(
            [&err, column]() { err = setCurrentSceneColumn(column); });
        if (!err.isEmpty()) {
          engine->throwError(err);
          return QJSValue();
        }
        return teng->voidValue();
      });

  // teng->getQScriptEngine()->evaluate("console={version:'1.0'};function
  // version() {print('Toonz '+toonz.version+'\nscript '+script.version);};");

  /*
QFile initFile(":/Resources/init.js");
if (initFile.open(QIODevice::ReadOnly))
{
QTextStream stream(&initFile);
QString contents = stream.readAll();
initFile.close();
teng->getQScriptEngine()->evaluate(contents, "init.js");
}
*/

  setWidget(m_scriptConsole);
  setMinimumHeight(80);
  allowMultipleInstances(false);

  resize(800, 300);
  connect(m_scriptConsole, SIGNAL(selectionChanged()), this,
          SLOT(selectNone()));
}

//-----------------------------------------------------------------------------

ScriptConsolePanel::~ScriptConsolePanel() {}

//-----------------------------------------------------------------------------

void ScriptConsolePanel::executeCommand(const QString &cmd) {
  m_scriptConsole->executeCommand(cmd);
}

//-----------------------------------------------------------------------------

void ScriptConsolePanel::selectNone() {
  TApp::instance()->getCurrentSelection()->setSelection(0);
}
