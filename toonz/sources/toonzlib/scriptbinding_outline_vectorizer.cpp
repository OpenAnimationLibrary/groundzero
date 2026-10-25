

#include "toonz/scriptbinding_outline_vectorizer.h"
#include "toonz/scriptbinding_level.h"
#include "toonz/tcenterlinevectorizer.h"
#include "toonz/stage.h"
#include "ttoonzimage.h"
#include "tpalette.h"
#include <memory>  // for std::unique_ptr, std::make_unique

namespace TScriptBinding {

//----------------------------------------------------------
OutlineVectorizer::OutlineVectorizer()
    : m_parameters(std::make_unique<NewOutlineConfiguration>()) {
  // Initialize transparent color to a safe default (fully transparent black)
  m_parameters->m_transparentColor = TPixel32::Transparent;
}
//----------------------------------------------------------
QString OutlineVectorizer::toString() { return "Outline Vectorizer"; }
//----------------------------------------------------------
QJSValue OutlineVectorizer::ctor(QJSEngine *engine, const QJSValueList &args) {
  return create(engine, new OutlineVectorizer());
}
//----------------------------------------------------------
static QJSValue vectorizeImage(QJSEngine *engine, const TImageP &src,
                               TPalette *palette,
                               NewOutlineConfiguration *parameters) {
  VectorizerCore vc;
  TAffine dpiAff;
  double factor = Stage::inch;
  double dpix = factor / 72, dpiy = factor / 72;
  TPointD center;

  if (TRasterImageP ri = src) {
    ri->getDpi(dpix, dpiy);
    center = ri->getRaster()->getCenterD();
  } else if (TToonzImageP ti = src) {
    ti->getDpi(dpix, dpiy);
    center = ti->getRaster()->getCenterD();
  } else {
    engine->throwError(QObject::tr("Vectorization failed"));
    return QJSValue();
  }

  if (dpix != 0.0 && dpiy != 0.0) dpiAff = TScale(factor / dpix, factor / dpiy);
  factor = norm(dpiAff * TPointD(1, 0));

  parameters->m_affine     = dpiAff * TTranslation(-center);
  parameters->m_thickScale = factor;

  // Hold the palette during vectorization – TPaletteP manages refcount
  TPaletteP paletteHolder = palette;
  TVectorImageP vi = vc.vectorize(src, *parameters, paletteHolder.getPointer());
  vi->setPalette(paletteHolder.getPointer());

  return Wrapper::create(engine, new Image(vi));
}
//----------------------------------------------------------
QJSValue OutlineVectorizer::vectorize(QJSValue arg) {
  Level *level = cast<Level>(arg);
  Image *img   = cast<Image>(arg);
  QString type;
  TPaletteP palette;  // Smart pointer – automatic release

  if (level) {
    type = level->getType();
    if (type != "Raster" && type != "ToonzRaster")
      return throwError(tr("Can't vectorize a %1 level").arg(type));
    if (level->getFrameCount() <= 0)
      return throwError(tr("Can't vectorize a level with no frames"));
    palette = level->getSimpleLevel()->getPalette();  // may be null
  } else if (img) {
    type = img->getType();
    if (type != "Raster" && type != "ToonzRaster")
      return throwError(tr("Can't vectorize a %1 image").arg(type));
    if (TToonzImageP ti = img->getImg()) {
      palette = ti->getPalette();
    }
  } else {
    return throwError(tr("Bad argument (%1): should be an Image or a Level")
                          .arg(arg.toString()));
  }

  // If no palette was provided, create a new one.
  if (!palette) palette = new TPalette();

  if (img) {
    return vectorizeImage(engine(), img->getImg(), palette.getPointer(),
                          m_parameters.get());
  } else if (level) {
    Level *outputLevel = new Level();
    QJSValue newLevel  = create(engine(), outputLevel);
    QList<TFrameId> fids;
    level->getFrameIds(fids);
    for (const TFrameId &fid : fids) {
      TImageP srcImg = level->getImg(fid);
      if (srcImg && (srcImg->getType() == TImage::RASTER ||
                     srcImg->getType() == TImage::TOONZ_RASTER)) {
        QJSValue newFrame = vectorizeImage(
            engine(), srcImg, palette.getPointer(), m_parameters.get());
        if (newFrame.isUndefined()) {
          return QJSValue();  // the error has been thrown
        }
        outputLevel->setFrame(QString::fromStdString(fid.expand()), newFrame);
      }
    }
    return newLevel;
  }

  // Unreachable
  return QJSValue();
}
//----------------------------------------------------------
int OutlineVectorizer::getAccuracy() const {
  return (5.0 - m_parameters->m_mergeTol) * 2.0;
}
void OutlineVectorizer::setAccuracy(int v) {
  m_parameters->m_mergeTol = 5.0 - v * 0.5;
}
//----------------------------------------------------------
int OutlineVectorizer::getDespeckling() const {
  return m_parameters->m_despeckling;
}
void OutlineVectorizer::setDespeckling(int v) {
  m_parameters->m_despeckling = v;
}
//----------------------------------------------------------
bool OutlineVectorizer::getPreservePaintedAreas() const {
  return !m_parameters->m_leaveUnpainted;
}
void OutlineVectorizer::setPreservePaintedAreas(bool v) {
  m_parameters->m_leaveUnpainted = !v;
}
//----------------------------------------------------------
double OutlineVectorizer::getCornerAdherence() const {
  return m_parameters->m_adherenceTol * 100.0;
}
void OutlineVectorizer::setCornerAdherence(double v) {
  m_parameters->m_adherenceTol = 0.01 * v;
}
//----------------------------------------------------------
double OutlineVectorizer::getCornerAngle() const {
  return m_parameters->m_angleTol * 180.0;
}
void OutlineVectorizer::setCornerAngle(double v) {
  m_parameters->m_angleTol = v / 180.0;
}
//----------------------------------------------------------
double OutlineVectorizer::getCornerCurveRadius() const {
  return m_parameters->m_relativeTol * 100;
}
void OutlineVectorizer::setCornerCurveRadius(double v) {
  m_parameters->m_relativeTol = 0.01 * v;
}
//----------------------------------------------------------
int OutlineVectorizer::getMaxColors() const {
  return m_parameters->m_maxColors;
}
void OutlineVectorizer::setMaxColors(int v) { m_parameters->m_maxColors = v; }
//----------------------------------------------------------
QString OutlineVectorizer::getTransparentColor() const {
  // TPixel32 is now always initialized (see constructor)
  TPixel32 c = m_parameters->m_transparentColor;
  QColor color(c.r, c.g, c.b, c.m);
  return color.name();
}
//----------------------------------------------------------
void OutlineVectorizer::setTransparentColor(const QString &colorName) {
  QColor color;
  color.setNamedColor(colorName);
  if (color.isValid()) {
    m_parameters->m_transparentColor =
        TPixel32(color.red(), color.green(), color.blue(), color.alpha());
  } else {
    // Throw error and abort
    throwError(tr("Invalid color: %1").arg(colorName));
    return;
  }
}
//----------------------------------------------------------
int OutlineVectorizer::getToneThreshold() const {
  return m_parameters->m_toneTol;
}
void OutlineVectorizer::setToneThreshold(int v) { m_parameters->m_toneTol = v; }
//----------------------------------------------------------

}  // namespace TScriptBinding
