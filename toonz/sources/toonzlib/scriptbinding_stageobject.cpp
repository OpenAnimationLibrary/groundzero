

#include "toonz/scriptbinding_stageobject.h"
#include "toonz/scriptbinding_scene.h"

// TnzLib includes
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectid.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/doubleparamcmd.h"

// TnzBase includes
#include "tdoubleparam.h"

// Qt includes
#include <QRegularExpression>
#include <QStringList>

#include <cmath>
#include <limits>
#include <set>

namespace TScriptBinding {

namespace {

// The script names of the channels and their internal units:
//   x, y      inches
//   z         z depth (internal units; 0 is the table)
//   so        stacking order
//   angle     degrees
//   scalex, scaley, scale
//             factors: 1 is 100%
//   shearx, sheary
//             shear factors (the UI shows them as angles)
//   path      position along the motion path, in % of its length
const struct {
  const char *m_name;
  TStageObject::Channel m_channel;
} channels[] = {
    {"x", TStageObject::T_X},
    {"y", TStageObject::T_Y},
    {"z", TStageObject::T_Z},
    {"so", TStageObject::T_SO},
    {"angle", TStageObject::T_Angle},
    {"scalex", TStageObject::T_ScaleX},
    {"scaley", TStageObject::T_ScaleY},
    {"scale", TStageObject::T_Scale},
    {"shearx", TStageObject::T_ShearX},
    {"sheary", TStageObject::T_ShearY},
    {"path", TStageObject::T_Path},
    {"rotx", TStageObject::T_RotX},
    {"roty", TStageObject::T_RotY},
    {"focus", TStageObject::T_FocusZ},
    {"aperture", TStageObject::T_Aperture},
};

// Parses the channel name (case insensitive); makes the calling script throw
// and returns false if it isn't valid
bool getChannel(QJSEngine *engine, const QJSValue &arg,
                TStageObject::Channel &channel) {
  int value = 0;
  if (arg.isString() && StageObject::parseChannel(arg.toString(), value)) {
    channel = (TStageObject::Channel)value;
    return true;
  }
  engine->throwError(StageObject::badChannelMessage(arg.toString()));
  return false;
}

bool getNumber(QJSEngine *engine, const QJSValue &arg, const QString &what,
               double &value) {
  if (arg.isNumber() && std::isfinite(arg.toNumber())) {
    value = arg.toNumber();
    return true;
  }
  engine->throwError(
      QObject::tr("Bad %1 (%2): expected a number").arg(what, arg.toString()));
  return false;
}

// a frame where a keyframe can be: an integer >= 0
bool getKeyframeFrame(QJSEngine *engine, const QJSValue &arg, int &frame) {
  if (arg.isNumber()) {
    double value = arg.toNumber();
    if (std::isfinite(value) && value >= 0 && value == std::floor(value) &&
        value <= std::numeric_limits<int>::max()) {
      frame = (int)value;
      return true;
    }
  }
  engine->throwError(QObject::tr("Bad frame (%1): expected an integer >= 0")
                         .arg(arg.toString()));
  return false;
}

// Parses the stage object ids as TStageObjectId::toString() writes them
bool parseStageObjectId(const QString &name, TStageObjectId &id) {
  if (name == QLatin1String("Table")) {
    id = TStageObjectId::TableId;
    return true;
  }
  static const QRegularExpression re("^(Col|Peg|Camera)([1-9][0-9]{0,6})$");
  QRegularExpressionMatch match = re.match(name);
  if (!match.hasMatch()) return false;
  int index    = match.captured(2).toInt() - 1;
  QString type = match.captured(1);
  if (type == QLatin1String("Col"))
    id = TStageObjectId::ColumnId(index);
  else if (type == QLatin1String("Peg"))
    id = TStageObjectId::PegbarId(index);
  else
    id = TStageObjectId::CameraId(index);
  return true;
}

QString idName(const TStageObjectId &id) {
  return QString::fromStdString(id.toString());
}

}  // namespace

//=============================================================================

StageObject::StageObject(const QJSValue &sceneValue, Scene *scene,
                         TStageObject *obj)
    : m_sceneValue(sceneValue), m_scene(scene), m_obj(obj) {
  m_obj->addRef();
}

StageObject::~StageObject() { m_obj->release(); }

QJSValue StageObject::ctor(QJSEngine *engine, const QJSValueList &args) {
  engine->throwError(
      tr("StageObject can't be created directly: use Scene.getStageObject(), "
         "Scene.getStageObjects() or Scene.newPegbar()"));
  return QJSValue();
}

TXsheet *StageObject::getXsheet() const {
  return m_scene->getToonzScene()->getXsheet();
}

TStageObject *StageObject::getStageObject() const {
  // the object is removed from the tree when its column is deleted, and the
  // tree is replaced when the scene is loaded
  TStageObjectTree *tree = getXsheet()->getStageObjectTree();
  if (tree->getStageObject(m_obj->getId(), false) == m_obj) return m_obj;
  throwError(tr("The stage object %1 is no longer in the scene")
                 .arg(idName(m_obj->getId())));
  return 0;
}

void StageObject::listStageObjects(TXsheet *xsh,
                                   std::vector<TStageObject *> &objs) {
  TStageObjectTree *tree = xsh->getStageObjectTree();
  int count              = tree->getStageObjectCount();
  for (int i = 0; i < count; ++i) {
    TStageObject *obj = tree->getStageObject(i);
    if (!obj->getId().isColumn()) objs.push_back(obj);
  }
  // the column objects are created on demand, as the UI does
  int columnCount = xsh->getColumnCount();
  for (int c = 0; c < columnCount; ++c)
    objs.push_back(xsh->getStageObject(TStageObjectId::ColumnId(c)));
}

TStageObject *StageObject::findStageObject(TXsheet *xsh, const QString &name) {
  TStageObjectId id;
  if (parseStageObjectId(name, id)) {
    if (id.isColumn()) {
      if (id.getIndex() < xsh->getColumnCount()) return xsh->getStageObject(id);
    } else if (TStageObject *obj =
                   xsh->getStageObjectTree()->getStageObject(id, false))
      return obj;
  }
  std::vector<TStageObject *> objs;
  listStageObjects(xsh, objs);
  std::string stdName = name.toStdString();
  for (TStageObject *obj : objs)
    if (obj->getName() == stdName) return obj;
  return 0;
}

bool StageObject::parseChannel(const QString &name, int &channel) {
  for (const auto &c : channels) {
    if (name.compare(QLatin1String(c.m_name), Qt::CaseInsensitive) == 0) {
      channel = c.m_channel;
      return true;
    }
  }
  return false;
}

QString StageObject::badChannelMessage(const QString &name) {
  QStringList names;
  for (const auto &c : channels) names.append(QString::fromLatin1(c.m_name));
  return QObject::tr("Bad channel name (%1): must be one of %2")
      .arg(name, names.join(", "));
}

QString StageObject::getTypeName(TStageObject *obj) {
  TStageObjectId id = obj->getId();
  if (id.isColumn()) return "Column";
  if (id.isPegbar()) return "Pegbar";
  if (id.isCamera()) return "Camera";
  if (id.isTable()) return "Table";
  return "";
}

QString StageObject::linkToParent(TXsheet *xsh, TStageObject *obj,
                                  const QString &name) {
  TStageObjectId id = obj->getId();
  if (id.isTable()) return tr("The Table can't have a parent");

  TStageObjectId parentId = TStageObjectId::NoneId;
  if (!name.isEmpty()) {
    TStageObject *parent = findStageObject(xsh, name);
    if (!parent) return tr("Stage object not found: %1").arg(name);
    parentId = parent->getId();
    if (parent == obj) return tr("Can't link %1 to itself").arg(idName(id));
    if (parent->isAncestor(obj))
      return tr("Can't link %1 to %2: %2 is linked to %1")
          .arg(idName(id), idName(parentId));
    if (parentId.isColumn()) {
      TXshColumn *column = xsh->getColumn(parentId.getIndex());
      if (column && !column->canBeParent())
        return tr("%1 can't be a parent").arg(idName(parentId));
    }
  }

  // as TStageObjectCmd::setParent(), without undo
  if (parentId == TStageObjectId::NoneId && (id.isColumn() || id.isPegbar()))
    parentId = TStageObjectId::TableId;
  obj->setParent(parentId);
  obj->setParentHandle("B");
  return "";
}

void StageObject::doSetKeyframe(TStageObject *obj, int channel, int frame,
                                double value) {
  // As the Function Editor cells and Set Key: the new keyframe gets the
  // interpolation and the step of the preferences, or of the segment it
  // splits. No undo: the scene of the script isn't the one of the UI.
  KeyframeSetter setter(obj->getParam((TStageObject::Channel)channel), -1,
                        false);
  setter.createKeyframe(frame);
  setter.setValue(value);
}

void StageObject::doRemoveKeyframe(TStageObject *obj, int channel, int frame) {
  // as KeyframeSetter::removeKeyframeAt(), without undo
  TDoubleParam *curve = obj->getParam((TStageObject::Channel)channel);
  if (curve->isKeyframe(frame)) curve->deleteKeyframe(frame);
}

QString StageObject::toString() {
  TStageObjectTree *tree = getXsheet()->getStageObjectTree();
  if (tree->getStageObject(m_obj->getId(), false) != m_obj)
    return tr("StageObject (removed)");
  if (m_obj->hasSpecifiedName())
    return QString("StageObject(%1: %2)")
        .arg(idName(m_obj->getId()), QString::fromStdString(m_obj->getName()));
  return QString("StageObject(%1)").arg(idName(m_obj->getId()));
}

QString StageObject::getIdName() const {
  TStageObject *obj = getStageObject();
  if (!obj) return QString();
  return idName(obj->getId());
}

QString StageObject::getType() const {
  TStageObject *obj = getStageObject();
  if (!obj) return QString();
  return getTypeName(obj);
}

QString StageObject::getName() const {
  TStageObject *obj = getStageObject();
  if (!obj) return QString();
  return QString::fromStdString(obj->getName());
}

void StageObject::setName(const QString &name) {
  // as TStageObjectCmd::rename(), without undo
  TStageObject *obj = getStageObject();
  if (!obj) return;
  obj->setName(name.toStdString());
}

QString StageObject::getParentName() const {
  TStageObject *obj = getStageObject();
  if (!obj) return QString();
  TStageObjectId parentId = obj->getParent();
  if (parentId == TStageObjectId::NoneId) return "";
  TStageObject *parent =
      getXsheet()->getStageObjectTree()->getStageObject(parentId, false);
  return parent ? QString::fromStdString(parent->getName()) : idName(parentId);
}

void StageObject::setParentName(const QString &name) {
  TStageObject *obj = getStageObject();
  if (!obj) return;
  QString err = linkToParent(getXsheet(), obj, name);
  if (!err.isEmpty()) throwError(err);
}

QJSValue StageObject::getValue(const QJSValue &channelArg,
                               const QJSValue &frameArg) {
  TStageObject::Channel channel = TStageObject::T_X;
  double frame                  = 0;
  if (!getChannel(engine(), channelArg, channel) ||
      !getNumber(engine(), frameArg, tr("frame"), frame))
    return QJSValue();
  TStageObject *obj = getStageObject();
  if (!obj) return QJSValue();
  return obj->getParam(channel)->getValue(frame);
}

QJSValue StageObject::setValue(const QJSValue &channel, const QJSValue &frame,
                               const QJSValue &value) {
  return setKeyframe(channel, frame, value);
}

QJSValue StageObject::setKeyframe(const QJSValue &channelArg,
                                  const QJSValue &frameArg,
                                  const QJSValue &valueArg) {
  TStageObject::Channel channel = TStageObject::T_X;
  int frame                     = 0;
  double value                  = 0;
  if (!getChannel(engine(), channelArg, channel) ||
      !getKeyframeFrame(engine(), frameArg, frame) ||
      !getNumber(engine(), valueArg, tr("value"), value))
    return QJSValue();
  TStageObject *obj = getStageObject();
  if (!obj) return QJSValue();
  doSetKeyframe(obj, channel, frame, value);
  return self();
}

QJSValue StageObject::removeKeyframe(const QJSValue &channelArg,
                                     const QJSValue &frameArg) {
  TStageObject::Channel channel = TStageObject::T_X;
  int frame                     = 0;
  if (!getChannel(engine(), channelArg, channel) ||
      !getKeyframeFrame(engine(), frameArg, frame))
    return QJSValue();
  TStageObject *obj = getStageObject();
  if (!obj) return QJSValue();
  doRemoveKeyframe(obj, channel, frame);
  return self();
}

QJSValue StageObject::isKeyframe(const QJSValue &channelArg,
                                 const QJSValue &frameArg) {
  TStageObject::Channel channel = TStageObject::T_X;
  int frame                     = 0;
  if (!getChannel(engine(), channelArg, channel) ||
      !getKeyframeFrame(engine(), frameArg, frame))
    return QJSValue();
  TStageObject *obj = getStageObject();
  if (!obj) return QJSValue();
  return obj->getParam(channel)->isKeyframe(frame);
}

QJSValue StageObject::getKeyframes(const QJSValue &channelArg) {
  TStageObject::Channel channel = TStageObject::T_X;
  if (!getChannel(engine(), channelArg, channel)) return QJSValue();
  TStageObject *obj = getStageObject();
  if (!obj) return QJSValue();

  std::set<double> frames;
  obj->getParam(channel)->getKeyframes(frames);
  QJSValue result = engine()->newArray((uint)frames.size());
  quint32 index   = 0;
  for (double frame : frames) result.setProperty(index++, frame);
  return result;
}

QJSValue StageObject::getCenter(const QJSValue &frameArg) {
  int frame = 0;
  if (!getKeyframeFrame(engine(), frameArg, frame)) return QJSValue();
  TStageObject *obj = getStageObject();
  if (!obj) return QJSValue();

  TPointD center  = obj->getCenter(frame);
  QJSValue result = engine()->newObject();
  result.setProperty("x", center.x);
  result.setProperty("y", center.y);
  return result;
}

QJSValue StageObject::setCenter(const QJSValue &frameArg, const QJSValue &xArg,
                                const QJSValue &yArg) {
  int frame = 0;
  double x = 0, y = 0;
  if (!getKeyframeFrame(engine(), frameArg, frame) ||
      !getNumber(engine(), xArg, "x", x) || !getNumber(engine(), yArg, "y", y))
    return QJSValue();
  TStageObject *obj = getStageObject();
  if (!obj) return QJSValue();

  // as the center fields of the Animate tool, without undo
  obj->setCenter(frame, TPointD(x, y));
  return self();
}

}  // namespace TScriptBinding
