#include "automation_live.h"

#include "tapp.h"
#include "tdoubleparam.h"
#include "tfxattributes.h"
#include "tnotanimatableparam.h"
#include "toutputproperties.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "tundo.h"
#include "tunit.h"
#include "toonz/doubleparamcmd.h"
#include "toonz/fxdag.h"
#include "toonz/levelset.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/tfxhandle.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshzeraryfxcolumn.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSet>
#include <QThread>
#include <QUuid>

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>

namespace GroundzeroAutomation {
namespace Live {
namespace {

constexpr int MaxKeys       = 4096;
constexpr int MaxParameters = 256;
constexpr int MaxHandles    = 1024;

void limit(bool exceeded, const QString& message) {
  if (exceeded) throw Error("resource_limit", message);
}

void guiThread() {
  if (!QCoreApplication::instance() ||
      QThread::currentThread() != QCoreApplication::instance()->thread())
    throw Error("gui_busy",
                "Native live scene access requires the GUI thread.");
}

QString text(const std::string& value) {
  limit(value.size() > 4096, "Native metadata string exceeds 4096 bytes.");
  return QString::fromStdString(value);
}

QString text(const std::wstring& value) {
  limit(value.size() > 4096, "Native metadata string exceeds 4096 characters.");
  return QString::fromStdWString(value);
}

double finite(double value) {
  if (!std::isfinite(value))
    throw Error("unsupported_target",
                "Native metadata contains a nonfinite number.");
  return value;
}

QJsonArray point(const TPointD& value) {
  return {finite(value.x), finite(value.y)};
}

struct ReadBudget final {
  QElapsedTimer timer;
  int parameters = 0;
  int keys       = 0;
  int cells      = 0;
  std::map<TParam*, int> owners;
  QJsonArray reasons;
  ReadBudget() { timer.start(); }
  void check() const {
    if (timer.elapsed() >= 25)
      throw Error("deadline_exceeded",
                  "Live inspection exceeded its 25 ms traversal budget.");
  }
  void unsupported(const QString& reason) {
    if (!reasons.contains(reason)) {
      limit(reasons.size() >= 64,
            "Live inspection has too many unsupported constructs.");
      reasons.append(reason);
    }
  }
};

QJsonObject keyJson(const TDoubleKeyframe& key) {
  limit(key.m_fileParams.m_path.getQString().size() > 4096,
        "Native keyframe path exceeds 4096 characters.");
  return {{"frame", finite(key.m_frame)},
          {"value", finite(key.m_value)},
          {"type", int(key.m_type)},
          {"previous_type", int(key.m_prevType)},
          {"is_keyframe", key.m_isKeyframe},
          {"step", key.m_step},
          {"speed_in", point(key.m_speedIn)},
          {"speed_out", point(key.m_speedOut)},
          {"linked_handles", key.m_linkedHandles},
          {"expression", text(key.m_expressionText)},
          {"file", key.m_fileParams.m_path.getQString()},
          {"file_field", key.m_fileParams.m_fieldIndex},
          {"unit_name", text(key.m_unitName)},
          {"similar_shape_offset", finite(key.m_similarShapeOffset)}};
}

bool simpleKey(const TDoubleKeyframe& key) {
  const bool simpleType = key.m_type == TDoubleKeyframe::Constant ||
                          key.m_type == TDoubleKeyframe::Linear;
  return simpleType && key.m_step == 1 && key.m_isKeyframe &&
         key.m_frame >= 0 && key.m_frame <= 999999 &&
         std::floor(key.m_frame) == key.m_frame;
}

QJsonObject displayUnit(TDoubleParam* curve) {
  if (!curve->getMeasure() || !curve->getMeasure()->getCurrentUnit()) return {};
  const auto* unit = curve->getMeasure()->getCurrentUnit();
  return {{"name", text(unit->getDefaultExtension())},
          {"zero", finite(unit->convertTo(0))},
          {"one", finite(unit->convertTo(1))}};
}

QJsonArray choices(TDoubleParam* curve) {
  const auto& values = curve->getUIChoices();
  limit(values.size() > 256,
        "Native scalar choice schema exceeds 256 entries.");
  QJsonArray result;
  for (const auto& value : values) result.append(text(value));
  return result;
}

QJsonObject curveJson(TDoubleParam* curve, ReadBudget& budget) {
  budget.check();
  limit(++budget.parameters > MaxParameters,
        "Live inspection exceeds 256 parameters.");
  ++budget.owners[curve];
  budget.keys += curve->getKeyframeCount();
  limit(budget.keys > MaxKeys, "Live inspection exceeds 4096 curve keys.");
  bool supported = !curve->isCycleEnabled();
  QJsonArray keys;
  for (int index = 0; index < curve->getKeyframeCount(); ++index) {
    budget.check();
    const auto& key = curve->getKeyframe(index);
    supported &= simpleKey(key);
    keys.append(keyJson(key));
  }
  QJsonObject result{{"type", "double"},
                     {"default", finite(curve->getDefaultValue())},
                     {"measure", text(curve->getMeasureName())},
                     {"locked", curve->isEditorLocked()},
                     {"cycle", curve->isCycleEnabled()},
                     {"display_unit", displayUnit(curve)},
                     {"choices", choices(curve)},
                     {"checkbox", curve->isUICheckbox()},
                     {"keys", keys},
                     {"supported", supported}};
  double minimum, maximum, step;
  if (curve->getValueRange(minimum, maximum, step))
    result["range"] =
        QJsonArray{finite(minimum), finite(maximum), finite(step)};
  if (!supported)
    budget.unsupported("A curve has unsupported interpolation or cycling.");
  return result;
}

void encode(QDataStream& stream, const QJsonValue& value) {
  stream << qint32(value.type());
  if (value.isDouble())
    stream << value.toDouble();
  else if (value.isString())
    stream << value.toString();
  else if (value.isBool())
    stream << value.toBool();
  else if (value.isArray()) {
    const auto array = value.toArray();
    stream << qint32(array.size());
    for (const auto& child : array) encode(stream, child);
  } else if (value.isObject()) {
    const auto object = value.toObject();
    stream << qint32(object.size());
    for (auto it = object.begin(); it != object.end(); ++it) {
      stream << it.key();
      encode(stream, it.value());
    }
  }
}

QString digest(const QJsonObject& value) {
  QByteArray bytes;
  QDataStream stream(&bytes, QIODevice::WriteOnly);
  stream.setVersion(QDataStream::Qt_5_15);
  stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
  encode(stream, value);
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

template <typename T>
using Handles = std::map<QString, TSmartPointerT<T>>;

template <typename T>
QString identity(Handles<T>& handles, T* object, int& issued) {
  if (!object) return {};
  for (const auto& entry : handles)
    if (entry.second.getPointer() == object) return entry.first;
  limit(issued >= MaxHandles,
        "Live scene epoch exhausted its 1024 native handles.");
  const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  handles.emplace(id, TSmartPointerT<T>(object));
  ++issued;
  return id;
}

template <typename T>
T* resolve(const Handles<T>& handles, const QString& id) {
  const auto found = handles.find(id);
  if (found == handles.end())
    throw Error(
        "target_deleted",
        "Native target is unknown or retired; inspect the current scene.");
  return found->second.getPointer();
}

}  // namespace

class SceneState final {
public:
  QString instance;
  ToonzScene* scene = nullptr;
  TXsheet* sheet    = nullptr;
  std::shared_ptr<void> lease;
  quint64 epoch        = 0;
  quint64 serial       = 0;
  quint64 binding      = 0;
  quint64 historyReset = 0;
  QString membership;
  QString observed;
  bool started = false;
  int issued   = 0;
  Handles<TStageObject> objects;
  Handles<TFx> effects;
  Handles<TFx> nativeEffects;
  Handles<TParam> parameters;
  Handles<TXshLevel> levels;
  Handles<TXshColumn> columns;
  QJsonArray reasons;

