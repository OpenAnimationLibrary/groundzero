// The classes of the opentoonz Python module (built only with the CMake
// option WITH_PYTHON): Image, Scene, Level and StageObject, with the API of
// the JavaScript classes of the same name (see scriptbinding_image.h,
// scriptbinding_scene.h, scriptbinding_level.h and
// scriptbinding_stageobject.h), on the same toonzlib calls. As in the
// JavaScript binding, the scenes of the scripts aren't the one of the UI, and
// their changes are not undoable.

// must be the first include (see pythonbinding.h)
#include "pythonbinding.h"

// TnzLib includes
#include "toonz/scriptbinding_image.h"
#include "toonz/scriptbinding_level.h"
#include "toonz/scriptbinding_renderer.h"
#include "toonz/scriptbinding_scene.h"
#include "toonz/scriptbinding_stageobject.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshlevel.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/levelset.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectid.h"
#include "toonz/tstageobjecttree.h"

// TnzCore / TnzBase includes
#include "tdoubleparam.h"
#include "tfiletype.h"
#include "tfilepath.h"
#include "tsystem.h"
#include "traster.h"
#include "trasterimage.h"
#include "ttoonzimage.h"

// Qt includes
#include <QJSValue>
#include <QObject>
#include <QString>

#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace py = pybind11;

namespace PythonBinding {

namespace {

typedef std::shared_ptr<ToonzScene> ScenePtr;
typedef TScriptBinding::Image JsImage;
typedef TScriptBinding::Scene JsScene;
typedef TScriptBinding::Level JsLevel;
typedef TScriptBinding::StageObject JsStageObject;

std::string toStd(const QString &s) { return s.toStdString(); }

QString toQString(const py::handle &h) {
  return QString::fromStdString(py::str(h).cast<std::string>());
}

// the argument as the error messages show it
QString describe(const py::handle &h) {
  return QString::fromStdString(py::repr(h).cast<std::string>());
}

// Raises the Python exception type (e.g. PyExc_ValueError)
[[noreturn]] void raiseError(PyObject *type, const QString &message) {
  PyErr_SetString(type, message.toUtf8().constData());
  throw py::error_already_set();
}

// The text of a path argument: a str or an os.PathLike
QString getPathText(const py::handle &arg) {
  py::object path = py::reinterpret_borrow<py::object>(arg);
  if (!py::isinstance<py::str>(path) && py::hasattr(path, "__fspath__"))
    path = py::module_::import("os").attr("fspath")(path);
  if (!py::isinstance<py::str>(path))
    raiseError(PyExc_TypeError,
               QObject::tr("Argument doesn't look like a file path : %1")
                   .arg(describe(arg)));
  return toQString(path);
}

// A path of a scene: relative paths are in the scenes folder of the current
// project
TFilePath getScenePath(const QString &text) {
  TFilePath fp(text.toStdWString());
  if (!fp.isAbsolute())
    fp = TProjectManager::instance()->getCurrentProject()->getScenesPath() + fp;
  return fp;
}

bool doesExist(const TFilePath &fp) {
  try {
    return TSystem::doesExistFileOrLevel(fp);
  } catch (...) {
    return false;
  }
}

// A number (int or float, not bool): false if arg isn't one
bool getNumber(const py::handle &arg, double &value) {
  if (py::isinstance<py::bool_>(arg) ||
      !(PyFloat_Check(arg.ptr()) || PyIndex_Check(arg.ptr())))
    return false;
  value = PyFloat_AsDouble(arg.ptr());
  if (value == -1.0 && PyErr_Occurred()) {
    PyErr_Clear();
    return false;
  }
  return true;
}

// A finite number, else raises with the JavaScript binding message
double getFiniteNumber(const py::handle &arg, const QString &what) {
  double value  = 0;
  bool isNumber = getNumber(arg, value);
  if (!isNumber || !std::isfinite(value))
    raiseError(
        isNumber ? PyExc_ValueError : PyExc_TypeError,
        QObject::tr("Bad %1 (%2): expected a number").arg(what, describe(arg)));
  return value;
}

// A frame where a keyframe can be: an integer >= 0 (3.0 is accepted, as in
// JavaScript)
int getKeyframeFrame(const py::handle &arg) {
  double value  = 0;
  bool isNumber = getNumber(arg, value);
  if (isNumber && std::isfinite(value) && value >= 0 &&
      value == std::floor(value) && value <= std::numeric_limits<int>::max())
    return (int)value;
  raiseError(isNumber ? PyExc_ValueError : PyExc_TypeError,
             QObject::tr("Bad frame (%1): expected an integer >= 0")
                 .arg(describe(arg)));
}

// A channel name (case insensitive), as a TStageObject::Channel
TStageObject::Channel getChannel(const py::handle &arg) {
  int channel = 0;
  if (py::isinstance<py::str>(arg) &&
      JsStageObject::parseChannel(toQString(arg), channel))
    return (TStageObject::Channel)channel;
  raiseError(PyExc_ValueError,
             JsStageObject::badChannelMessage(py::isinstance<py::str>(arg)
                                                  ? toQString(arg)
                                                  : describe(arg)));
}

// A frame id: an int or a str such as "12" or "12a"
TFrameId getFid(const py::handle &arg, QString &err) {
  if (!py::isinstance<py::bool_>(arg) &&
      (py::isinstance<py::str>(arg) || py::isinstance<py::int_>(arg))) {
    // parsed by the JavaScript binding
    TFrameId fid = JsLevel::getFid(QJSValue(toQString(arg)), err);
    if (err.isEmpty()) return fid;
  }
  err = QObject::tr("Argument '%1' does not look like a FrameId")
            .arg(describe(arg));
  return TFrameId();
}

QString getIdName(const TStageObjectId &id) {
  return QString::fromStdString(id.toString());
}

//=============================================================================
// Image

class ImageWrapper {
public:
  TImageP m_img;

