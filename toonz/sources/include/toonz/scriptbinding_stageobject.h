#pragma once

#ifndef SCRIPTBINDING_STAGEOBJECT_H
#define SCRIPTBINDING_STAGEOBJECT_H

#include "toonz/scriptbinding.h"

#include <vector>

// Forward declarations
class TStageObject;
class TXsheet;

namespace TScriptBinding {

class Scene;

// A stage object (column, pegbar, camera or the table) of the xsheet of a
// Scene, with its animation channels. The objects are obtained from
// Scene.getStageObject(name), Scene.getStageObjects() and Scene.newPegbar().
//
// Frames are xsheet rows, numbered from 0 like the rows of Scene.setCell()
// (the frame shown as 1 in the UI is the frame 0). The channel values are in
// the internal units of the channels (see the channel table in the .cpp),
// independent of the unit preferences. The changes, made with the keyframe
// machinery of the UI, are not undoable (like the other Scene edits).
class DVAPI StageObject final : public Wrapper {
  Q_OBJECT

  // the script value of the Scene, which keeps it alive
  QJSValue m_sceneValue;
  Scene *m_scene;
  TStageObject *m_obj;

public:
  StageObject(const QJSValue &sceneValue, Scene *scene, TStageObject *obj);
  ~StageObject() override;

  WRAPPER_STD_METHODS(StageObject)
  Q_INVOKABLE QString toString();

  // The object id as the UI shows it: Col1, Peg1, Camera1, Table. Note: it
  // replaces the numeric id property of the other bound objects
  Q_PROPERTY(QString id READ getIdName)
  QString getIdName() const;

  // Column, Pegbar, Camera or Table
  Q_PROPERTY(QString type READ getType)
  QString getType() const;

  // The name given by the user (the id if none); "" restores the default
  Q_PROPERTY(QString name READ getName WRITE setName)
  QString getName() const;
  void setName(const QString &name);

  // The name of the parent object ("" if none). Assigning "" links columns
  // and pegbars to the table
  Q_PROPERTY(QString parent READ getParentName WRITE setParentName)
  QString getParentName() const;
  void setParentName(const QString &name);

  Q_INVOKABLE QJSValue getValue(const QJSValue &channel, const QJSValue &frame);
  Q_INVOKABLE QJSValue setValue(const QJSValue &channel, const QJSValue &frame,
                                const QJSValue &value);
  Q_INVOKABLE QJSValue setKeyframe(const QJSValue &channel,
                                   const QJSValue &frame,
                                   const QJSValue &value);
  Q_INVOKABLE QJSValue removeKeyframe(const QJSValue &channel,
                                      const QJSValue &frame);
  Q_INVOKABLE QJSValue isKeyframe(const QJSValue &channel,
                                  const QJSValue &frame);
  Q_INVOKABLE QJSValue getKeyframes(const QJSValue &channel);

  // The center (in inches), as the center fields of the Animate tool
  Q_INVOKABLE QJSValue getCenter(const QJSValue &frame);
  Q_INVOKABLE QJSValue setCenter(const QJSValue &frame, const QJSValue &x,
                                 const QJSValue &y);

  // The stage objects of xsh, in the order of the stage object tree
  // (cameras, table, pegbars, then the columns of the xsheet)
  static void listStageObjects(TXsheet *xsh, std::vector<TStageObject *> &objs);
  // The stage object named name: an id (Col1, Peg1, Camera1, Table) of an
  // existing object, else the first object (in the listStageObjects() order)
  // whose name is name. Returns 0 if there is none.
  static TStageObject *findStageObject(TXsheet *xsh, const QString &name);

private:
  // The wrapped object, or 0 (after making the calling script throw) if it
  // is no longer in the xsheet of the scene (e.g. its column was deleted)
  TStageObject *getStageObject() const;
  TXsheet *getXsheet() const;

  Q_DISABLE_COPY(StageObject)
};

}  // namespace TScriptBinding

Q_DECLARE_METATYPE(TScriptBinding::StageObject *)

#endif  // SCRIPTBINDING_STAGEOBJECT_H
