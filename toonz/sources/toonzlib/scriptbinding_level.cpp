
#include "toonz/scriptbinding_level.h"

// Toonz script bindings
#include "toonz/scriptbinding_files.h"

// TnzLib includes
#include "toonz/tcenterlinevectorizer.h"
#include "toonz/tcamera.h"
#include "toonz/stage.h"
#include "toonz/txshleveltypes.h"
#include "toonz/levelproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/levelset.h"

// TnzCore / TnzBase includes
#include "tlevel_io.h"
#include "tlevel.h"
#include "timage_io.h"
#include "trop.h"
#include "trasterimage.h"
#include "ttoonzimage.h"
#include "tvectorimage.h"
#include "tpalette.h"
#include "tofflinegl.h"
#include "tvectorrenderdata.h"
#include "tgeometry.h"
#include "tfiletype.h"
#include "tsystem.h"

// Qt includes
#include <QRegularExpression>
#include <QColor>

namespace TScriptBinding {

Level::Level()
    : m_sl(nullptr)
    , m_type(NO_XSHLEVEL)
    , m_scene(new ToonzScene())
    , m_sceneOwner(true) {}

Level::Level(TXshSimpleLevel *sl)
    : m_sl(sl)
    , m_type(sl->getType())
    , m_scene(sl->getScene())
    , m_sceneOwner(false) {
  if (m_sl) {
    m_sl->addRef();
  }
}

Level::~Level() {
  if (m_sceneOwner) {
    delete m_scene;
    m_scene = nullptr;
  }
  if (m_sl) {
    m_sl->release();
    m_sl = nullptr;
  }
}

QJSValue Level::ctor(QJSEngine *engine, const QJSValueList &args) {
  Level *level = new Level();
  QJSValue obj = create(engine, level);

  if (args.size() == 1) {
    return level->load(args[0]);
  }

  return obj;
}

QString Level::toString() {
  QString info = "(";
  QString comma;

  if (getName() != "") {
    info.append(comma).append(getName());
    comma = ", ";
  }

  info.append(comma).append(tr("%1 frames").arg(getFrameCount()));
  info.append(")");

  switch (m_type) {
  case PLI_XSHLEVEL:
    return QString("Vector level %1").arg(info);
  case TZP_XSHLEVEL:
    return QString("Toonz level %1").arg(info);
  case NO_XSHLEVEL:
    return QString("Empty level");
  case OVL_XSHLEVEL:
    return QString("Raster level %1").arg(info);
  default:
    return QString("Level %1").arg(info);
  }
}

QString Level::getType() const {
  switch (m_type) {
  case NO_XSHLEVEL:
    return "Empty";
  case PLI_XSHLEVEL:
    return "Vector";
  case TZP_XSHLEVEL:
    return "ToonzRaster";
  case OVL_XSHLEVEL:
    return "Raster";
  default:
    return "Unknown";
  }
}

int Level::getFrameCount() const { return m_sl ? m_sl->getFrameCount() : 0; }

QString Level::getName() const {
  return m_sl ? QString::fromStdWString(m_sl->getName()) : "";
}

void Level::setName(const QString &name) {
  if (m_sl) {
    m_sl->setName(name.toStdWString());
  }
}

QJSValue Level::getPath() const {
  if (m_sl) {
    FilePath *result = new FilePath(m_sl->getPath());
    return result->create<FilePath>(engine());
  }
  return QJSValue();
}

void Level::setPath(const QJSValue &pathArg) {
  if (!m_sl) {
    throwError(tr("Cannot set path on empty level"));
    return;
  }

  TFilePath fp;
  FilePath *filePath = cast<FilePath>(pathArg);

  if (filePath) {
    fp = filePath->getToonzFilePath();
  } else if (pathArg.isString()) {
    fp = TFilePath(pathArg.toString().toStdString());
  } else {
    throwError(tr("Bad argument (%1). It should be FilePath or string")
                   .arg(pathArg.toString()));
    return;
  }

  m_sl->setPath(fp);
  try {
    m_sl->load();
  } catch (...) {
    throwError(tr("Exception loading level (%1)")
                   .arg(QString::fromStdWString(fp.getWideString())));
  }
}

QJSValue Level::load(const QJSValue &fpArg) {
  // Clean up existing level if any
  if (m_sl) {
    if (m_scene) {
      m_scene->getLevelSet()->removeLevel(m_sl, true);
    }
    m_sl->release();
    m_sl = nullptr;
  }

  // Get the path
  TFilePath fp;
  if (!checkFilePath(engine(), fpArg, fp)) {
    return QJSValue();
  }

  QString fpStr = fpArg.toString();

  try {
    if (!TSystem::doesExistFileOrLevel(fp)) {
      return throwError(tr("File %1 doesn't exist").arg(fpStr));
    }

    TFileType::Type fileType = TFileType::getInfo(fp);
    if (TFileType::isVector(fileType)) {
      m_type = PLI_XSHLEVEL;
    } else if (0 != (fileType & TFileType::CMAPPED_IMAGE)) {
      m_type = TZP_XSHLEVEL;
    } else if (0 != (fileType & TFileType::RASTER_IMAGE)) {
      m_type = OVL_XSHLEVEL;
    } else {
      return throwError(tr("File %1 is unsupported").arg(fpStr));
    }

    TXshLevel *xl = m_scene->loadLevel(fp);
    if (xl) {
      m_sl = xl->getSimpleLevel();
      m_sl->addRef();
    }

    return self();
  } catch (...) {
    return throwError(tr("Exception reading %1").arg(fpStr));
  }
}

QJSValue Level::save(const QJSValue &fpArg) {
  if (getFrameCount() == 0) {
    return throwError(tr("Can't save an empty level"));
  }

  // Get the path
  TFilePath fp;
  if (!checkFilePath(engine(), fpArg, fp)) {
    return QJSValue();
  }

  QString fpStr = fpArg.toString();

  // Handle conversion (if it is needed and possible)
  TFileType::Type fileType = TFileType::getInfo(fp);

  bool isCompatible = false;
  if (TFileType::isFullColor(fileType)) {
    if (m_sl->getType() == OVL_XSHLEVEL) isCompatible = true;
  } else if (TFileType::isVector(fileType)) {
    if (m_sl->getType() == PLI_XSHLEVEL) isCompatible = true;
  } else if (fileType & TFileType::CMAPPED_IMAGE) {
    if (m_sl->getType() == TZP_XSHLEVEL) isCompatible = true;
  } else {
    return throwError(tr("Unrecognized file type: %1").arg(fpStr));
  }

  if (!isCompatible) {
    return throwError(tr("Can't save a %1 level to this file type: %2")
                          .arg(getType(), fpStr));
  }

  try {
    m_sl->save(fp);
  } catch (TSystemException &se) {
    return throwError(tr("Exception writing %1")
                          .arg(QString::fromStdWString(se.getMessage())));
  }

  return self();
}

TFrameId Level::getFid(const QJSValue &arg, QString &err) {
  if (arg.isNumber() || arg.isString()) {
    QString s = arg.toString();
    QRegularExpression re(R"(^(-?\d+)(\w?)$)");
    QRegularExpressionMatch match = re.match(s);

    if (match.hasMatch()) {
      int d     = match.captured(1).toInt();
      QString c = match.captured(2);
      TFrameId fid;

      if (c.length() == 1) {
        fid = TFrameId(d, static_cast<wchar_t>(c[0].unicode()));
      } else {
        fid = TFrameId(d);
      }

      err.clear();
      return fid;
    }
  }

  err = QObject::tr("Argument '%1' does not look like a FrameId")
            .arg(arg.toString());
  return TFrameId();
}

TImageP Level::getImg(const TFrameId &fid) {
  return m_sl ? m_sl->getFrame(fid, false) : TImageP();
}

QJSValue Level::getFrame(const QJSValue &fidArg) {
  if (getFrameCount() == 0) {
    return throwError("An empty level has no frames");
  }

  QString err;
  TFrameId fid = getFid(fidArg, err);
  if (!err.isEmpty()) {
    return throwError(err);
  }

  TImageP content = m_sl->getFrame(fid, false);
  if (content) {
    Image *img = new Image(content.getPointer());
    return create(img);
  }

  return QJSValue();
}

QJSValue Level::getFrameByIndex(const QJSValue &indexArg) {
  if (getFrameCount() == 0) {
    return throwError("An empty level has no frames");
  }

  if (!indexArg.isNumber()) {
    return throwError(
        tr("frame index (%1) must be a number").arg(indexArg.toString()));
  }

  int index = indexArg.toInt();
  if (index < 0 || index >= getFrameCount()) {
    return throwError(tr("frame index (%1) is out of range (0-%2)")
                          .arg(index)
                          .arg(getFrameCount() - 1));
  }

  TFrameId fid    = m_sl->index2fid(index);
  TImageP content = m_sl->getFrame(fid, false);

  if (content) {
    Image *img = new Image(content.getPointer());
    return create(img);
  }

  return QJSValue();
}

QJSValue Level::setFrame(const QJSValue &fidArg, const QJSValue &imageArg) {
  QString err;
  TFrameId fid = getFid(fidArg, err);
  if (!err.isEmpty()) {
    return throwError(err);
  }

  Image *img = cast<Image>(imageArg);
  if (!img) {
    return throwError(
        tr("second argument (%1) is not an image").arg(imageArg.toString()));
  }

  QString imgType = img->getType();
  int levelType   = NO_XSHLEVEL;

  if (imgType == "ToonzRaster") {
    levelType = TZP_XSHLEVEL;
  } else if (imgType == "Raster") {
    levelType = OVL_XSHLEVEL;
  } else if (imgType == "Vector") {
    levelType = PLI_XSHLEVEL;
  } else {
    return throwError(tr("cannot insert a %1 image into a level").arg(imgType));
  }

  // Handle empty level case
  if (m_type == NO_XSHLEVEL) {
    m_type        = levelType;
    TXshLevel *xl = m_scene->createNewLevel(levelType);
    m_sl          = xl->getSimpleLevel();
    m_sl->addRef();
    m_sl->setPalette(img->getImg()->getPalette());

    if (levelType != PLI_XSHLEVEL) {
      LevelProperties *lprop = m_sl->getProperties();
      lprop->setDpiPolicy(LevelProperties::DP_ImageDpi);

      int xres   = img->getWidth();
      int yres   = img->getHeight();
      double dpi = img->getDpi();

      lprop->setDpi(dpi);
      lprop->setImageDpi(TPointD(dpi, dpi));
      lprop->setImageRes(TDimension(xres, yres));
    }
  } else if (m_type != levelType) {
    return throwError(
        tr("cannot insert a %1 image to a %2 level").arg(imgType, getType()));
  }

  if (m_sl->getFrameCount() == 0) {
    m_sl->setPalette(img->getImg()->getPalette());
  }

  m_sl->setFrame(fid, img->getImg());
  m_sl->setDirtyFlag(true);
  return self();
}

TFrameId Level::nextFrameId(const TXshSimpleLevel *sl) {
  if (!sl || sl->getFrameCount() == 0) return TFrameId(1);
  // the frames are sorted: the last one has the greatest number
  std::vector<TFrameId> fids;
  sl->getFids(fids);
  return TFrameId(fids.back().getNumber() + 1);
}

QJSValue Level::hasFrame(const QJSValue &fidArg) {
  QString err;
  TFrameId fid = getFid(fidArg, err);
  if (!err.isEmpty()) {
    return throwError(err);
  }
  return QJSValue(m_sl && m_sl->isFid(fid));
}

QJSValue Level::removeFrame(const QJSValue &fidArg) {
  QString err;
  TFrameId fid = getFid(fidArg, err);
  if (!err.isEmpty()) {
    return throwError(err);
  }
  if (m_sl && m_sl->isFid(fid)) {
    m_sl->eraseFrame(fid);
    m_sl->setDirtyFlag(true);
  }
  return self();
}

QJSValue Level::appendFrame(const QJSValue &imageArg) {
  TFrameId fid = nextFrameId(m_sl);
  // an error is thrown by setFrame(), which then returns undefined
  QJSValue result = setFrame(QJSValue(fid.getNumber()), imageArg);
  if (result.isUndefined()) {
    return result;
  }
  return QJSValue(QString::fromStdString(fid.expand()));
}

QJSValue Level::getFrameIds() {
  QList<TFrameId> fids;
  getFrameIds(fids);

  QJSValue result     = engine()->newArray();
  quint32 index       = 0;

  for (const TFrameId &fid : fids) {
    QString fidStr = QString::fromStdString(fid.expand());
    result.setProperty(index++, fidStr);
  }

  return result;
}

void Level::getFrameIds(QList<TFrameId> &result) {
  if (getFrameCount() > 0 && m_sl) {
    std::vector<TFrameId> fids;
    m_sl->getFids(fids);

    for (const TFrameId &fid : fids) {
      result.append(fid);
    }
  }
}

int Level::setFrame(const TFrameId &fid, const TImageP &img) {
  TImage::Type imgType = img->getType();
  int levelType        = NO_XSHLEVEL;

  if (imgType == TImage::TOONZ_RASTER) {
    levelType = TZP_XSHLEVEL;
  } else if (imgType == TImage::RASTER) {
    levelType = OVL_XSHLEVEL;
  } else if (imgType == TImage::VECTOR) {
    levelType = PLI_XSHLEVEL;
  } else {
    return -1;
  }

  // Handle empty level case
  if (m_type == NO_XSHLEVEL) {
    m_type        = levelType;
    TXshLevel *xl = m_scene->createNewLevel(levelType);
    m_sl          = xl->getSimpleLevel();
    m_sl->addRef();
    m_sl->setPalette(img->getPalette());

    if (levelType != PLI_XSHLEVEL) {
      LevelProperties *lprop = m_sl->getProperties();
      lprop->setDpiPolicy(LevelProperties::DP_ImageDpi);

      int xres = 0, yres = 0;
      double dpix = 0, dpiy = 0;

      if (TRasterImageP ri = img) {
        if (ri->getRaster()) {
          TDimension size = ri->getRaster()->getSize();
          xres            = size.lx;
          yres            = size.ly;
          ri->getDpi(dpix, dpiy);
        }
      } else if (TToonzImageP ti = img) {
        if (ti->getRaster()) {
          TDimension size = ti->getRaster()->getSize();
          xres            = size.lx;
          yres            = size.ly;
          ti->getDpi(dpix, dpiy);
        }
      }

      lprop->setDpi(dpix);
      lprop->setImageDpi(TPointD(dpix, dpiy));
      lprop->setImageRes(TDimension(xres, yres));
    }
  } else if (m_type != levelType) {
    return -2;
  }

  if (m_sl->getFrameCount() == 0) {
    m_sl->setPalette(img->getPalette());
  }

  m_sl->setFrame(fid, img);
  m_sl->setDirtyFlag(true);
  return 1;
}

}  // namespace TScriptBinding
