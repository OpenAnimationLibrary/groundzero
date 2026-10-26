

#include "toonz/scriptbinding_image.h"
#include "toonz/scriptbinding_level.h"
#include "toonz/scriptbinding_files.h"
#include "tsystem.h"
#include "ttoonzimage.h"
#include "trasterimage.h"
#include "traster.h"
#include "tpixel.h"
#include "tpixelutils.h"
#include "tfiletype.h"
#include "timage_io.h"
#include "tlevel_io.h"

#include <cmath>

namespace TScriptBinding {

Image::Image() {}

Image::Image(const TImageP img) : m_img(img) {}

Image::Image(TImage *img) : m_img(img) {}

Image::~Image() {}

QJSValue Image::ctor(QJSEngine *engine, const QJSValueList &args) {
  Image *img   = new Image();
  QJSValue obj = create(engine, img);
  if (!checkArgumentCount(engine, args, "the Image constructor", 0, 1))
    return QJSValue();
  if (args.size() == 1) {
    return img->load(args[0]);
  }
  return obj;
}

QString Image::toString() {
  if (m_img) {
    TImage::Type type = m_img->getType();
    if (type == TImage::RASTER)
      return QString("Raster image ( %1 x %2 )")
          .arg(getWidth())
          .arg(getHeight());
    else if (type == TImage::TOONZ_RASTER)
      return QString("Toonz raster image ( %1 x %2 )")
          .arg(getWidth())
          .arg(getHeight());
    else if (type == TImage::VECTOR)
      return QString("Vector image");
    else
      return QString("Image");
  } else {
    return "Empty image";
  }
}

int Image::getWidth() {
  return !!m_img && !!m_img->raster() ? m_img->raster()->getSize().lx : 0;
}

int Image::getHeight() {
  return !!m_img && !!m_img->raster() ? m_img->raster()->getSize().ly : 0;
}

double Image::getDpi() {
  if (TRasterImageP ri = m_img) {
    double dpix = 0, dpiy = 0;
    ri->getDpi(dpix, dpiy);
    return dpix;
  } else if (TToonzImageP ti = m_img) {
    double dpix = 0, dpiy = 0;
    ti->getDpi(dpix, dpiy);
    return dpix;
  } else
    return 0;
}

void Image::setDpi(double dpi) {
  QString err = changeDpi(m_img, dpi);
  if (!err.isEmpty()) throwError(err);
}

QString Image::changeDpi(const TImageP &img, double dpi) {
  if (!std::isfinite(dpi) || dpi <= 0)
    return tr("Bad dpi (%1): expected a number > 0").arg(dpi);
  if (TRasterImageP ri = img) {
    ri->setDpi(dpi, dpi);
    return "";
  } else if (TToonzImageP ti = img) {
    ti->setDpi(dpi, dpi);
    return "";
  }
  return tr("Can't set the dpi of a %1 image").arg(getTypeName(img));
}

namespace {

// The raster of a Raster image, with the message of the error if the pixels
// of img can't be accessed
TRasterP getPixelRaster(const TImageP &img, QString &err) {
  TRasterImageP ri = img;
  if (!ri || !ri->getRaster()) {
    err = QObject::tr(
              "Pixels can be accessed only in Raster images, not in "
              "%1 images")
              .arg(Image::getTypeName(img));
    return TRasterP();
  }
  err.clear();
  return ri->getRaster();
}

QString outsideMessage(int x, int y, const TRasterP &ras) {
  return QObject::tr("Pixel (%1, %2) is outside the image (%3 x %4)")
      .arg(x)
      .arg(y)
      .arg(ras->getLx())
      .arg(ras->getLy());
}

// An integer pixel coordinate
bool getCoordinate(const QJSValue &value, int &coordinate) {
  if (!value.isNumber()) return false;
  double d = value.toNumber();
  if (!std::isfinite(d) || d != std::floor(d) || std::abs(d) > 1.0e9)
    return false;
  coordinate = (int)d;
  return true;
}

}  // namespace

QString Image::readPixel(const TImageP &img, int x, int y, QColor &color) {
  QString err;
  TRasterP ras = getPixelRaster(img, err);
  if (!ras) return err;
  if (x < 0 || y < 0 || x >= ras->getLx() || y >= ras->getLy())
    return outsideMessage(x, y, ras);

  TPixel32 pix;
  if (TRaster32P ras32 = ras) {
    ras32->lock();
    pix = ras32->pixels(y)[x];
    ras32->unlock();
  } else if (TRaster64P ras64 = ras) {
    ras64->lock();
    pix = toPixel32(ras64->pixels(y)[x]);
    ras64->unlock();
  } else {
    return tr("Unsupported raster format");
  }
  color = QColor(pix.r, pix.g, pix.b, pix.m);
  return "";
}

QString Image::writePixel(const TImageP &img, int x, int y,
                          const QColor &color) {
  QString err;
  TRasterP ras = getPixelRaster(img, err);
  if (!ras) return err;
  if (x < 0 || y < 0 || x >= ras->getLx() || y >= ras->getLy())
    return outsideMessage(x, y, ras);

  TPixel32 pix(color.red(), color.green(), color.blue(), color.alpha());
  if (TRaster32P ras32 = ras) {
    ras32->lock();
    ras32->pixels(y)[x] = pix;
    ras32->unlock();
  } else if (TRaster64P ras64 = ras) {
    ras64->lock();
    ras64->pixels(y)[x] = toPixel64(pix);
    ras64->unlock();
  } else {
    return tr("Unsupported raster format");
  }
  return "";
}

QString Image::fillImage(const TImageP &img, const QColor &color) {
  QString err;
  TRasterP ras = getPixelRaster(img, err);
  if (!ras) return err;

  TPixel32 pix(color.red(), color.green(), color.blue(), color.alpha());
  if (TRaster32P ras32 = ras)
    ras32->fill(pix);
  else if (TRaster64P ras64 = ras)
    ras64->fill(toPixel64(pix));
  else
    return tr("Unsupported raster format");
  return "";
}

QJSValue Image::getPixel(const QJSValue &xArg, const QJSValue &yArg) {
  int x = 0, y = 0;
  if (!getCoordinate(xArg, x) || !getCoordinate(yArg, y))
    return throwError(tr("Bad pixel coordinates (%1, %2): expected integers")
                          .arg(xArg.toString(), yArg.toString()));
  QColor color;
  QString err = readPixel(m_img, x, y, color);
  if (!err.isEmpty()) return throwError(err);
  return QJSValue(color.name(QColor::HexArgb));
}

QJSValue Image::setPixel(const QJSValue &xArg, const QJSValue &yArg,
                         const QString &colorName) {
  int x = 0, y = 0;
  if (!getCoordinate(xArg, x) || !getCoordinate(yArg, y))
    return throwError(tr("Bad pixel coordinates (%1, %2): expected integers")
                          .arg(xArg.toString(), yArg.toString()));
  QColor color;
  if (!checkColor(engine(), colorName, color)) return QJSValue();
  QString err = writePixel(m_img, x, y, color);
  if (!err.isEmpty()) return throwError(err);
  return self();
}

QJSValue Image::fill(const QString &colorName) {
  QColor color;
  if (!checkColor(engine(), colorName, color)) return QJSValue();
  QString err = fillImage(m_img, color);
  if (!err.isEmpty()) return throwError(err);
  return self();
}

QJSValue Image::clone() {
  if (!m_img) return throwError(tr("Can't clone an empty image"));
  return create(engine(), new Image(m_img->cloneImage()));
}

QString Image::getTypeName(const TImageP &img) {
  if (!img) return "Empty";
  TImage::Type type = img->getType();
  if (type == TImage::RASTER)
    return "Raster";
  else if (type == TImage::TOONZ_RASTER)
    return "ToonzRaster";
  else if (type == TImage::VECTOR)
    return "Vector";
  else
    return "Unknown";
}

QString Image::getType() const { return getTypeName(m_img); }

QString Image::loadImage(const TFilePath &fp, const QString &fpStr,
                         TImageP &img, QString &warning) {
  // clear the old image (if any)
  img = TImageP();
  warning.clear();

  try {
    // check if the file/level does exist
    if (!TSystem::doesExistFileOrLevel(fp)) {
      return tr("File %1 doesn't exist").arg(fpStr);
    }

    // the file could be a level
    TFileType::Type fileType = TFileType::getInfo(fp);
    if (TFileType::isLevel(fileType)) {
      // file is a level: read first frame
      TLevelReaderP lr(fp);
      TLevelP level = lr->loadInfo();
      int n         = level->getFrameCount();
      if (n > 0) {
        // there are some frames
        TFrameId fid = fp.getFrame();
        if (fid == TFrameId::NO_FRAME || fid == TFrameId::EMPTY_FRAME)
          fid = level->begin()->first;

        img = lr->getFrameReader(fid)->load();
        if (!img) {
          return QString("Could not read %1").arg(fpStr);
        }
        img->setPalette(level->getPalette());
        if (n > 1 && (fp.getFrame() == TFrameId::EMPTY_FRAME ||
                      fp.getFrame() == TFrameId::NO_FRAME)) {
          // warning: a multi-frame level read into an Image
          warning = tr("Loaded first frame of %1").arg(n);
        }
      } else {
        // level contains no frame (not sure it can even happen)
        return QString("%1 contains no frames").arg(fpStr);
      }
    } else {
      // plain image: try to read it
      if (!TImageReader::load(fp, img)) {
        return QString("File %1 not found or not readable").arg(fpStr);
      }
    }
    return "";
  } catch (...) {
    return tr("Unexpected error while reading image").arg(fpStr);
  }
}

QJSValue Image::load(const QJSValue &fpArg) {
  // clear the old image (if any)
  m_img = TImageP();

  // get the path
  TFilePath fp;
  if (!checkFilePath(engine(), fpArg, fp)) return QJSValue();
  QString fpStr = fpArg.toString();

  QString warningText;
  QString err = loadImage(fp, fpStr, m_img, warningText);
  if (!warningText.isEmpty()) warning(warningText);
  if (!err.isEmpty()) return throwError(err);
  // return a reference to the Image object
  return self();
}

QString Image::saveImage(const TImageP &img, const TFilePath &fp,
                         const QString &fpStr) {
  if (!img) return "Can't save an empty image";

  // handle conversion (if it is needed and possible)
  TFileType::Type fileType = TFileType::getInfo(fp);

  bool isCompatible = false;
  if (TFileType::isFullColor(fileType)) {
    if (img->getType() == TImage::RASTER) isCompatible = true;
  } else if (TFileType::isVector(fileType)) {
    if (img->getType() == TImage::VECTOR) isCompatible = true;
  } else if (fileType & TFileType::CMAPPED_IMAGE) {
    if (img->getType() == TImage::TOONZ_RASTER) isCompatible = true;
  } else {
    return tr("Unrecognized file type :").arg(fpStr);
  }
  if (!isCompatible) {
    return tr("Can't save a %1 image to this file type : %2")
        .arg(getTypeName(img))
        .arg(fpStr);
  }

  try {
    if (TFileType::isLevel(fileType)) {
      TLevelP level = new TLevel();
      level->setPalette(img->getPalette());
      level->setFrame(TFrameId(1), img);
      TLevelWriterP lw(fp);
      if (img->getPalette()) lw->setPalette(img->getPalette());
      lw->save(level);
    } else {
      TImageWriterP iw(fp);
      iw->save(fp, img);
    }
    return "";
  } catch (...) {
    return tr("Unexpected error while writing image").arg(fpStr);
  }
}

QJSValue Image::save(const QJSValue &fpArg) {
  // clear the old image (if any)
  if (!m_img) {
    return throwError("Can't save an empty image");
  }

  // get the path
  TFilePath fp;
  if (!checkFilePath(engine(), fpArg, fp)) return QJSValue();
  QString fpStr = fpArg.toString();

  QString err = saveImage(m_img, fp, fpStr);
  if (!err.isEmpty()) return throwError(err);
  // return a reference to the Image object
  return self();
}

bool checkImage(QJSEngine *engine, const QJSValue &value, Image *&img) {
  img = cast<Image>(value);
  if (!img || !img->getImg()) {
    engine->throwError(
        QObject::tr("Bad argument (%1): should be an Image (not empty)")
            .arg(value.toString()));
    return false;
  } else
    return true;
}

}  // namespace TScriptBinding
