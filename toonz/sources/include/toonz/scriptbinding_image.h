#pragma once

#ifndef SCRIPTBINDING_IMAGE_H
#define SCRIPTBINDING_IMAGE_H

#include "toonz/scriptbinding.h"

namespace TScriptBinding {

class DVAPI Image final : public Wrapper {
  Q_OBJECT
  TImageP m_img;

public:
  Image();
  Image(const TImageP img);
  Image(TImage *img);

  ~Image();

  WRAPPER_STD_METHODS(Image)
  Q_INVOKABLE QString toString();

  Q_PROPERTY(QString type READ getType)
  QString getType() const;

  const TImageP &getImg() const { return m_img; }
  void setImg(const TImageP &img) { m_img = img; }

  Q_PROPERTY(int width READ getWidth)
  Q_PROPERTY(int height READ getHeight)
  Q_PROPERTY(double dpi READ getDpi WRITE setDpi)
  int getWidth();
  int getHeight();
  double getDpi();
  void setDpi(double dpi);

  Q_INVOKABLE QJSValue load(const QJSValue &fp);
  Q_INVOKABLE QJSValue save(const QJSValue &fp);

  // Pixels of the Raster images: (0, 0) is the lower left corner, the colors
  // are the names of QColor ("red", "#ff8800", "#80ff8800" as #AARRGGBB)
  Q_INVOKABLE QJSValue getPixel(const QJSValue &x, const QJSValue &y);
  Q_INVOKABLE QJSValue setPixel(const QJSValue &x, const QJSValue &y,
                                const QString &colorName);
  Q_INVOKABLE QJSValue fill(const QString &colorName);
  Q_INVOKABLE QJSValue clone();

  // The operations of the methods above, shared with the Python binding.
  // They return the message of the error, or "" if there is none.
  static QString getTypeName(const TImageP &img);
  static QString readPixel(const TImageP &img, int x, int y, QColor &color);
  static QString writePixel(const TImageP &img, int x, int y,
                            const QColor &color);
  static QString fillImage(const TImageP &img, const QColor &color);
  static QString changeDpi(const TImageP &img, double dpi);
  // fpStr is the text of the path for the error messages. img is cleared
  // first. warning is the message of a warning, if any.
  static QString loadImage(const TFilePath &fp, const QString &fpStr,
                           TImageP &img, QString &warning);
  static QString saveImage(const TImageP &img, const TFilePath &fp,
                           const QString &fpStr);
};

// if value is not an Image (or the image is empty) it makes the calling script
// throw an error and returns false
DVAPI bool checkImage(QJSEngine *engine, const QJSValue &value, Image *&img);

}  // namespace TScriptBinding

Q_DECLARE_METATYPE(TScriptBinding::Image *)

#endif
