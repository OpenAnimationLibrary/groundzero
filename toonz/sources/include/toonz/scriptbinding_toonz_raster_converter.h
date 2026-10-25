#pragma once

#ifndef SCRIPTBINDING_TOONZ_RASTER_CONVERTER_H
#define SCRIPTBINDING_TOONZ_RASTER_CONVERTER_H

#include "toonz/scriptbinding.h"

class ToonzScene;
class TXshSimpleLevel;

namespace TScriptBinding {

class DVAPI ToonzRasterConverter final : public Wrapper {
  Q_OBJECT
  bool m_flatSource;

public:
  ToonzRasterConverter();
  ~ToonzRasterConverter();

  Q_INVOKABLE QString toString();
  WRAPPER_STD_METHODS(ToonzRasterConverter)

  Q_PROPERTY(bool flatSource READ getFlatSource WRITE setFlatSource)
  bool getFlatSource() const { return m_flatSource; }
  void setFlatSource(bool v) { m_flatSource = v; }

  Q_INVOKABLE int foo(int x) { return x * 2; }

  // ToonzRasterConverter.convert(img_or_level), also callable as a method
  // of the instances (see bindAll())
  static QJSValue convert(QJSEngine *engine, const QJSValueList &args);
};

}  // namespace TScriptBinding

Q_DECLARE_METATYPE(TScriptBinding::ToonzRasterConverter *)

#endif