  explicit SceneState(QString id) : instance(std::move(id)) {}
  void retire() {
    objects.clear();
    effects.clear();
    nativeEffects.clear();
    parameters.clear();
    levels.clear();
    columns.clear();
    observed.clear();
    ++serial;
  }
  QString objectId(TStageObject* value) {
    return identity(objects, value, issued);
  }
  QString fxId(TFx* value) { return identity(effects, value, issued); }
  QString nativeFxId(TFx* value) {
    return identity(nativeEffects, value, issued);
  }
  QString parameterId(TParam* value) {
    return identity(parameters, value, issued);
  }
  QString levelId(TXshLevel* value) { return identity(levels, value, issued); }
  QString columnId(TXshColumn* value) {
    return identity(columns, value, issued);
  }
};

namespace {

void availableGeneration(std::uint64_t generation) {
  limit(generation == std::numeric_limits<std::uint64_t>::max(),
        "A native lifecycle generation is exhausted; restart the editor.");
}

QString memberships(SceneState& state) {
  auto* graph = state.sheet->getFxDag();
  for (const auto value :
       {state.sheet->getStageObjectTree()->membershipGeneration(),
        graph->getInternalFxs()->membershipGeneration(),
        graph->getTerminalFxs()->membershipGeneration(),
        state.sheet->columnGeneration(),
        state.scene->getLevelSet()->membershipGeneration(),
        graph->outputGeneration()})
    availableGeneration(value);
  return QString("%1:%2:%3:%4:%5:%6")
      .arg(
          qulonglong(state.sheet->getStageObjectTree()->membershipGeneration()))
      .arg(qulonglong(graph->getInternalFxs()->membershipGeneration()))
      .arg(qulonglong(graph->getTerminalFxs()->membershipGeneration()))
      .arg(qulonglong(state.sheet->columnGeneration()))
      .arg(qulonglong(state.scene->getLevelSet()->membershipGeneration()))
      .arg(qulonglong(graph->outputGeneration()));
}

bool bindingChanged(const SceneState& state, TSceneHandle* handle) {
  return state.binding != handle->bindingGeneration() ||
         state.historyReset != TUndoManager::manager()->resetGeneration() ||
         state.scene != handle->getScene() ||
         (state.scene && state.sheet != state.scene->getXsheet());
}

void refresh(SceneState& state) {
  guiThread();
  auto* handle  = TApp::instance()->getCurrentScene();
  auto* history = TUndoManager::manager();
  availableGeneration(handle->bindingGeneration());
  availableGeneration(history->resetGeneration());
  availableGeneration(history->changeGeneration());
  if (bindingChanged(state, handle)) {
    state.retire();
    state.binding      = handle->bindingGeneration();
    state.historyReset = history->resetGeneration();
    state.lease        = handle->retainScene();
    state.scene        = handle->getScene();
    state.sheet        = state.scene ? state.scene->getXsheet() : nullptr;
    state.membership.clear();
    state.issued  = 0;
    state.reasons = {"Inspect the current scene before editing."};
    ++state.epoch;
  }
  if (!state.sheet) return;
  const QString current = memberships(state);
  if (current != state.membership) {
    state.retire();
    state.membership = current;
    state.reasons    = {"Native owner membership changed; inspect again."};
  }
}

void requireEpoch(SceneState& state, const QJsonObject& params) {
  refresh(state);
  if (params.contains("instance_id") &&
      params.value("instance_id") != state.instance)
    throw Error("scene_changed",
                "The target belongs to another live instance.");
  if (TUndoManager::manager()->isBusy())
    throw Error("gui_busy",
                "Native undo history is executing or has an open block.");
  if (!state.scene ||
      requiredString(params, "scene_epoch") != QString::number(state.epoch))
    throw Error("scene_changed",
                "The editor scene changed; inspect its current epoch.");
}

QJsonObject cameraJson(SceneState& state) {
  auto* tree   = state.sheet->getStageObjectTree();
  auto* object = tree->getStageObject(tree->getCurrentCameraId(), false);
  if (!object || !object->getCamera())
    throw Error("unsupported_target", "The active native camera is missing.");
  const auto* camera = object->getCamera();
  return {{"object_id", state.objectId(object)},
          {"width_px", camera->getRes().lx},
          {"height_px", camera->getRes().ly},
          {"width_in", finite(camera->getSize().lx)},
          {"height_in", finite(camera->getSize().ly)}};
}

QJsonObject sceneJson(SceneState& state) {
  return {{"name", text(state.scene->getSceneName())},
          {"dirty", TApp::instance()->getCurrentScene()->getDirtyFlag()},
          {"fps", finite(state.scene->getProperties()
                             ->getOutputProperties()
                             ->getFrameRate())},
          {"camera", cameraJson(state)}};
}

QJsonArray levelsJson(SceneState& state, ReadBudget& budget) {
  auto* levels = state.scene->getLevelSet();
  limit(levels->getLevelCount() > 256, "Live inspection exceeds 256 levels.");
  QJsonArray result;
  for (int index = 0; index < levels->getLevelCount(); ++index) {
    budget.check();
    auto* level    = levels->getLevel(index);
    const int type = level->getType();
    const bool supported =
        type == PLI_XSHLEVEL || type == TZP_XSHLEVEL || type == OVL_XSHLEVEL;
    if (!supported)
      budget.unsupported("The root scene contains an unsupported level type.");
    result.append(QJsonObject{{"level_id", state.levelId(level)},
                              {"name", text(level->getName())},
                              {"native_type", type},
                              {"drawing_count", level->getFrameCount()},
                              {"supported", supported}});
  }
  return result;
}

QJsonArray exposures(SceneState& state, int column, ReadBudget& budget) {
  auto* cells = state.sheet->getColumn(column)->getCellColumn();
  if (!cells) return {};
  int first = 0, last = -1;
  cells->getRange(first, last);
  limit(first < 0 || last > 999999 || qint64(last) - first + 1 > 4096,
        "A live column exposure span exceeds 4096 rows.");
  QJsonArray result;
  for (int row = first; row <= last; ++row) {
    budget.check();
    const auto& cell = cells->getCell(row);
    if (cell.isEmpty()) continue;
    limit(++budget.cells > 4096,
          "Live inspection exceeds 4096 occupied exposures.");
    result.append(QJsonObject{
        {"frame", row},
        {"level_id", state.levelId(cell.m_level.getPointer())},
        {"drawing_id", QJsonObject{{"number", cell.m_frameId.getNumber()},
                                   {"suffix", cell.m_frameId.getLetter()}}}});
  }
  return result;
}

QJsonArray columnsJson(SceneState& state, ReadBudget& budget) {
  limit(state.sheet->getColumnCount() > 64,
        "Live inspection exceeds 64 root columns.");
  QJsonArray result;
  for (int index = 0; index < state.sheet->getColumnCount(); ++index) {
    budget.check();
    auto* column = state.sheet->getColumn(index);
    if (!column) continue;
    if (!column->getLevelColumn() && !column->getZeraryFxColumn())
      budget.unsupported("A root column has an unsupported native type.");
    result.append(QJsonObject{{"index", index},
                              {"column_id", state.columnId(column)},
                              {"locked", column->isLocked()},
                              {"visible", column->isPreviewVisible()},
                              {"exposures", exposures(state, index, budget)}});
  }
  return result;
}

QString channelName(int index) {
  static const QStringList names{
      "rotation", "x",          "y",          "z",       "stacking_order",
      "scale_x",  "scale_y",    "scale",      "path",    "shear_x",
      "shear_y",  "rotation_x", "rotation_y", "focus_z", "aperture"};
  return names.at(index);
}

bool artworkHandle(const std::string& value) {
  return !value.empty() && value.front() == 'H';
}

void stageDependencies(TStageObject* object, ReadBudget& budget) {
  if (object->getStatus() != TStageObject::XY || object->getSpline() ||
      object->getOffsetLayerCount() || object->getPlasticSkeletonDeformation())
    budget.unsupported(
        "A stage object has spline, offset, or Plastic dependencies.");
  if (artworkHandle(object->getHandle()) ||
      artworkHandle(object->getParentHandle()))
    budget.unsupported(
        "A stage object depends on drawing hooks or skeleton handles.");
}

QJsonObject cameraProperties(const TCamera* camera) {
  if (!camera) return {};
  return {{"width_px", camera->getRes().lx},
          {"height_px", camera->getRes().ly},
          {"width_in", finite(camera->getSize().lx)},
          {"height_in", finite(camera->getSize().ly)}};
}

QJsonObject stageJson(SceneState& state, TStageObject* object,
                      ReadBudget& budget) {
  stageDependencies(object, budget);
  TPointD center, offset;
  object->getCenterAndOffset(center, offset);
  auto* parent = state.sheet->getStageObjectTree()->getStageObject(
      object->getParent(), false);
  QJsonArray curves;
  for (int index = 0; index < TStageObject::T_ChannelCount; ++index) {
    auto curve =
        curveJson(object->getParam(TStageObject::Channel(index)), budget);
    curve["channel"] = channelName(index);
    curves.append(curve);
  }
  const auto id      = object->getId();
  const QString kind = id.isCamera()   ? "camera"
                       : id.isPegbar() ? "pegbar"
                                       : "other";
  return {{"object_id", state.objectId(object)},
          {"kind", kind},
          {"name", text(object->getName())},
          {"parent_id", state.objectId(parent)},
          {"locked", object->isLocked()},
          {"camera", cameraProperties(object->getCamera())},
          {"center", point(center)},
          {"offset", point(offset)},
          {"handle", text(object->getHandle())},
          {"parent_handle", text(object->getParentHandle())},
          {"curves", curves}};
}

QJsonArray stagesJson(SceneState& state, ReadBudget& budget) {
  auto* tree = state.sheet->getStageObjectTree();
  limit(tree->getStageObjectCount() > 128,
        "Live inspection exceeds 128 stage objects.");
  QJsonArray result;
  for (int index = 0; index < tree->getStageObjectCount(); ++index) {
    budget.check();
    result.append(stageJson(state, tree->getStageObject(index), budget));
  }
  return result;
}

TFx* parameterFx(TFx* fx) {
  if (auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx))
    return wrapper->getZeraryFx();
  return fx;
}

std::set<TFx*> graphMembers(SceneState& state) {
  auto* graph = state.sheet->getFxDag();
  limit(graph->getInternalFxs()->getFxCount() > 64 ||
            graph->getOutputFxCount() > 64,
        "Live inspection exceeds 64 FX nodes.");
  std::set<TFx*> result;
  graph->getInternalFxs()->getFxs(result);
  result.insert(graph->getXsheetFx());
  for (int index = 0; index < graph->getOutputFxCount(); ++index)
    result.insert(graph->getOutputFx(index));
  for (int index = 0; index < state.sheet->getColumnCount(); ++index) {
    auto* column = state.sheet->getColumn(index);
    if (column && column->getFx()) result.insert(column->getFx());
  }
  limit(result.size() > 64, "Live inspection exceeds 64 FX nodes.");
  return result;
}

bool builtin(TFx* fx) {
  static const std::set<std::string> types{
      "colorCardFx",          "overFx",     "STD_iwa_LinearGradientFx",
      "STD_radialGradientFx", "STD_blurFx", "STD_glowFx"};
  return fx && types.count(fx->getFxType());
}

QJsonObject parameterJson(TParam* param, ReadBudget& budget, int depth);

QJsonObject parameterGroup(TParamSet* group, ReadBudget& budget, int depth) {
  limit(depth >= 8 || group->getParamCount() > MaxParameters,
        "Native parameter group exceeds live inspection limits.");
  QJsonArray children;
  for (int index = 0; index < group->getParamCount(); ++index) {
    auto child =
        parameterJson(group->getParam(index).getPointer(), budget, depth + 1);
    child["name"] = text(group->getParamName(index));
    children.append(child);
  }
  return {{"type", "group"}, {"children", children}};
}

QJsonObject parameterJson(TParam* param, ReadBudget& budget, int depth) {
  budget.check();
  if (auto* curve = dynamic_cast<TDoubleParam*>(param))
    return curveJson(curve, budget);
  limit(++budget.parameters > MaxParameters,
        "Live inspection exceeds 256 parameters.");
  ++budget.owners[param];
  if (auto* group = dynamic_cast<TParamSet*>(param))
    return parameterGroup(group, budget, depth);
  if (auto* value = dynamic_cast<TIntParam*>(param))
    return {{"type", "integer"}, {"value", value->getValue()}};
  if (auto* value = dynamic_cast<TBoolParam*>(param))
    return {{"type", "boolean"}, {"value", value->getValue()}};
  if (auto* value = dynamic_cast<TEnumParam*>(param))
    return {{"type", "enum"}, {"value", value->getValue()}};
  budget.unsupported("An FX parameter has an unsupported native type.");
  return {{"type", "unsupported"}};
}

QJsonArray fxParameters(TFx* fx, ReadBudget& budget) {
  auto* parameters = fx->getParams();
  limit(parameters->getParamCount() > MaxParameters,
        "FX parameter count exceeds 256.");
  QJsonArray result;
  for (int index = 0; index < parameters->getParamCount(); ++index) {
    auto value      = parameterJson(parameters->getParam(index), budget, 0);
    value["name"]   = text(parameters->getParamName(index));
    value["hidden"] = parameters->isParamHidden(index);
    result.append(value);
  }
  return result;
}

QJsonArray portsJson(SceneState& state, TFx* fx,
                     const std::set<TFx*>& members) {
  limit(fx->getInputPortCount() > 64, "An FX exceeds 64 native input ports.");
  QJsonArray result;
  for (int index = 0; index < fx->getInputPortCount(); ++index) {
    auto* source = fx->getInputPort(index)->getFx();
    if (source && !members.count(source))
      throw Error("unsupported_target",
                  "An FX port refers outside the root graph.");
    result.append(QJsonObject{{"name", text(fx->getInputPortName(index))},
                              {"source_fx_id", state.fxId(source)}});
  }
  return result;
}

bool structuralFx(TFx* fx, TFx* native, FxDag* graph) {
  if (fx == graph->getXsheetFx() || dynamic_cast<TOutputFx*>(fx)) return true;
  return dynamic_cast<TColumnFx*>(fx) && fx == native;
}

QJsonObject fxJson(SceneState& state, TFx* fx, ReadBudget& budget,
                   const std::set<TFx*>& members) {
  auto* native = parameterFx(fx);
  if (!native)
    throw Error("unsupported_target", "An FX wrapper has no native effect.");
  auto* graph          = state.sheet->getFxDag();
  const bool supported = structuralFx(fx, native, graph) || builtin(native);
  if (!supported)
    budget.unsupported("The root graph contains an unsupported FX type.");
  if (native->getLinkedFx() != native)
    budget.unsupported("The root graph contains linked FX parameters.");
  if (fx->getAttributes()->hasGlobalControl())
    budget.unsupported(
        "The root graph contains an unsupported global FX control.");
  QJsonObject result{
      {"fx_id", state.fxId(fx)},
      {"native_identity", state.nativeFxId(native)},
      {"type", text(native->getFxType())},
      {"name", text(native->getName())},
      {"supported", supported},
      {"enabled", fx->getAttributes()->isEnabled()},
      {"version", fx->getAttributes()->getFxVersion()},
      {"global_control", fx->getAttributes()->hasGlobalControl()},
      {"ports", portsJson(state, fx, members)},
      {"terminal", graph->getTerminalFxs()->containsFx(fx)},
      {"current_output", graph->getCurrentOutputFx() == fx}};
  if (supported) result["parameters"] = fxParameters(native, budget);
  return result;
}

QJsonArray effectsJson(SceneState& state, ReadBudget& budget) {
  QJsonArray result;
  const auto members = graphMembers(state);
  for (auto* fx : members) {
    budget.check();
    result.append(fxJson(state, fx, budget, members));
  }
  return result;
}

struct Snapshot final {
  QJsonObject value;
  QJsonObject qualification;
  QJsonArray reasons;
  std::map<TParam*, int> owners;
  QString metadataDigest;
};

Snapshot snapshot(SceneState& state) {
  ReadBudget budget;
  Snapshot result;
  result.value         = {{"scene", sceneJson(state)},
                          {"levels", levelsJson(state, budget)},
                          {"columns", columnsJson(state, budget)},
                          {"stage_objects", stagesJson(state, budget)},
                          {"fx", effectsJson(state, budget)}};
  result.reasons       = budget.reasons;
  result.owners        = std::move(budget.owners);
  result.qualification = result.value;
  auto scene           = result.qualification.value("scene").toObject();
  scene.remove("dirty");
  result.qualification["scene"]       = scene;
  result.qualification["instance_id"] = state.instance;
  result.qualification["scene_epoch"] = QString::number(state.epoch);
  result.qualification["history_generation"] =
      QString::number(qulonglong(TUndoManager::manager()->changeGeneration()));
  result.qualification["membership"]  = state.membership;
  result.qualification["unsupported"] = result.reasons;
  QJsonArray parameterIdentities;
  for (const auto& entry : result.owners)
    parameterIdentities.append(
        QJsonArray{state.parameterId(entry.first), entry.second});
  result.qualification["parameter_identities"] = parameterIdentities;
  result.metadataDigest                        = digest(result.qualification);
  limit(QJsonDocument(result.value).toJson(QJsonDocument::Compact).size() >
            900 * 1024,
        "Live metadata exceeds its bounded response size.");
  budget.check();
  return result;
}

QString fingerprint(const Snapshot& data, const QJsonObject& target) {
  return digest({{"profile", "qualified_target_dependencies_v1"},
                 {"metadata_digest", data.metadataDigest},
                 {"target", target}});
}

void observe(SceneState& state, const Snapshot& data) {
  const QString current = data.metadataDigest;
  if (state.observed != current) {
    state.observed = current;
    ++state.serial;
  }
  state.reasons = data.reasons;
}

struct Channel final {
  TStageObject::Channel native;
  QString name;
  QString unit;
};

const std::vector<Channel>& channels() {
  static const std::vector<Channel> result{
      {TStageObject::T_X, "x", "inch"},
      {TStageObject::T_Y, "y", "inch"},
      {TStageObject::T_Angle, "rotation", "degree"}};
  return result;
}

const Channel& channel(const QString& name) {
  for (const auto& value : channels())
    if (value.name == name) return value;
  throw Error("invalid_params",
              "Live stage edits support x, y, or rotation only.");
}

QJsonObject stageTarget(const QString& id, const Channel& value) {
  return {{"object_id", id}, {"channel", value.name}, {"unit", value.unit}};
}

QJsonObject fxTarget(const QString& id) {
  return {{"fx_id", id}, {"parameter", "value"}, {"unit", "fxLength_native"}};
}

bool uniqueOwner(const Snapshot& data, TDoubleParam* curve) {
  const auto owner = data.owners.find(curve);
  return owner != data.owners.end() && owner->second == 1;
}

bool editableStage(TStageObject* object) {
  return !object->isLocked() &&
         (object->getId().isCamera() || object->getId().isPegbar());
}

bool editableCurve(const Snapshot& data, TDoubleParam* curve) {
  return curve && !curve->isEditorLocked() && uniqueOwner(data, curve);
}

struct CurveFields final {
  QString collection, identity, curves, field;
};

const CurveFields& curveFields(bool fx) {
  static const CurveFields effect{"fx", "fx_id", "parameters", "name"};
  static const CurveFields stage{"stage_objects", "object_id", "curves",
                                 "channel"};
  return fx ? effect : stage;
}

QJsonObject targetCurve(const Snapshot& data, const QString& id,
                        const QString& field, bool fx) {
  const auto& fields = curveFields(fx);
  for (const auto& item : data.value.value(fields.collection).toArray()) {
    const auto owner = item.toObject();
    if (owner.value(fields.identity).toString() != id) continue;
    for (const auto& curve : owner.value(fields.curves).toArray())
      if (curve.toObject().value(fields.field).toString() == field)
        return curve.toObject();
  }
  throw Error("unsupported_target",
              "The target curve is absent from bounded metadata.");
}

QJsonArray editableStages(SceneState& state, const Snapshot& data) {
  QJsonArray result;
  for (const auto& entry : state.objects) {
    auto* object = entry.second.getPointer();
    if (!editableStage(object)) continue;
    for (const auto& value : channels()) {
      auto* curve = object->getParam(value.native);
      if (!editableCurve(data, curve) || !curve->getKeyframeCount()) continue;
      auto target       = stageTarget(entry.first, value);
      target["tool"]    = "live.stage.set_key_value";
      target["minimum"] = -1000000;
      target["maximum"] = 1000000;
      target["curve"]   = targetCurve(data, entry.first, value.name, false);
      target["expected_fingerprint"] =
          fingerprint(data, stageTarget(entry.first, value));
      result.append(target);
    }
  }
  return result;
}

QJsonArray editableEffects(SceneState& state, const Snapshot& data) {
  QJsonArray result;
  for (const auto& entry : state.effects) {
    auto* fx = parameterFx(entry.second.getPointer());
    if (!fx || fx->getFxType() != "STD_blurFx") continue;
    auto* curve =
        dynamic_cast<TDoubleParam*>(fx->getParams()->getParam("value"));
    if (!editableCurve(data, curve)) continue;
    auto target        = fxTarget(entry.first);
    target["tool"]     = "live.fx.set_scalar_value";
    target["minimum"]  = 0;
    target["maximum"]  = 10000;
    target["curve"]    = targetCurve(data, entry.first, "value", true);
    target["location"] = curve->getKeyframeCount() ? "existing_key" : "default";
    target["expected_fingerprint"] = fingerprint(data, fxTarget(entry.first));
    result.append(target);
  }
  return result;
}

void requireFresh(const Snapshot& data, const QJsonObject& target,
                  const QJsonObject& params) {
  if (!data.reasons.isEmpty())
    throw Error("unsupported_target",
                "Live writes require the admitted root metadata profile.",
                {{"reasons", data.reasons}});
  if (requiredString(params, "expected_fingerprint") !=
      fingerprint(data, target))
    throw Error("stale_target",
                "Target dependencies changed; inspect before editing.");
}

double newValue(const QJsonObject& params, TDoubleParam* curve, double bound) {
  const auto input   = params.value("value");
  const double value = input.toDouble();
  if (!input.isDouble() || !std::isfinite(value) || std::abs(value) > bound)
    throw Error("invalid_params",
                "The scalar value exceeds the admitted finite range.");
  double minimum, maximum, step;
  if (curve->getValueRange(minimum, maximum, step) &&
      (value < minimum || value > maximum))
    throw Error("invalid_params",
                "The scalar value exceeds its native parameter range.");
  if (curve->isEditorLocked())
    throw Error("locked", "The native curve is editor-locked.");
  return value;
}

int existingKey(TDoubleParam* curve, const QJsonObject& params) {
  if (!params.contains("frame"))
    throw Error("invalid_params", "An existing key requires frame.");
  const int frame = integerParam(params, "frame", 0, 999999, -1);
  for (int index = 0; index < curve->getKeyframeCount(); ++index)
    if (curve->getKeyframe(index).m_frame == frame) return index;
  throw Error("unsupported_target",
              "Live edits require an existing key at the exact frame.");
}

TDoubleParam* fxCurve(SceneState& state, const QJsonObject& params) {
  auto* owner = resolve(state.effects, requiredString(params, "fx_id"));
  if (!graphMembers(state).count(owner))
    throw Error("target_deleted",
                "The FX no longer belongs to the root graph.");
  auto* fx = parameterFx(owner);
  if (!fx || fx->getFxType() != "STD_blurFx" ||
      requiredString(params, "parameter") != "value")
    throw Error("unsupported_target",
                "Live scalar FX edits support only STD_blurFx.value.");
  if (requiredString(params, "unit") != "fxLength_native")
    throw Error("invalid_params", "Blur value requires fxLength_native units.");
  auto* curve = dynamic_cast<TDoubleParam*>(fx->getParams()->getParam("value"));
  if (!curve)
    throw Error("unsupported_target",
                "Blur value is not a native double parameter.");
  return curve;
}

int fxKey(TDoubleParam* curve, const QJsonObject& params) {
  const QString location = requiredString(params, "location");
  if (location == "existing_key") return existingKey(curve, params);
  if (location != "default")
    throw Error("invalid_params",
                "FX location must be default or existing_key.");
  if (curve->getKeyframeCount() || params.contains("frame"))
    throw Error("unsupported_target",
                "Default edits require an unkeyed curve and no frame.");
  return -1;
}

TStageObject* stageObject(SceneState& state, const QJsonObject& params) {
  auto* object = resolve(state.objects, requiredString(params, "object_id"));
  const auto* owned =
      state.sheet->getStageObjectTree()->getStageObject(object->getId(), false);
  if (owned != object)
    throw Error("target_deleted", "The stage object left the root tree.");
  if (!object->getId().isCamera() && !object->getId().isPegbar())
    throw Error("unsupported_target",
                "Live stage edits require a camera or pegbar.");
  if (object->isLocked())
    throw Error("locked", "The native stage object is locked.");
  return object;
}

void notifyNative(ToonzScene* scene, bool fx) {
  auto* app = TApp::instance();
  if (app->getCurrentScene()->getScene() != scene) return;
  if (fx) app->getCurrentFx()->notifyFxChanged();
  app->getCurrentXsheet()->notifyXsheetChanged();
  app->getCurrentScene()->notifySceneChanged();
}

class ScalarUndo final : public TUndo {
  TDoubleParamP m_curve;
  TDoubleKeyframe m_before, m_after;
  int m_index;
  ToonzScene* m_scene;
  bool m_fx;
  std::shared_ptr<void> m_lease;

