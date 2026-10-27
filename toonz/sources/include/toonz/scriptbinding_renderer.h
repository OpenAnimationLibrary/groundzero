#pragma once

#ifndef SCRIPTBINDING_RENDERER_H
#define SCRIPTBINDING_RENDERER_H

#include "toonz/scriptbinding.h"

namespace TScriptBinding {

class DVAPI Renderer final : public Wrapper {
  Q_OBJECT
  class Imp;
  Imp *m_imp;

public:
  Renderer();
  ~Renderer();

  WRAPPER_STD_METHODS(Renderer)
  Q_INVOKABLE QString toString();

  Q_INVOKABLE QJSValue renderScene(const QJSValue &scene);
  Q_INVOKABLE QJSValue renderFrame(const QJSValue &scene, int frame);

  Q_INVOKABLE void dumpCache();

  // Renders the frame row (from 0) of scene with its output settings, as
  // renderFrame() does; columns are the indices of the columns to render
  // (all if empty). Returns a null image if the rendering fails. Used by
  // Scene.renderFrame() and by the Python binding.
  static TImageP renderFrameImage(ToonzScene *scene, int row,
                                  const QList<int> &columns = QList<int>());
};

}  // namespace TScriptBinding

Q_DECLARE_METATYPE(TScriptBinding::Renderer *)

#endif
