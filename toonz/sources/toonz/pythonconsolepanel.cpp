#include "toonzqt/uitranslations.h"
#include "pythonconsolepanel.h"

// Tnz6 includes
#include "tapp.h"
#include "scriptconsolepanel.h"

// TnzLib includes
#include "toonz/pythonengine.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheet.h"

// TnzQt includes
#include "toonzqt/selection.h"
#include "toonzqt/tselectionhandle.h"

// Qt includes
#include <QKeyEvent>
#include <QMimeData>
#include <QRegularExpression>
#include <QStringList>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QUrl>
#include <QVariantMap>

#include <climits>
#include <cmath>
#include <stdexcept>
#include <string>

namespace {

const QString mainPrompt         = ">>> ";
const QString continuationPrompt = "... ";

QTextCharFormat textFormat(const QColor &color) {
  QTextCharFormat format;
  format.setForeground(color);
  return format;
}

// opentoonz.currentSceneInfo(): the scene open in the UI (read only)
QVariant currentSceneInfo(const QVariantList &args) {
  if (!args.isEmpty())
    throw std::invalid_argument("currentSceneInfo() takes no arguments");
  return getCurrentSceneInfo();
}

// An integer >= 0 (not a bool or a str) argument
int getIndexArgument(const QVariantList &args, const char *name,
                     const char *what) {
  if (args.size() == 1) {
    int type      = args[0].userType();
    bool isNumber = type == QMetaType::Int || type == QMetaType::LongLong ||
                    type == QMetaType::UInt || type == QMetaType::ULongLong ||
                    type == QMetaType::Double;
    double value = isNumber ? args[0].toDouble() : -1;
    if (isNumber && value >= 0 && value == std::floor(value) &&
        value <= INT_MAX)
      return (int)value;
  }
  throw std::invalid_argument(std::string(name) + "() expects " + what +
                              " (an integer >= 0, the first one is 0)");
}

// opentoonz.setCurrentFrame(frame) and setCurrentColumn(column): make the
// frame or the column current in the UI
QVariant setCurrentFrame(const QVariantList &args) {
  QString err = setCurrentSceneFrame(
      getIndexArgument(args, "setCurrentFrame", "a frame"));
  if (!err.isEmpty()) throw std::invalid_argument(err.toStdString());
  return QVariant();
}

QVariant setCurrentColumn(const QVariantList &args) {
  QString err = setCurrentSceneColumn(
      getIndexArgument(args, "setCurrentColumn", "a column"));
  if (!err.isEmpty()) throw std::invalid_argument(err.toStdString());
  return QVariant();
}

// The functions of the opentoonz module that need the application
void defineAppFunctions() {
  static bool defined = false;
  if (defined) return;
  defined = true;
  PythonEngine::instance()->defineFunction(
      "currentSceneInfo", currentSceneInfo,
      "currentSceneInfo() -> dict\n\nThe scene open in the application "
      "(read only): name, path, untitled, dirty, frameCount, columnCount "
      "(of the current xsheet), currentFrame and currentColumn (from 0).");
  PythonEngine::instance()->defineFunction(
      "setCurrentFrame", setCurrentFrame,
      "setCurrentFrame(frame)\n\nMakes the frame (from 0) current in the "
      "application.");
  PythonEngine::instance()->defineFunction(
      "setCurrentColumn", setCurrentColumn,
      "setCurrentColumn(column)\n\nMakes the column (from 0) current in the "
      "application.");
}

}  // namespace

//=============================================================================
// PythonConsole
//-----------------------------------------------------------------------------

PythonConsole::PythonConsole(QWidget *parent)
    : QTextEdit(parent), m_commandIndex(0), m_prompt(mainPrompt) {
  // the style of the Script Console
  setObjectName("ScriptConsole");
  setUndoRedoEnabled(false);

  defineAppFunctions();

  // the interpreter is started by the first line
  QTextCursor cursor(document());
  cursor.insertText(
      tr("Python Console: the module opentoonz is imported (see "
         "help(opentoonz)). Scripts block the application while they run."),
      textFormat(QColor(120, 120, 120)));
  showPrompt();

  connect(PythonEngine::instance(), SIGNAL(output(int, const QString &)), this,
          SLOT(onOutput(int, const QString &)));
  connect(this, SIGNAL(cursorPositionChanged()), this,
          SLOT(onCursorPositionChanged()));
}