  ImageWrapper() {}
  explicit ImageWrapper(const TImageP &img) : m_img(img) {}

  // the JavaScript Image gives the same descriptions of the image
  QString toString() const { return JsImage(m_img).toString(); }
  QString getType() const { return JsImage::getTypeName(m_img); }
  int getWidth() const { return JsImage(m_img).getWidth(); }
  int getHeight() const { return JsImage(m_img).getHeight(); }
  double getDpi() const { return JsImage(m_img).getDpi(); }

  void setDpi(double dpi) {
    QString err = JsImage::changeDpi(m_img, dpi);
    if (!err.isEmpty()) raiseError(PyExc_ValueError, err);
  }

  static QColor getColor(const py::handle &arg) {
    QColor color;
    QString name = toQString(arg);
    color.setNamedColor(name);
    if (!color.isValid())
      raiseError(PyExc_ValueError,
                 QObject::tr("%1 is not a valid color (valid color names are "
                             "'red', 'transparent', '#FF8800', etc.)")
                     .arg(name));
    return color;
  }

  std::string getPixel(int x, int y) const {
    QColor color;
    QString err = JsImage::readPixel(m_img, x, y, color);
    if (!err.isEmpty()) raiseError(PyExc_ValueError, err);
    return toStd(color.name(QColor::HexArgb));
  }

  void setPixel(int x, int y, const py::handle &colorArg) {
    QColor color = getColor(colorArg);
    QString err  = JsImage::writePixel(m_img, x, y, color);
    if (!err.isEmpty()) raiseError(PyExc_ValueError, err);
  }

  void fill(const py::handle &colorArg) {
    QColor color = getColor(colorArg);
    QString err  = JsImage::fillImage(m_img, color);
    if (!err.isEmpty()) raiseError(PyExc_ValueError, err);
  }

  ImageWrapper clone() const {
    if (!m_img)
      raiseError(PyExc_RuntimeError, JsImage::tr("Can't clone an empty image"));
    return ImageWrapper(TImageP(m_img->cloneImage()));
  }

  void load(const py::handle &arg) {
    // as Image.load() of the JavaScript binding
    m_img         = TImageP();
    QString fpStr = getPathText(arg);
    TFilePath fp(fpStr.toStdWString());
    if (!doesExist(fp))
      raiseError(PyExc_FileNotFoundError,
                 JsImage::tr("File %1 doesn't exist").arg(fpStr));
    QString warningText;
    QString err = JsImage::loadImage(fp, fpStr, m_img, warningText);
    if (!err.isEmpty()) raiseError(PyExc_OSError, err);
    if (!warningText.isEmpty() &&
        PyErr_WarnEx(PyExc_UserWarning, warningText.toUtf8().constData(), 1) <
            0)
      throw py::error_already_set();
  }

  void save(const py::handle &arg) {
    if (!m_img)
      raiseError(PyExc_RuntimeError, JsImage::tr("Can't save an empty image"));
    QString fpStr = getPathText(arg);
    TFilePath fp(fpStr.toStdWString());
    QString err = JsImage::saveImage(m_img, fp, fpStr);
    if (!err.isEmpty()) raiseError(PyExc_OSError, err);
  }

  // an empty (transparent) Raster or ToonzRaster image
  static ImageWrapper create(int width, int height, const QString &type) {
    if (width <= 0 || height <= 0)
      raiseError(PyExc_ValueError,
                 QString("Bad size (%1 x %2)").arg(width).arg(height));
    if (type == "Raster") {
      TRaster32P ras(width, height);
      ras->clear();
      return ImageWrapper(TRasterImageP(ras));
    } else if (type == "ToonzRaster") {
      TRasterCM32P ras(width, height);
      ras->clear();
      return ImageWrapper(
          TToonzImageP(ras, TRect(0, 0, width - 1, height - 1)));
    }
    raiseError(PyExc_ValueError,
               JsImage::tr("Bad image type (%1): should be 'Raster' or "
                           "'ToonzRaster'")
                   .arg(type));
  }
};

//=============================================================================
// Level

class LevelWrapper {
public:
  // the scene of the level (only the level holds it if the level was created
  // by Level()). Declared first: the level is released before its scene
  ScenePtr m_scene;
  TXshSimpleLevelP m_sl;
  int m_type;

  LevelWrapper()
      : m_scene(std::make_shared<ToonzScene>()), m_type(NO_XSHLEVEL) {}
  LevelWrapper(const ScenePtr &scene, TXshSimpleLevel *sl)
      : m_scene(scene), m_sl(sl), m_type(sl->getType()) {}

  TXshSimpleLevel *getSimpleLevel() const { return m_sl.getPointer(); }

