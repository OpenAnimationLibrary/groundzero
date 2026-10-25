

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
#include "toonz/txsheethandle.h"
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
      flipBook->setLevel(level->getSimpleLevel());
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
