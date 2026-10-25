

#include "columncommand.h"
#include "toonzqt/menubarcommand.h"
#include "tmsgcore.h"
#include "tfilepath.h"
#include "tapp.h"
#include "toonz/tscenehandle.h"
#include "toonz/tproject.h"
#include "toonz/toonzscene.h"
#include "toonz/toonzfolders.h"
#include "filebrowserpopup.h"
#include "floatingpanelcommand.h"
#include "scriptconsolepanel.h"
#ifdef WITH_PYTHON
#include "pythonconsolepanel.h"
#endif
#include "tsystem.h"

#include <QFileDialog>

class RunScriptCommand final : public MenuItemHandler {
public:
  RunScriptCommand() : MenuItemHandler("MI_RunScript") {}
  void execute() override;
} runScriptCommand;

void RunScriptCommand::execute() {
  static GenericLoadFilePopup *popup = 0;
  if (popup == 0) {
    popup        = new GenericLoadFilePopup(QObject::tr("Run script"));
    TFilePath fp = ToonzFolder::getLibraryFolder() + "scripts";
    TFileStatus fpStatus(fp);
    if (!fpStatus.doesExist() || !fpStatus.isDirectory())
      fp = ToonzFolder::getLibraryFolder();

    QStringList types;
    types << "qs"
          << "js"
          << "txt"
          << "toonzscript";
    popup->setFilterTypes(types);

    popup->setFolder(fp);
  }
  TFilePath fp = popup->getPath();
  if (fp == TFilePath()) return;

  TPanel *panel = OpenFloatingPanel::getOrOpenFloatingPanel("ScriptConsole");
  if (!panel) return;

  ScriptConsolePanel *scriptConsolePanel =
      dynamic_cast<ScriptConsolePanel *>(panel);
  if (!scriptConsolePanel) return;

  QString s = QString::fromStdWString(fp.getWideString());
  s         = s.replace("\\", "\\\\").replace("\"", "\\\"");

  QString cmd = QString("run(\"%1\")").arg(s);

  scriptConsolePanel->executeCommand(cmd);

  /*
TFilePath projectFolder =
TApp::instance()->getCurrentScene()->getScene()->getProject()->getProjectFolder();
QString fileNameStr = QFileDialog::getSaveFileName(0,"","c:\\"); // ,
//   QObject::tr("Save Motion Path"),
QString::fromStdWString(projectFolder.getWideString()),
//   QObject::tr("Motion Path files (*.mpath)"));
if(fileNameStr == "") return;
TFilePath fp(fileNameStr.toStdWString());
if(fp.getType()=="") fp = fp.withType("mpath");
*/
}

#ifdef WITH_PYTHON
//=============================================================================
// RunPythonScriptCommand
//-----------------------------------------------------------------------------

class RunPythonScriptCommand final : public MenuItemHandler {
public:
  RunPythonScriptCommand() : MenuItemHandler("MI_RunPythonScript") {}
  void execute() override;
} runPythonScriptCommand;

void RunPythonScriptCommand::execute() {
  static GenericLoadFilePopup *popup = 0;
  if (popup == 0) {
    popup        = new GenericLoadFilePopup(QObject::tr("Run Python Script"));
    TFilePath fp = ToonzFolder::getLibraryFolder() + "scripts";
    TFileStatus fpStatus(fp);
    if (!fpStatus.doesExist() || !fpStatus.isDirectory())
      fp = ToonzFolder::getLibraryFolder();

    popup->setFilterTypes(QStringList() << "py");
    popup->setFolder(fp);
  }
  TFilePath fp = popup->getPath();
  if (fp == TFilePath()) return;

  // the output is printed in the Python Console
  TPanel *panel = OpenFloatingPanel::getOrOpenFloatingPanel("PythonConsole");
  PythonConsolePanel *pythonConsolePanel =
      dynamic_cast<PythonConsolePanel *>(panel);
  if (!pythonConsolePanel) return;

  pythonConsolePanel->runFile(QString::fromStdWString(fp.getWideString()));
}
#endif