  QString toString() const {
    QString info = "(";
    QString comma;
    if (getName() != "") {
      info.append(comma).append(getName());
      comma = ", ";
    }
    info.append(comma).append(JsLevel::tr("%1 frames").arg(getFrameCount()));
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

  QString getType() const {
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

  int getFrameCount() const {
    return getSimpleLevel() ? getSimpleLevel()->getFrameCount() : 0;
  }

  QString getName() const {
    return getSimpleLevel()
               ? QString::fromStdWString(getSimpleLevel()->getName())
               : "";
  }

  void setName(const QString &name) {
    if (getSimpleLevel()) getSimpleLevel()->setName(name.toStdWString());
  }

  py::object getPath() const {
    if (!getSimpleLevel()) return py::none();
    return py::str(
        QString::fromStdWString(getSimpleLevel()->getPath().getWideString())
            .toStdString());
  }

  py::list getFrameIds() const {
    py::list result;
    if (getFrameCount() > 0) {
      std::vector<TFrameId> fids;
      getSimpleLevel()->getFids(fids);
      for (const TFrameId &fid : fids) result.append(py::str(fid.expand()));
    }
    return result;
  }

  void load(const py::handle &arg) {
    // as Level.load() of the JavaScript binding
    if (TXshSimpleLevel *sl = getSimpleLevel()) {
      TLevelSet *levelSet = m_scene->getLevelSet();
      if (levelSet->getLevel(sl->getName()) == sl)
        levelSet->removeLevel(sl, true);
      m_sl   = TXshSimpleLevelP();
      m_type = NO_XSHLEVEL;
    }

    QString fpStr = getPathText(arg);
    TFilePath fp(fpStr.toStdWString());
    if (!doesExist(fp))
      raiseError(PyExc_FileNotFoundError,
                 JsLevel::tr("File %1 doesn't exist").arg(fpStr));

    TFileType::Type fileType = TFileType::getInfo(fp);
    int type                 = NO_XSHLEVEL;
    if (TFileType::isVector(fileType))
      type = PLI_XSHLEVEL;
    else if (0 != (fileType & TFileType::CMAPPED_IMAGE))
      type = TZP_XSHLEVEL;
    else if (0 != (fileType & TFileType::RASTER_IMAGE))
      type = OVL_XSHLEVEL;
    else
      raiseError(PyExc_ValueError,
                 JsLevel::tr("File %1 is unsupported").arg(fpStr));

    TXshLevel *xl = 0;
    try {
      xl = m_scene->loadLevel(fp);
    } catch (...) {
      raiseError(PyExc_OSError, JsLevel::tr("Exception reading %1").arg(fpStr));
    }
    m_type = type;
    if (xl && xl->getSimpleLevel()) m_sl = xl->getSimpleLevel();
  }

  void save(const py::handle &arg) {
    if (getFrameCount() == 0)
      raiseError(PyExc_RuntimeError, JsLevel::tr("Can't save an empty level"));

    QString fpStr = getPathText(arg);
    TFilePath fp(fpStr.toStdWString());

    TFileType::Type fileType = TFileType::getInfo(fp);
    int levelType            = getSimpleLevel()->getType();
    bool isCompatible        = false;
    if (TFileType::isFullColor(fileType))
      isCompatible = levelType == OVL_XSHLEVEL;
    else if (TFileType::isVector(fileType))
      isCompatible = levelType == PLI_XSHLEVEL;
    else if (fileType & TFileType::CMAPPED_IMAGE)
      isCompatible = levelType == TZP_XSHLEVEL;
    else
      raiseError(PyExc_ValueError,
                 JsLevel::tr("Unrecognized file type: %1").arg(fpStr));
    if (!isCompatible)
      raiseError(PyExc_ValueError,
                 JsLevel::tr("Can't save a %1 level to this file type: %2")
                     .arg(getType(), fpStr));

    QString err;
    try {
      getSimpleLevel()->save(fp);
    } catch (TSystemException &se) {
      err = QString::fromStdWString(se.getMessage());
    } catch (...) {
      err = fpStr;
    }
    if (!err.isEmpty())
      raiseError(PyExc_OSError, JsLevel::tr("Exception writing %1").arg(err));
  }

  // The image of a frame (the image of the level itself: after changing it,
  // setFrame() marks the level as modified)
  py::object getFrame(const py::handle &fidArg) {
    if (getFrameCount() == 0)
      raiseError(PyExc_RuntimeError, "An empty level has no frames");
    QString err;
    TFrameId fid = getFid(fidArg, err);
    if (!err.isEmpty()) raiseError(PyExc_ValueError, err);
    TImageP img = getSimpleLevel()->getFrame(fid, false);
    if (!img) return py::none();
    return py::cast(ImageWrapper(img));
  }

  py::object getFrameByIndex(int index) {
    if (getFrameCount() == 0)
      raiseError(PyExc_RuntimeError, "An empty level has no frames");
    if (index < 0 || index >= getFrameCount())
      raiseError(PyExc_IndexError,
                 JsLevel::tr("frame index (%1) is out of range (0-%2)")
                     .arg(index)
                     .arg(getFrameCount() - 1));
    TImageP img =
        getSimpleLevel()->getFrame(getSimpleLevel()->index2fid(index), false);
    if (!img) return py::none();
    return py::cast(ImageWrapper(img));
  }

  // as Level.setFrame() of the JavaScript binding, but the level can't be
  // empty (it has to belong to a scene: see Scene.newLevel())
  void setFrame(const py::handle &fidArg, const py::handle &imageArg) {
    QString err;
    TFrameId fid = getFid(fidArg, err);
    if (!err.isEmpty()) raiseError(PyExc_ValueError, err);
    if (!py::isinstance<ImageWrapper>(imageArg))
      raiseError(PyExc_TypeError,
                 JsLevel::tr("second argument (%1) is not an image")
                     .arg(describe(imageArg)));
    TImageP img = imageArg.cast<ImageWrapper &>().m_img;
    if (!img) raiseError(PyExc_ValueError, "Can't insert an empty image");

    QString imgType = JsImage::getTypeName(img);
    int levelType   = NO_XSHLEVEL;
    if (imgType == "ToonzRaster")
      levelType = TZP_XSHLEVEL;
    else if (imgType == "Raster")
      levelType = OVL_XSHLEVEL;
    else if (imgType == "Vector")
      levelType = PLI_XSHLEVEL;
    else
      raiseError(
          PyExc_ValueError,
          JsLevel::tr("cannot insert a %1 image into a level").arg(imgType));
    TXshSimpleLevel *sl = getSimpleLevel();
    if (!sl)
      raiseError(PyExc_RuntimeError,
                 "Can't add frames to an empty level: create the level with "
                 "Scene.newLevel()");
    if (m_type != levelType)
      raiseError(PyExc_ValueError,
                 JsLevel::tr("cannot insert a %1 image to a %2 level")
                     .arg(imgType, getType()));

    if (sl->getFrameCount() == 0) sl->setPalette(img->getPalette());
    sl->setFrame(fid, img);
    sl->setDirtyFlag(true);
  }

  bool hasFrame(const py::handle &fidArg) const {
    QString err;
    TFrameId fid = getFid(fidArg, err);
    if (!err.isEmpty()) raiseError(PyExc_ValueError, err);
    return getSimpleLevel() && getSimpleLevel()->isFid(fid);
  }

  // does nothing if the level has not the frame; the cells of the scenes
  // that use it are not changed
  void removeFrame(const py::handle &fidArg) {
    QString err;
    TFrameId fid = getFid(fidArg, err);
    if (!err.isEmpty()) raiseError(PyExc_ValueError, err);
    if (getSimpleLevel() && getSimpleLevel()->isFid(fid)) {
      getSimpleLevel()->eraseFrame(fid);
      getSimpleLevel()->setDirtyFlag(true);
    }
  }

  // returns the id of the new frame
  std::string appendFrame(const py::handle &imageArg) {
    TFrameId fid = JsLevel::nextFrameId(getSimpleLevel());
    setFrame(py::int_(fid.getNumber()), imageArg);
    return fid.expand();
  }
};

//=============================================================================
// StageObject

class StageObjectWrapper {
public:
  // the scene, which the object keeps alive. Declared first: the stage
  // object is released before its scene
  ScenePtr m_scene;
  TStageObjectP m_obj;

  StageObjectWrapper(const ScenePtr &scene, TStageObject *obj)
      : m_scene(scene), m_obj(obj) {}

  TXsheet *getXsheet() const { return m_scene->getXsheet(); }

  // the object is removed from the tree when its column is deleted, and the
  // tree is replaced when the scene is loaded
  bool isInScene() const {
    TStageObjectTree *tree = getXsheet()->getStageObjectTree();
    return tree->getStageObject(m_obj->getId(), false) == m_obj.getPointer();
  }

  // The wrapped object; raises if it is no longer in the scene
  TStageObject *get() const {
    if (!isInScene())
      raiseError(
          PyExc_RuntimeError,
          JsStageObject::tr("The stage object %1 is no longer in the scene")
              .arg(getIdName(m_obj->getId())));
    return m_obj.getPointer();
  }

  QString toString() const {
    if (!isInScene()) return JsStageObject::tr("StageObject (removed)");
    if (m_obj->hasSpecifiedName())
      return QString("StageObject(%1: %2)")
          .arg(getIdName(m_obj->getId()),
               QString::fromStdString(m_obj->getName()));
    return QString("StageObject(%1)").arg(getIdName(m_obj->getId()));
  }

  QString getParentName() const {
    TStageObject *obj       = get();
    TStageObjectId parentId = obj->getParent();
    if (parentId == TStageObjectId::NoneId) return "";
    TStageObject *parent =
        getXsheet()->getStageObjectTree()->getStageObject(parentId, false);
    return parent ? QString::fromStdString(parent->getName())
                  : getIdName(parentId);
  }

  void setParentName(const QString &name) {
    QString err = JsStageObject::linkToParent(getXsheet(), get(), name);
    if (!err.isEmpty()) raiseError(PyExc_ValueError, err);
  }

  double getValue(const py::handle &channelArg, const py::handle &frameArg) {
    TStageObject::Channel channel = getChannel(channelArg);
    double frame = getFiniteNumber(frameArg, JsStageObject::tr("frame"));
    return get()->getParam(channel)->getValue(frame);
  }

  void setKeyframe(const py::handle &channelArg, const py::handle &frameArg,
                   const py::handle &valueArg) {
    TStageObject::Channel channel = getChannel(channelArg);
    int frame                     = getKeyframeFrame(frameArg);
    double value = getFiniteNumber(valueArg, JsStageObject::tr("value"));
    JsStageObject::doSetKeyframe(get(), channel, frame, value);
  }

  void removeKeyframe(const py::handle &channelArg,
                      const py::handle &frameArg) {
    TStageObject::Channel channel = getChannel(channelArg);
    int frame                     = getKeyframeFrame(frameArg);
    JsStageObject::doRemoveKeyframe(get(), channel, frame);
  }

  bool isKeyframe(const py::handle &channelArg, const py::handle &frameArg) {
    TStageObject::Channel channel = getChannel(channelArg);
    int frame                     = getKeyframeFrame(frameArg);
    return get()->getParam(channel)->isKeyframe(frame);
  }

  // the key frames, as ints (floats if not integer)
  py::list getKeyframes(const py::handle &channelArg) {
    TStageObject::Channel channel = getChannel(channelArg);
    std::set<double> frames;
    get()->getParam(channel)->getKeyframes(frames);
    py::list result;
    for (double frame : frames) {
      if (frame == std::floor(frame) &&
          std::abs(frame) <= std::numeric_limits<int>::max())
        result.append(py::int_((int)frame));
      else
        result.append(py::float_(frame));
    }
    return result;
  }

  py::tuple getCenter(const py::handle &frameArg) {
    int frame      = getKeyframeFrame(frameArg);
    TPointD center = get()->getCenter(frame);
    return py::make_tuple(center.x, center.y);
  }

  void setCenter(const py::handle &frameArg, const py::handle &xArg,
                 const py::handle &yArg) {
    int frame = getKeyframeFrame(frameArg);
    double x  = getFiniteNumber(xArg, "x");
    double y  = getFiniteNumber(yArg, "y");
    // as the center fields of the Animate tool, without undo
    get()->setCenter(frame, TPointD(x, y));
  }
};

//=============================================================================
// Scene

class SceneWrapper {
public:
  ScenePtr m_scene;

  SceneWrapper() : m_scene(std::make_shared<ToonzScene>()) {
    TProjectManager::instance()->initializeScene(m_scene.get());
  }

  TXsheet *getXsheet() const { return m_scene->getXsheet(); }

  QString toString() const {
    return QString("Scene (%1 frames)").arg(m_scene->getFrameCount());
  }

  void load(const py::handle &arg) {
    QString fpStr = getPathText(arg);
    TFilePath fp  = getScenePath(fpStr);
    if (!doesExist(fp))
      raiseError(PyExc_FileNotFoundError,
                 JsScene::tr("File %1 doesn't exist").arg(fpStr));
    bool ok = true;
    try {
      m_scene->load(fp);
    } catch (...) {
      ok = false;
    }
    if (!ok)
      raiseError(PyExc_OSError, JsScene::tr("Exception reading %1").arg(fpStr));
  }

  void save(const py::handle &arg) {
    QString fpStr = getPathText(arg);
    TFilePath fp  = getScenePath(fpStr);
    bool ok       = true;
    try {
      m_scene->save(fp);
    } catch (...) {
      ok = false;
    }
    if (!ok)
      raiseError(PyExc_OSError, JsScene::tr("Exception writing %1").arg(fpStr));
  }

  py::list getLevels() const {
    py::list result;
    std::vector<TXshLevel *> levels;
    m_scene->getLevelSet()->listLevels(levels);
    for (TXshLevel *xl : levels)
      if (TXshSimpleLevel *sl = xl->getSimpleLevel())
        result.append(py::cast(LevelWrapper(m_scene, sl)));
    return result;
  }

  py::object getLevel(const QString &name) const {
    TXshLevel *xl       = m_scene->getLevelSet()->getLevel(name.toStdWString());
    TXshSimpleLevel *sl = xl ? xl->getSimpleLevel() : 0;
    if (!sl) return py::none();
    return py::cast(LevelWrapper(m_scene, sl));
  }

  LevelWrapper newLevel(const QString &levelTypeStr, const QString &levelName) {
    int levelType = NO_XSHLEVEL;
    if (levelTypeStr == "Vector")
      levelType = PLI_XSHLEVEL;
    else if (levelTypeStr == "ToonzRaster")
      levelType = TZP_XSHLEVEL;
    else if (levelTypeStr == "Raster")
      levelType = OVL_XSHLEVEL;
    if (levelType == NO_XSHLEVEL)
      raiseError(PyExc_ValueError,
                 JsScene::tr("Bad level type (%1): must be Vector,Raster or "
                             "ToonzRaster")
                     .arg(levelTypeStr));
    if (m_scene->getLevelSet()->hasLevel(levelName.toStdWString()))
      raiseError(PyExc_ValueError,
                 JsScene::tr("Can't add the level: name(%1) is already used")
                     .arg(levelName));

    TXshLevel *xl =
        m_scene->createNewLevel(levelType, levelName.toStdWString());
    if (!xl || !xl->getSimpleLevel())
      raiseError(PyExc_RuntimeError,
                 QString("Can't create the level %1").arg(levelName));
    xl->getSimpleLevel()->setDirtyFlag(true);
    return LevelWrapper(m_scene, xl->getSimpleLevel());
  }

  LevelWrapper loadLevel(const QString &levelName, const py::handle &arg) {
    if (m_scene->getLevelSet()->hasLevel(levelName.toStdWString()))
      raiseError(PyExc_ValueError,
                 JsScene::tr("Can't add the level: name(%1) is already used")
                     .arg(levelName));
    QString fpStr = getPathText(arg);
    TFilePath fp(fpStr.toStdWString());
    TFileType::Type type = TFileType::getInfo(fp);
    if ((type & TFileType::VIEWABLE) == 0)
      raiseError(PyExc_ValueError,
                 JsScene::tr("Can't load this kind of file as a level : %1")
                     .arg(fpStr));
    TXshLevel *xl = 0;
    try {
      xl = m_scene->loadLevel(fp);
    } catch (...) {
      xl = 0;
    }
    if (!xl || !xl->getSimpleLevel())
      raiseError(PyExc_OSError,
                 JsScene::tr("Could not load level %1").arg(fpStr));
    return LevelWrapper(m_scene, xl->getSimpleLevel());
  }

  // as Scene::doSetCell() of the JavaScript binding: returns the error
  QString doSetCell(int row, int col, const py::handle &levelArg,
                    const py::handle &fidArg) {
    if (row < 0 || col < 0) return "Bad row/col values";

    QString err;
    TXshCell cell;
    cell.m_frameId = getFid(fidArg, err);
    if (err != "") return err;
    if (py::isinstance<LevelWrapper>(levelArg)) {
      TXshSimpleLevel *sl = levelArg.cast<LevelWrapper &>().getSimpleLevel();
      TXshLevel *xl = sl ? m_scene->getLevelSet()->getLevel(sl->getName()) : 0;
      if (!xl || xl->getSimpleLevel() != sl)
        return JsScene::tr("Level is not included in the scene : %1")
            .arg(toQString(levelArg));
      cell.m_level = sl;
    } else if (py::isinstance<py::str>(levelArg)) {
      QString levelName = toQString(levelArg);
      TXshLevel *xl =
          m_scene->getLevelSet()->getLevel(levelName.toStdWString());
      if (!xl)
        return JsScene::tr("Level '%1' is not included in the scene")
            .arg(levelName);
      cell.m_level = xl;
    } else
      return JsScene::tr("%1 : Expected a Level instance or a level name")
          .arg(describe(levelArg));
    getXsheet()->setCell(row, col, cell);
    return "";
  }

  void setCell(int row, int col, const py::handle &levelArg,
               const py::handle &fidArg) {
    QString err = doSetCell(row, col, levelArg, fidArg);
    if (err != "") raiseError(PyExc_ValueError, err);
  }

  // cell: None (clears the cell) or a dict with the keys "level" and "fid",
  // as getCell() returns
  void setCell(int row, int col, const py::handle &cellArg) {
    if (cellArg.is_none()) {
      if (row >= 0 && col >= 0) getXsheet()->setCell(row, col, TXshCell());
      return;
    }
    if (!py::isinstance<py::dict>(cellArg) ||
        !cellArg.cast<py::dict>().contains("level") ||
        !cellArg.cast<py::dict>().contains("fid"))
      raiseError(
          PyExc_TypeError,
          "Third argument should be None or a dict with the keys 'level' "
          "and 'fid'");
    py::dict cell    = cellArg.cast<py::dict>();
    py::object level = cell["level"];
    py::object fid   = cell["fid"];
    setCell(row, col, level, fid);
  }

  py::object getCell(int row, int col) {
    if (row < 0 || col < 0) return py::none();
    TXshCell cell       = getXsheet()->getCell(row, col);
    TXshSimpleLevel *sl = cell.getSimpleLevel();
    if (!sl) return py::none();
    py::dict result;
    result["level"] = py::cast(LevelWrapper(m_scene, sl));
    if (cell.m_frameId.getLetter().isEmpty())
      result["fid"] = py::int_(cell.m_frameId.getNumber());
    else
      result["fid"] = py::str(cell.m_frameId.expand());
    return result;
  }

  void insertColumn(int col) {
    if (col < 0)
      raiseError(PyExc_ValueError, QString("Bad column (%1)").arg(col));
    getXsheet()->insertColumn(col);
  }

  void deleteColumn(int col) {
    if (col < 0)
      raiseError(PyExc_ValueError, QString("Bad column (%1)").arg(col));
    getXsheet()->removeColumn(col);
  }

  py::list getColumns() const {
    py::list result;
    for (int i = 0; i < getXsheet()->getColumnCount(); ++i) {
      JsScene::ColumnInfo info = JsScene::getColumnInfo(m_scene.get(), i);
      py::dict column;
      column["index"]    = py::int_(info.m_index);
      column["id"]       = py::str(toStd(info.m_id));
      column["name"]     = py::str(toStd(info.m_name));
      column["type"]     = py::str(toStd(info.m_type));
      column["firstRow"] = py::int_(info.m_firstRow);
      column["lastRow"]  = py::int_(info.m_lastRow);
      result.append(column);
    }
    return result;
  }

  ImageWrapper renderFrame(int frame) {
    int count = m_scene->getFrameCount();
    if (frame < 0 || frame >= count)
      raiseError(PyExc_ValueError,
                 JsScene::tr("Frame %1 is out of range: the scene has %2 "
                             "frames (the first is 0)")
                     .arg(frame)
                     .arg(count));
    TImageP img =
        TScriptBinding::Renderer::renderFrameImage(m_scene.get(), frame);
    if (!img) raiseError(PyExc_RuntimeError, JsScene::tr("Render failed"));
    return ImageWrapper(img);
  }

  StageObjectWrapper getStageObject(const QString &name) {
    TStageObject *obj = JsStageObject::findStageObject(getXsheet(), name);
    if (!obj)
      raiseError(PyExc_LookupError,
                 JsScene::tr("Stage object not found: %1").arg(name));
    return StageObjectWrapper(m_scene, obj);
  }

  py::list getStageObjects() {
    std::vector<TStageObject *> objs;
    JsStageObject::listStageObjects(getXsheet(), objs);
    py::list result;
    for (TStageObject *obj : objs)
      result.append(py::cast(StageObjectWrapper(m_scene, obj)));
    return result;
  }

  StageObjectWrapper newPegbar() {
    // as TStageObjectCmd::addNewPegbar(), without undo
    TStageObjectTree *tree = getXsheet()->getStageObjectTree();
    int index              = 0;
    while (tree->getStageObject(TStageObjectId::PegbarId(index), false))
      index++;
    TStageObject *obj =
        tree->getStageObject(TStageObjectId::PegbarId(index), true);
    return StageObjectWrapper(m_scene, obj);
  }
};

// A method returning its object, to allow chaining as in JavaScript (e.g.
// scene.load(path).save(otherPath))
template <class T, class... Args>
std::function<py::object(py::object, Args...)> chain(
    void (T::*method)(Args...)) {
  return [method](py::object self, Args... args) {
    (self.cast<T &>().*method)(args...);
    return self;
  };
}

}  // namespace

//=============================================================================

void bindClasses(py::module_ &m) {
  typedef const py::handle &Arg;

  py::class_<ImageWrapper>(
      m, "Image",
      "An image: Image() is empty, Image(path) loads the file, "
      "Image.create(width, height) makes a transparent Raster image. The "
      "images of a Level are given by Level.getFrame() and "
      "getFrameByIndex(), the ones of a rendering by Scene.renderFrame(). "
      "The pixels are in the Raster images only, with the colors as strings "
      "(\"red\", \"#ff8800\", and \"#AARRGGBB\" when alpha is not 255); "
      "(0, 0) is the lower left corner.")
      .def(py::init([](const py::object &path) {
             std::unique_ptr<ImageWrapper> image(new ImageWrapper());
             if (!path.is_none()) image->load(path);
             return image;
           }),
           py::arg("path") = py::none())
      .def_static(
          "create",
          [](int width, int height, const std::string &type) {
            return ImageWrapper::create(width, height,
                                        QString::fromStdString(type));
          },
          py::arg("width"), py::arg("height"), py::arg("type") = "Raster",
          "A transparent image (type: Raster or ToonzRaster)")
      .def("__str__", [](const ImageWrapper &i) { return toStd(i.toString()); })
      .def(
          "__repr__",
          [](const ImageWrapper &i) { return toStd("<" + i.toString() + ">"); })
      .def_property_readonly(
          "type", [](const ImageWrapper &i) { return toStd(i.getType()); },
          "Empty, Vector, ToonzRaster or Raster")
      .def_property_readonly("width", &ImageWrapper::getWidth)
      .def_property_readonly("height", &ImageWrapper::getHeight)
      .def_property("dpi", &ImageWrapper::getDpi, &ImageWrapper::setDpi,
                    "The dpi of the Raster and ToonzRaster images")
      .def("getPixel", &ImageWrapper::getPixel, py::arg("x"), py::arg("y"),
           "The color of a pixel of a Raster image, as a string")
      .def(
          "setPixel",
          [](py::object self, int x, int y, Arg color) {
            self.cast<ImageWrapper &>().setPixel(x, y, color);
            return self;
          },
          py::arg("x"), py::arg("y"), py::arg("color"),
          "Sets the color of a pixel of a Raster image; returns the image")
      .def("fill", chain(&ImageWrapper::fill), py::arg("color"),
           "Fills a Raster image with the color; returns the image")
      .def("clone", &ImageWrapper::clone, "A copy of the image")
      .def("load", chain(&ImageWrapper::load), py::arg("path"),
           "Loads the image file; returns the image")
      .def("save", chain(&ImageWrapper::save), py::arg("path"),
           "Saves the image to a file of a compatible type; returns the "
           "image");

  py::class_<LevelWrapper>(
      m, "Level",
      "A level: Level() is empty, Level(path) loads the file. The levels of a "
      "Scene are given by Scene.getLevels(), getLevel(), newLevel(), "
      "loadLevel() and getCell().")
      .def(py::init([](const py::object &path) {
             std::unique_ptr<LevelWrapper> level(new LevelWrapper());
             if (!path.is_none()) level->load(path);
             return level;
           }),
           py::arg("path") = py::none())
      .def("__str__", [](const LevelWrapper &l) { return toStd(l.toString()); })
      .def(
          "__repr__",
          [](const LevelWrapper &l) { return toStd("<" + l.toString() + ">"); })
      .def_property_readonly(
          "type", [](const LevelWrapper &l) { return toStd(l.getType()); },
          "Empty, Vector, ToonzRaster or Raster")
      .def_property_readonly("frameCount", &LevelWrapper::getFrameCount)
      .def_property(
          "name", [](const LevelWrapper &l) { return toStd(l.getName()); },
          [](LevelWrapper &l, const std::string &name) {
            l.setName(QString::fromStdString(name));
          })
      .def_property_readonly("path", &LevelWrapper::getPath,
                             "The path of the level file (None if empty)")
      .def("getFrameIds", &LevelWrapper::getFrameIds,
           "The frame ids, as strings")
      .def("getFrame", &LevelWrapper::getFrame, py::arg("fid"),
           "The image of the frame (e.g. 1 or \"1a\"), or None. After "
           "changing it, give it to setFrame()")
      .def("getFrameByIndex", &LevelWrapper::getFrameByIndex, py::arg("index"),
           "The image of the frame at the index (from 0)")
      .def("setFrame", chain(&LevelWrapper::setFrame), py::arg("fid"),
           py::arg("image"),
           "Sets the image of a frame, which is added if the level has not "
           "it; returns the level")
      .def("hasFrame", &LevelWrapper::hasFrame, py::arg("fid"))
      .def("removeFrame", chain(&LevelWrapper::removeFrame), py::arg("fid"),
           "Removes the frame, if the level has it; returns the level")
      .def("appendFrame", &LevelWrapper::appendFrame, py::arg("image"),
           "Adds the image after the last frame; returns the frame id")
      .def("load", chain(&LevelWrapper::load), py::arg("path"),
           "Loads the level file; returns the level")
      .def("save", chain(&LevelWrapper::save), py::arg("path"),
           "Saves the level to a file of a compatible type; returns the "
           "level");

  py::class_<StageObjectWrapper>(
      m, "StageObject",
      "A column, pegbar, camera or the table of a Scene, with its animation "
      "channels (x, y, z, so, angle, scalex, scaley, scale, shearx, sheary, "
      "path, rotx, roty), in internal units (inches, degrees, factors). "
      "Frames are xsheet rows numbered from 0. The changes are not undoable.")
      .def("__str__",
           [](const StageObjectWrapper &o) { return toStd(o.toString()); })
      .def("__repr__",
           [](const StageObjectWrapper &o) {
             return toStd("<" + o.toString() + ">");
           })
      .def("__eq__",
           [](const StageObjectWrapper &o, const py::object &other) {
             return py::isinstance<StageObjectWrapper>(other) &&
                    other.cast<const StageObjectWrapper &>()
                            .m_obj.getPointer() == o.m_obj.getPointer();
           })
      .def("__hash__",
           [](const StageObjectWrapper &o) {
             return std::hash<const void *>()(o.m_obj.getPointer());
           })
      .def_property_readonly(
          "id",
          [](const StageObjectWrapper &o) {
            return toStd(getIdName(o.get()->getId()));
          },
          "Col1, Peg1, Camera1 or Table")
      .def_property_readonly(
          "type",
          [](const StageObjectWrapper &o) {
            return toStd(JsStageObject::getTypeName(o.get()));
          },
          "Column, Pegbar, Camera or Table")
      .def_property(
          "name",
          [](const StageObjectWrapper &o) { return o.get()->getName(); },
          [](StageObjectWrapper &o, const std::string &name) {
            // as TStageObjectCmd::rename(), without undo
            o.get()->setName(name);
          },
          "The name given by the user (the id if none); \"\" restores the "
          "default")
      .def_property(
          "parent",
          [](const StageObjectWrapper &o) { return toStd(o.getParentName()); },
          [](StageObjectWrapper &o, const std::string &name) {
            o.setParentName(QString::fromStdString(name));
          },
          "The name of the parent (\"\" if none); assigning \"\" links "
          "columns and pegbars to the table")
      .def("getValue", &StageObjectWrapper::getValue, py::arg("channel"),
           py::arg("frame"), "The value of the channel at the frame")
      .def("setKeyframe", chain(&StageObjectWrapper::setKeyframe),
           py::arg("channel"), py::arg("frame"), py::arg("value"),
           "Sets a key frame (frame: int >= 0); returns the object")
      .def("setValue", chain(&StageObjectWrapper::setKeyframe),
           py::arg("channel"), py::arg("frame"), py::arg("value"),
           "Same as setKeyframe()")
      .def("removeKeyframe", chain(&StageObjectWrapper::removeKeyframe),
           py::arg("channel"), py::arg("frame"),
           "Removes the key frame, if any; returns the object")
      .def("isKeyframe", &StageObjectWrapper::isKeyframe, py::arg("channel"),
           py::arg("frame"))
      .def("getKeyframes", &StageObjectWrapper::getKeyframes,
           py::arg("channel"), "The frames of the keys of the channel")
      .def("getCenter", &StageObjectWrapper::getCenter, py::arg("frame"),
           "The center (in inches) as a tuple (x, y)")
      .def("setCenter", chain(&StageObjectWrapper::setCenter), py::arg("frame"),
           py::arg("x"), py::arg("y"),
           "Sets the center (in inches); returns the object");

  py::class_<SceneWrapper>(
      m, "Scene",
      "A scene: Scene() is a new scene, Scene(path) loads the file (relative "
      "paths are in the scenes folder of the current project). It isn't the "
      "scene open in the UI, and its changes are not undoable.")
      .def(py::init([](const py::object &path) {
             std::unique_ptr<SceneWrapper> scene(new SceneWrapper());
             if (!path.is_none()) scene->load(path);
             return scene;
           }),
           py::arg("path") = py::none())
      .def("__str__", [](const SceneWrapper &s) { return toStd(s.toString()); })
      .def(
          "__repr__",
          [](const SceneWrapper &s) { return toStd("<" + s.toString() + ">"); })
      .def_property_readonly(
          "frameCount",
          [](const SceneWrapper &s) { return s.m_scene->getFrameCount(); })
      .def_property_readonly(
          "columnCount",
          [](const SceneWrapper &s) { return s.getXsheet()->getColumnCount(); })
      .def("load", chain(&SceneWrapper::load), py::arg("path"),
           "Loads the scene file; returns the scene")
      .def("save", chain(&SceneWrapper::save), py::arg("path"),
           "Saves the scene; returns the scene")
      .def("getLevels", &SceneWrapper::getLevels)
      .def(
          "getLevel",
          [](const SceneWrapper &s, const std::string &name) {
            return s.getLevel(QString::fromStdString(name));
          },
          py::arg("name"), "The level named name, or None")
      .def(
          "newLevel",
          [](SceneWrapper &s, const std::string &type,
             const std::string &name) {
            return s.newLevel(QString::fromStdString(type),
                              QString::fromStdString(name));
          },
          py::arg("type"), py::arg("name"),
          "Adds a new level (type: Vector, ToonzRaster or Raster)")
      .def(
          "loadLevel",
          [](SceneWrapper &s, const std::string &name, Arg path) {
            return s.loadLevel(QString::fromStdString(name), path);
          },
          py::arg("name"), py::arg("path"), "Loads a level into the scene")
      .def(
          "setCell",
          [](py::object self, int row, int col, Arg level, Arg fid) {
            self.cast<SceneWrapper &>().setCell(row, col, level, fid);
            return self;
          },
          py::arg("row"), py::arg("col"), py::arg("level"), py::arg("fid"),
          "Sets a cell (level: a Level or a level name; fid: e.g. 1 or "
          "\"1a\"); returns the scene")
      .def(
          "setCell",
          [](py::object self, int row, int col, Arg cell) {
            self.cast<SceneWrapper &>().setCell(row, col, cell);
            return self;
          },
          py::arg("row"), py::arg("col"), py::arg("cell"),
          "Sets a cell from a dict {\"level\": ..., \"fid\": ...}, or clears "
          "it with None; returns the scene")
      .def("getCell", &SceneWrapper::getCell, py::arg("row"), py::arg("col"),
           "The cell as a dict {\"level\": Level, \"fid\": int or str}, or "
           "None if empty")
      .def("getColumns", &SceneWrapper::getColumns,
           "The columns, as dicts with the keys index, id, name, type "
           "(Level, Sound, SoundText, Zerary, Palette, Mesh or Empty), "
           "firstRow and lastRow (-1 if the column has no cells)")
      .def("renderFrame", &SceneWrapper::renderFrame, py::arg("frame"),
           "Renders the frame (from 0) with the output settings of the "
           "scene; returns an Image")
      .def("insertColumn", chain(&SceneWrapper::insertColumn), py::arg("col"))
      .def("deleteColumn", chain(&SceneWrapper::deleteColumn), py::arg("col"))
      .def(
          "getStageObject",
          [](SceneWrapper &s, const std::string &name) {
            return s.getStageObject(QString::fromStdString(name));
          },
          py::arg("name"),
          "The stage object by id (Col1, Peg1, Camera1, Table) or by name")
      .def("getStageObjects", &SceneWrapper::getStageObjects,
           "The cameras, the table, the pegbars, then the columns")
      .def("newPegbar", &SceneWrapper::newPegbar, "Adds a new pegbar");
}

}  // namespace PythonBinding
