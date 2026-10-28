#pragma once

#ifndef SCRIPTBINDING_LEVEL_H
#define SCRIPTBINDING_LEVEL_H

#include "toonz/scriptbinding.h"
#include "toonz/scriptbinding_image.h"

// Forward declarations
class ToonzScene;
class TXshSimpleLevel;
class TFrameId;

namespace TScriptBinding {

class DVAPI Level final : public Wrapper {
  Q_OBJECT

  TXshSimpleLevel *m_sl;
  ToonzScene *m_scene;
  bool m_sceneOwner;
  int m_type;
  // The script Scene that owns m_scene (a level taken from a scene): the
  // garbage collector must not delete the scene while the level is in use
  QJSValue m_sceneValue;

public:
  Level();
  explicit Level(TXshSimpleLevel *sl);
  ~Level() override;

  void keepScene(const QJSValue &sceneValue) { m_sceneValue = sceneValue; }

  WRAPPER_STD_METHODS(Level)
  Q_INVOKABLE QString toString();

  // Properties
  Q_PROPERTY(QString type READ getType)
  QString getType() const;

  Q_PROPERTY(int frameCount READ getFrameCount)
  int getFrameCount() const;

  Q_PROPERTY(QString name READ getName WRITE setName)
  QString getName() const;
  void setName(const QString &name);

  Q_PROPERTY(QJSValue path READ getPath WRITE setPath)
  QJSValue getPath() const;
  void setPath(const QJSValue &pathArg);

  // Frame operations
  Q_INVOKABLE QJSValue getFrame(const QJSValue &fid);
  Q_INVOKABLE QJSValue getFrameByIndex(const QJSValue &index);
  Q_INVOKABLE QJSValue setFrame(const QJSValue &fid, const QJSValue &image);
  // True if the level has the frame fid
  Q_INVOKABLE QJSValue hasFrame(const QJSValue &fid);
  // Removes the frame fid, if the level has it (the cells of the scenes that
  // use it are not changed)
  Q_INVOKABLE QJSValue removeFrame(const QJSValue &fid);
  // Adds the image after the last frame; returns the id of the new frame
  Q_INVOKABLE QJSValue appendFrame(const QJSValue &image);

  // Level operations
  Q_INVOKABLE QJSValue getFrameIds();
  Q_INVOKABLE QJSValue load(const QJSValue &fp);
  Q_INVOKABLE QJSValue save(const QJSValue &fp);

  // Internal helper methods
  void getFrameIds(QList<TFrameId> &fids);
  TImageP getImg(const TFrameId &fid);
  int setFrame(const TFrameId &fid, const TImageP &img);

  // Accessor for underlying simple level
  TXshSimpleLevel *getSimpleLevel() const { return m_sl; }

  // Static helpers (shared with the Python binding). getFid() parses a frame
  // id; nextFrameId() is the id of the frame appended after the last one of
  // the level (sl can be 0)
  static TFrameId nextFrameId(const TXshSimpleLevel *sl);
  static TFrameId getFid(const QJSValue &arg, QString &err);

private:
  Q_DISABLE_COPY(Level)
};

}  // namespace TScriptBinding

// Declare meta-type for script binding
Q_DECLARE_METATYPE(TScriptBinding::Level *)

#endif  // SCRIPTBINDING_LEVEL_H
