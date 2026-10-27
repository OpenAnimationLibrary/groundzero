

#include "toonz/scriptbinding_toonz_raster_converter.h"
#include "toonz/scriptbinding_level.h"
#include "tropcm.h"
#include "convert2tlv.h"

namespace TScriptBinding {

ToonzRasterConverter::ToonzRasterConverter() : m_flatSource(false) {}

ToonzRasterConverter::~ToonzRasterConverter() {}

QJSValue ToonzRasterConverter::ctor(QJSEngine *engine,
                                    const QJSValueList &args) {
  return create(engine, new ToonzRasterConverter());
}

QString ToonzRasterConverter::toString() { return "ToonzRasterConverter"; }

QJSValue ToonzRasterConverter::convert(QJSEngine *engine,
                                       const QJSValueList &args) {
  QString err;
  if (args.size() != 1) {
    engine->throwError(
        QString("Expected one argument (a raster Level or a raster Image)"));
    return QJSValue();
  }
  QJSValue arg = args[0];
  Level *level = cast<Level>(arg);
  Image *img   = cast<Image>(arg);
  QString type;
  if (level) {
    type = level->getType();
    if (type != "Raster")
      err = tr("Can't convert a %1 level").arg(type);
    else if (level->getFrameCount() <= 0)
      err = tr("Can't convert a level with no frames");
  } else if (img) {
    type = img->getType();
    if (type != "Raster") err = tr("Can't convert a %1 image").arg(type);
  } else {
    err = tr("Bad argument (%1): should be a raster Level or a raster Image")
              .arg(arg.toString());
  }
  if (!err.isEmpty()) {
    engine->throwError(err);
    return QJSValue();
  }
  RasterToToonzRasterConverter converter;
  if (img) {
    TRasterImageP ri = img->getImg();
    TToonzImageP ti  = converter.convert(ri);
    ti->setPalette(converter.getPalette().getPointer());
    return create(engine, new Image(ti));
  } else
    return QJSValue();
}

}  // namespace TScriptBinding
