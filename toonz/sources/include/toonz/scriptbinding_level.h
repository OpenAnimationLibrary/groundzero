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

public:
  Level();
  explicit Level(TXshSimpleLevel *sl);
  ~Level() override;

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

  // Static helper for parsing frame IDs
  static TFrameId getFid(const QJSValue &arg, QString &err);

private:
  Q_DISABLE_COPY(Level)
};

}  // namespace TScriptBinding

// Declare meta-type for script binding
Q_DECLARE_METATYPE(TScriptBinding::Level *)

#endif  // SCRIPTBINDING_LEVEL_H