  void restore(const TDoubleKeyframe& key) const {
    if (m_index < 0)
      m_curve->setDefaultValue(key.m_value);
    else
      m_curve->setKeyframe(m_index, key);
    notifyNative(m_scene, m_fx);
  }

public:
  ScalarUndo(TDoubleParam* curve, int index, ToonzScene* scene, bool fx)
      : m_curve(curve)
      , m_index(index)
      , m_scene(scene)
      , m_fx(fx)
      , m_lease(TSceneHandle::retainScene(scene)) {
    if (index < 0) {
      m_before.m_prevType = TDoubleKeyframe::None;
      m_before.m_value    = curve->getDefaultValue();
    } else
      m_before = curve->getKeyframe(index);
    m_after = m_before;
  }
  void setValue(double value) { m_after.m_value = value; }
  void apply() const {
    if (m_index < 0)
      m_curve->setDefaultValue(m_after.m_value);
    else {
      KeyframeSetter setter(m_curve.getPointer(), m_index, false);
      setter.setValue(m_after.m_value);
    }
  }
  void undo() const override { restore(m_before); }
  void redo() const override { restore(m_after); }
  int getSize() const override { return static_cast<int>(sizeof(*this)); }
  QString getHistoryString() override {
    return m_fx ? QObject::tr("Live Automation: Blur Value")
                : QObject::tr("Live Automation: Stage Key");
  }
};

QJsonObject commitScalar(SceneState& state, const Snapshot& before,
                         const QJsonObject& target, TDoubleParam* curve,
                         const QJsonObject& edit) {
  const int index    = edit.value("key_index").toInt();
  const double value = edit.value("value").toDouble();
  const double previous =
      index < 0 ? curve->getDefaultValue() : curve->getKeyframe(index).m_value;
  const QString initial = fingerprint(before, target);
  QJsonObject result{{"changed", previous != value},
                     {"before_fingerprint", initial},
                     {"after_fingerprint", initial},
                     {"before_value", previous},
                     {"after_value", value},
                     {"undo_entry_added", false},
                     {"observation_serial", QString::number(state.serial)}};
  if (previous == value) return result;
  const bool fx = target.contains("fx_id");
  auto undo     = std::make_unique<ScalarUndo>(curve, index, state.scene, fx);
  undo->setValue(value);
  ReadBudget checkBudget;
  auto expected = curveJson(curve, checkBudget);
  if (index < 0)
    expected["default"] = value;
  else {
    auto keys        = expected.value("keys").toArray();
    auto key         = keys[index].toObject();
    key["value"]     = value;
    keys[index]      = key;
    expected["keys"] = keys;
  }
  state.started = true;
  undo->apply();
  ReadBudget afterBudget;
  if (curveJson(curve, afterBudget) != expected)
    throw Error("native_error",
                "Native scalar edit changed unexpected curve metadata.");
  TUndoManager::manager()->add(undo.release());
  notifyNative(state.scene, fx);
  const auto after = snapshot(state);
  observe(state, after);
  result["after_fingerprint"]  = fingerprint(after, target);
  result["observation_serial"] = QString::number(state.serial);
  result["undo_entry_added"]   = true;
  return result;
}

}  // namespace

/// Create a GUI-owned registry without taking ownership of the editor scene.
/// Args:
///   instanceId: The authenticated live service instance identity.
/// Returns:
///   Shared opaque state for both clients of that service.
std::shared_ptr<SceneState> createSceneState(const QString& instanceId) {
  guiThread();
  return std::make_shared<SceneState>(instanceId);
}

/// Refresh lifecycle identities and return the last observed domain admission.
/// Args:
///   state: The service's GUI-thread registry.
/// Returns:
///   Scene epoch, observation serial, cached admission and bounded reasons.
QJsonObject refreshScene(SceneState& state) {
  refresh(state);
  return {{"scene_epoch", state.scene ? QJsonValue(QString::number(state.epoch))
                                      : QJsonValue()},
          {"observation_serial", QString::number(state.serial)},
          {"write_admission", state.scene && state.reasons.isEmpty()},
          {"reasons", state.reasons}};
}

/// Read one bounded root metadata observation without loading drawing images.
/// Args:
///   state: The service's GUI-thread registry.
///   params: Validated arguments containing the expected scene epoch.
/// Returns:
///   Metadata and qualified fingerprints for supported existing scalar targets.
QJsonObject inspectScene(SceneState& state, const QJsonObject& params) {
  ReadBudget totalBudget;
  requireEpoch(state, params);
  const auto data = snapshot(state);
  observe(state, data);
  QJsonObject result           = data.value;
  result["instance_id"]        = state.instance;
  result["scene_epoch"]        = QString::number(state.epoch);
  result["observation_serial"] = QString::number(state.serial);
  result["consistency"]        = "qualified_target_dependencies_v1";
  result["unsupported"]        = data.reasons;
  QJsonArray targets;
  if (data.reasons.isEmpty()) {
    targets = editableStages(state, data);
    for (const auto& target : editableEffects(state, data))
      targets.append(target);
  }
  result["editable_targets"] = targets;
  limit(
      QJsonDocument(result).toJson(QJsonDocument::Compact).size() > 1024 * 1024,
      "Live inspection exceeds its 1 MiB response limit.");
  totalBudget.check();
  return result;
}

/// Change one existing camera or pegbar key with one native undo entry.
/// Args:
///   state: The service's GUI-thread registry and mutation marker.
///   params: Validated target, value, units, epoch and expected fingerprint.
/// Returns:
///   Exact before/after values and fingerprints, including unchanged outcomes.
QJsonObject editStage(SceneState& state, const QJsonObject& params) {
  requireEpoch(state, params);
  auto* object             = stageObject(state, params);
  const auto& valueChannel = channel(requiredString(params, "channel"));
  if (requiredString(params, "unit") != valueChannel.unit)
    throw Error("invalid_params",
                "The stage channel requires its explicit native unit.");
  auto* curve        = object->getParam(valueChannel.native);
  const double value = newValue(params, curve, 1000000);
  const int index    = existingKey(curve, params);
  const auto before  = snapshot(state);
  if (!uniqueOwner(before, curve))
    throw Error("unsupported_target",
                "The target curve has shared native owners.");
  const auto target =
      stageTarget(requiredString(params, "object_id"), valueChannel);
  requireFresh(before, target, params);
  auto result = commitScalar(state, before, target, curve,
                             {{"key_index", index}, {"value", value}});
  for (auto it = target.begin(); it != target.end(); ++it)
    result[it.key()] = it.value();
  result["frame"] = params.value("frame");
  return result;
}

/// Change an unshared native Blur default or existing scalar key with undo.
/// Args:
///   state: The service's GUI-thread registry and mutation marker.
///   params: Validated target, location, value, unit, epoch and fingerprint.
/// Returns:
///   Exact before/after values and fingerprints, including unchanged outcomes.
QJsonObject editFx(SceneState& state, const QJsonObject& params) {
  requireEpoch(state, params);
  auto* curve        = fxCurve(state, params);
  const int index    = fxKey(curve, params);
  const double value = newValue(params, curve, 10000);
  if (value < 0)
    throw Error("invalid_params", "Blur value cannot be negative.");
  const auto before = snapshot(state);
  if (!uniqueOwner(before, curve))
    throw Error("unsupported_target",
                "The target curve has shared native owners.");
  const auto target = fxTarget(requiredString(params, "fx_id"));
  requireFresh(before, target, params);
  auto result = commitScalar(state, before, target, curve,
                             {{"key_index", index}, {"value", value}});
  for (auto it = target.begin(); it != target.end(); ++it)
    result[it.key()] = it.value();
  result["location"] = params.value("location");
  if (index >= 0) result["frame"] = params.value("frame");
  return result;
}

void resetMutationStarted(SceneState& state) { state.started = false; }
bool mutationStarted(const SceneState& state) { return state.started; }

}  // namespace Live
}  // namespace GroundzeroAutomation
