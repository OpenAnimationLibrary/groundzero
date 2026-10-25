#pragma once

#ifndef SCRIPTCONSOLEPANEL_H
#define SCRIPTCONSOLEPANEL_H

#include "pane.h"

#include <QString>
#include <QVariantMap>

class ScriptConsole;

// The scene open in the UI, as the currentSceneInfo() function of the script
// consoles (JavaScript and Python) returns it. Read only: name, path,
// untitled, dirty, frameCount, columnCount (of the current xsheet),
// currentFrame and currentColumn (from 0). Empty if there is no scene. To be
// called from the GUI thread.
QVariantMap getCurrentSceneInfo();

// Make the frame (row, from 0) or the column (from 0) current in the UI, as
// the setCurrentFrame() and setCurrentColumn() functions of the script
// consoles do. Return the message of the error, or "" if there is none. To
// be called from the GUI thread.
QString setCurrentSceneFrame(int frame);
QString setCurrentSceneColumn(int column);

class ScriptConsolePanel final : public TPanel {
  Q_OBJECT
  ScriptConsole *m_scriptConsole;

public:
  ScriptConsolePanel(QWidget *parent       = 0,
                     Qt::WindowFlags flags = Qt::WindowFlags());
  ~ScriptConsolePanel();

  void executeCommand(const QString &cmd);

public slots:
  void selectNone();
};

#endif  // TESTPANEL_H