//-----------------------------------------------------------------------------

PythonConsole::~PythonConsole() {}

//-----------------------------------------------------------------------------

QString PythonConsole::getCurrentLine() const {
  QTextBlock block = document()->lastBlock();
  return block.text().mid(m_prompt.length());
}

//-----------------------------------------------------------------------------

void PythonConsole::setCurrentLine(const QString &line) {
  QTextCursor cursor(document());
  cursor.movePosition(QTextCursor::End);
  cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
  cursor.insertText(m_prompt + line, textFormat(Qt::black));
  setTextCursor(cursor);
}

//-----------------------------------------------------------------------------

void PythonConsole::showPrompt() {
  QTextCursor cursor(document());
  cursor.movePosition(QTextCursor::End);
  if (!cursor.block().text().isEmpty()) cursor.insertBlock();
  cursor.insertText(m_prompt, textFormat(Qt::black));
  setTextCursor(cursor);
  ensureCursorVisible();
}

//-----------------------------------------------------------------------------

void PythonConsole::keyPressEvent(QKeyEvent *e) {
  switch (e->key()) {
  case Qt::Key_Return:
  case Qt::Key_Enter:
    onReturnKeyPress();
    break;
  case Qt::Key_Up:
    if (m_commandIndex > 0) {
      // the first KeyUp saves the current command, possibly not completed yet
      if (m_commandIndex == m_commands.count())
        m_currentCommand = getCurrentLine();
      setCurrentLine(m_commands[--m_commandIndex]);
    }
    break;
  case Qt::Key_Down:
    if (m_commandIndex < m_commands.count()) {
      if (m_commandIndex == m_commands.count() - 1) {
        // the last KeyDown inserts back the current command
        setCurrentLine(m_currentCommand);
        ++m_commandIndex;
      } else
        setCurrentLine(m_commands[++m_commandIndex]);
    }
    break;
  case Qt::Key_Home:
    if (!textCursor().block().next().isValid()) {
      // after the prompt
      QTextCursor::MoveMode mode = (e->modifiers() & Qt::ShiftModifier)
                                       ? QTextCursor::KeepAnchor
                                       : QTextCursor::MoveAnchor;
      QTextCursor cursor         = textCursor();
      cursor.movePosition(QTextCursor::StartOfBlock, mode);
      cursor.movePosition(QTextCursor::Right, mode, m_prompt.length());
      setTextCursor(cursor);
    } else
      QTextEdit::keyPressEvent(e);
    break;
  case Qt::Key_Backspace:
  case Qt::Key_Left:
    if (textCursor().positionInBlock() > m_prompt.length())
      QTextEdit::keyPressEvent(e);
    else
      e->ignore();
    break;
  default:
    QTextEdit::keyPressEvent(e);
  }
}

//-----------------------------------------------------------------------------

void PythonConsole::onCursorPositionChanged() {
  // only the last block (i.e. the current command) is editable
  setReadOnly(textCursor().block().next().isValid());
}

//-----------------------------------------------------------------------------

bool PythonConsole::canInsertFromMimeData(const QMimeData *source) const {
  return source->hasText() ||
         (source->hasUrls() && source->urls().length() == 1);
}

//-----------------------------------------------------------------------------

