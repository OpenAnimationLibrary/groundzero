#pragma once

#ifndef SCRIPTBINDING_SCENE_H
#define SCRIPTBINDING_SCENE_H

#include "toonz/scriptbinding.h"

namespace TScriptBinding {

class DVAPI Scene final : public Wrapper {
  Q_OBJECT
  ToonzScene *m_scene;

public:
  Scene();
  ~Scene();

  WRAPPER_STD_METHODS(Scene)
  Q_INVOKABLE QString toString();

  Q_PROPERTY(int frameCount READ getFrameCount)
  Q_PROPERTY(int columnCount READ getColumnCount)
  int getFrameCount();
  int getColumnCount();

  Q_INVOKABLE QJSValue load(const QJSValue &fpArg);
  Q_INVOKABLE QJSValue save(const QJSValue &fpArg);

  QString doSetCell(int row, int col, const QJSValue &level,
                    const QJSValue &fid);
  Q_INVOKABLE QJSValue setCell(int row, int col, const QJSValue &level,
                               const QJSValue &fid);
  Q_INVOKABLE QJSValue setCell(int row, int col, const QJSValue &cell);
  Q_INVOKABLE QJSValue getCell(int row, int col);

  Q_INVOKABLE QJSValue insertColumn(int col);
  Q_INVOKABLE QJSValue deleteColumn(int col);

  // The columns of the xsheet: an array of objects with the attributes
  // index, id (Col1), name, type (Level, Sound, SoundText, Zerary, Palette,
  // Mesh or Empty), firstRow and lastRow (the rows of the first and of the
  // last cell, from 0; -1 for the columns without cells)
  Q_INVOKABLE QJSValue getColumns();

  // Renders the row (from 0) with the output settings of the scene; returns
  // an Image
  Q_INVOKABLE QJSValue renderFrame(int frame);

  // The scene check (doc/scene_checker.md): an array of objects with
  // severity (error, warning or info), kind (missingDrawing, ...), title,
  // text, column, row, rowCount (-1, -1 and 0 when not about a cell), level
  // and subXsheet (names, "" for none) and styleId (-1 for none). The
  // optional argument is an object: {unusedStyles: true} also looks for
  // unused styles, which loads every drawing.
  Q_INVOKABLE QJSValue check(const QJSValue &options = QJSValue());

  struct ColumnInfo {
    int m_index;
    QString m_id, m_name, m_type;
    int m_firstRow, m_lastRow;
  };
  // The description of the column col (shared with the Python binding)
  static ColumnInfo getColumnInfo(ToonzScene *scene, int col);

  Q_INVOKABLE QJSValue getLevels() const;
  Q_INVOKABLE QJSValue getLevel(const QString &name) const;
  Q_INVOKABLE QJSValue newLevel(const QString &type, const QString &name) const;
  Q_INVOKABLE QJSValue loadLevel(const QString &name,
                                 const QJSValue &path) const;

  // The stage objects of the xsheet (see StageObject): by id (Col1, Peg1,
  // Camera1, Table) or by name, all of them, and a new pegbar
  Q_INVOKABLE QJSValue getStageObject(const QString &name);
  Q_INVOKABLE QJSValue getStageObjects();
  Q_INVOKABLE QJSValue newPegbar();

  ToonzScene *getToonzScene() const { return m_scene; }
};

}  // namespace TScriptBinding

Q_DECLARE_METATYPE(TScriptBinding::Scene *)

#endif