void PythonConsole::insertFromMimeData(const QMimeData *source) {
  if (source->hasUrls() && source->urls().length() == 1 &&
      source->urls()[0].isLocalFile()) {
    // a dropped file: its path, as a Python string
    QString text = source->urls()[0].toLocalFile();
    text = "\"" + text.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
    textCursor().insertText(text, textFormat(Qt::black));
  } else if (source->hasText()) {
    QStringList lines = source->text().split(QRegularExpression("\r\n|\r|\n"));
    if (lines.size() == 1) {
      textCursor().insertText(lines[0], textFormat(Qt::black));
      return;
    }
    // the lines of a multi-line text run one after the other (the last one
    // is left to complete)
    for (int i = 0; i < lines.size(); ++i) {
      moveCursor(QTextCursor::End);
      textCursor().insertText(lines[i], textFormat(Qt::black));
      if (i < lines.size() - 1) onReturnKeyPress();
    }
  } else if (source->hasUrls() && source->urls().length() == 1) {
    textCursor().insertText(source->urls()[0].toString(),
                            textFormat(Qt::black));
  }
}

//-----------------------------------------------------------------------------

void PythonConsole::onReturnKeyPress() {
  QString line = getCurrentLine();

  // the executed line is gray
  QTextCursor cursor(document());
  cursor.movePosition(QTextCursor::End);
  cursor.movePosition(QTextCursor::StartOfBlock);
  cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor,
                      m_prompt.length());
  cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
  cursor.mergeCharFormat(textFormat(QColor(120, 120, 120)));

  if (line.trimmed() != "") {
    m_commands.removeAll(line);
    m_commands.append(line);
  }
  m_commandIndex = m_commands.count();

  cursor.movePosition(QTextCursor::End);
  cursor.insertBlock();
  setTextCursor(cursor);

  bool more = PythonEngine::instance()->push(line);
  m_prompt  = more ? continuationPrompt : mainPrompt;
  showPrompt();
}

//-----------------------------------------------------------------------------

void PythonConsole::executeLine(const QString &line) {
  setCurrentLine(line);
  onReturnKeyPress();
}

//-----------------------------------------------------------------------------

void PythonConsole::runFile(const QString &path) {
  // discards what is being typed, and an incomplete statement
  if (m_prompt != mainPrompt) {
    PythonEngine::instance()->resetInput();
    m_prompt = mainPrompt;
  }
  QTextCursor cursor(document());
  cursor.movePosition(QTextCursor::End);
  cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
  cursor.insertText(m_prompt, textFormat(Qt::black));
  cursor.insertText(UiTranslations::text(tr("# Run %1")).arg(path),
                    textFormat(QColor(120, 120, 120)));
  cursor.insertBlock();
  setTextCursor(cursor);

  PythonEngine::instance()->runFile(path);
  showPrompt();
}

//-----------------------------------------------------------------------------

void PythonConsole::onOutput(int type, const QString &text) {
  QColor color =
      type == PythonEngine::StdErr ? QColor(Qt::red) : QColor(10, 150, 240);
  QTextCursor cursor(document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(text, textFormat(color));
  setTextCursor(cursor);
  ensureCursorVisible();
}

//=============================================================================
// PythonConsolePanel
//-----------------------------------------------------------------------------

PythonConsolePanel::PythonConsolePanel(QWidget *parent, Qt::WindowFlags flags)
    : TPanel(parent) {
  setPanelType("PythonConsole");
  setIsMaximizable(false);
  setWindowTitle(QObject::tr("Python Console"));

  m_console = new PythonConsole(this);

  setWidget(m_console);
  setMinimumHeight(80);
  allowMultipleInstances(false);

  resize(800, 300);
  connect(m_console, SIGNAL(selectionChanged()), this, SLOT(selectNone()));
}

//-----------------------------------------------------------------------------

PythonConsolePanel::~PythonConsolePanel() {}

//-----------------------------------------------------------------------------

void PythonConsolePanel::executeLine(const QString &line) {
  m_console->executeLine(line);
}

//-----------------------------------------------------------------------------

void PythonConsolePanel::runFile(const QString &path) {
  m_console->runFile(path);
}

//-----------------------------------------------------------------------------

void PythonConsolePanel::selectNone() {
  TApp::instance()->getCurrentSelection()->setSelection(0);
}
