#include "automation.h"
#include "automation_fx.h"

#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tfx.h"
#include "tnotanimatableparam.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "trasterfx.h"
#include "tzeraryfx.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshzeraryfxcolumn.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <vector>

namespace GroundzeroAutomation {
namespace {

const QStringList& allowedTypes() {
  static const QStringList types = {
      "colorCardFx",          "overFx",     "STD_iwa_LinearGradientFx",
      "STD_radialGradientFx", "STD_blurFx", "STD_glowFx"};
  return types;
}

QJsonObject stringSchema() { return {{"type", "string"}, {"minLength", 1}}; }

QJsonObject frameSchema() {
  return {{"type", "integer"},
          {"minimum", 0},
          {"maximum", 1000000},
          {"description", "Zero-based scene frame."}};
}

FxDag* dag(Document& document) {
  return document.scene->getXsheet()->getFxDag();
}

TFx* parameterFx(TFx* fx) {
  auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx);
  if (!wrapper) return fx;
  if (!wrapper->getZeraryFx())
    throw Error("INVALID_GRAPH", "Generator column has no effect.");
  return wrapper->getZeraryFx();
}

std::set<TFx*> sceneMembers(ToonzScene& scene) {
  std::set<TFx*> members;
  TXsheet* sheet = scene.getXsheet();
  FxDag* graph   = sheet->getFxDag();
  graph->getInternalFxs()->getFxs(members);
  members.insert(graph->getXsheetFx());
  for (int index = 0; index < graph->getOutputFxCount(); ++index)
    members.insert(graph->getOutputFx(index));
  for (int index = 0; index < sheet->getColumnCount(); ++index) {
    TXshColumn* column = sheet->getColumn(index);
    if (column && column->getFx()) members.insert(column->getFx());
  }
  return members;
}

std::set<TFx*> graphMembers(Document& document) {
  return sceneMembers(*document.scene);
}

TFx* ownedFx(Document& document, const QString& id) {
  TFx* fx = document.fx(id);
  if (graphMembers(document).count(fx) == 0) {
    document.effects.remove(id);
    throw Error("STALE_ENTITY", "Effect no longer belongs to this document.",
                {{"fx_id", id}});
  }
  return fx;
}

void requireEditable(TFx* fx) {
  const QString type = QString::fromStdString(parameterFx(fx)->getFxType());
  if (!allowedTypes().contains(type))
    throw Error("UNSUPPORTED_FX",
                "Effect is outside the builtin automation profile.",
                {{"type", type}});
}

TStageObject* columnObject(Document& document, TColumnFx* fx) {
  return document.scene->getXsheet()->getStageObjectTree()->getStageObject(
      TStageObjectId::ColumnId(fx->getColumnIndex()), false);
}

void requireColumnEditable(Document& document, TFx* fx) {
  auto* column = dynamic_cast<TColumnFx*>(fx);
  if (!column) return;
  TStageObject* object = columnObject(document, column);
  if (!object || !column->getXshColumn())
    throw Error("INVALID_GRAPH",
                "Column effect is missing its column or stage object.");
  if (object->isLocked() || column->getXshColumn()->isLocked())
    throw Error("locked",
                "Unlock the column and stage object before modifying it.");
}

TFxP makeEffect(const QString& type) {
  if (!allowedTypes().contains(type))
    throw Error("UNSUPPORTED_FX",
                "Effect creation requires an allowed builtin type.",
                {{"type", type}});
  std::vector<TFxInfo> registered;
  TFx::listFxs(registered);
  const std::string name = type.toStdString();
  const auto found       = std::find_if(
      registered.begin(), registered.end(),
      [&name](const TFxInfo& info) { return info.m_name == name; });
  if (found == registered.end())
    throw Error("UNAVAILABLE_FX",
                "Builtin effect is not registered in this worker.",
                {{"type", type}});
  // TPersist never falls back to TFx's external-program factory.
  std::unique_ptr<TPersist> object(TPersist::create(name));
  TFx* fx = dynamic_cast<TFx*>(object.get());
  if (!fx || fx->getFxType() != name)
    throw Error("NATIVE_FAILURE",
                "Builtin factory returned an unexpected effect.");
  object.release();
  return TFxP(fx);
}

QJsonObject numericSchema(TDoubleParam* param) {
  QJsonObject schema{{"type", "number"}};
  double minimum, maximum, step;
  if (param->getValueRange(minimum, maximum, step)) {
    schema["minimum"] = minimum;
    schema["maximum"] = maximum;
  }
  schema["measure"] = QString::fromStdString(param->getMeasureName());
  if (!param->getUIChoices().empty()) {
    QJsonArray choices;
    for (const auto& choice : param->getUIChoices())
      choices.append(QString::fromStdString(choice));
    schema["choices"] = choices;
  }
  return schema;
}

QJsonObject fixedArraySchema(int size, const QJsonObject& items) {
  return {{"type", "array"},
          {"minItems", size},
          {"maxItems", size},
          {"items", items}};
}

QJsonObject discreteSchema(TParam* param) {
  if (auto* integer = dynamic_cast<TIntParam*>(param)) {
    int minimum, maximum;
    integer->getValueRange(minimum, maximum);
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
  }
  if (dynamic_cast<TBoolParam*>(param)) return {{"type", "boolean"}};
  if (auto* enumeration = dynamic_cast<TEnumParam*>(param)) {
    QJsonArray values, captions;
    for (int index = 0; index < enumeration->getItemCount(); ++index) {
      int value;
      std::string caption;
      enumeration->getItem(index, value, caption);
      values.append(value);
      captions.append(QString::fromStdString(caption));
    }
    return {{"type", "integer"}, {"enum", values}, {"captions", captions}};
  }
  return {};
}

QJsonObject valueSchema(TParam* param) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param))
    return numericSchema(number);
  if (dynamic_cast<TPixelParam*>(param))
    return fixedArraySchema(
        4, {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}});
  if (dynamic_cast<TPointParam*>(param))
    return fixedArraySchema(2, {{"type", "number"}});
  return discreteSchema(param);
}

QJsonValue discreteValue(TParam* param) {
  if (auto* integer = dynamic_cast<TIntParam*>(param))
    return integer->getValue();
  if (auto* boolean = dynamic_cast<TBoolParam*>(param))
    return boolean->getValue();
  if (auto* enumeration = dynamic_cast<TEnumParam*>(param))
    return enumeration->getValue();
  return QJsonValue();
}

double checkedNumber(TDoubleParam* param, double frame) {
  double value;
  std::string error;
  if (!param->getValueChecked(frame, value, error))
    throw Error("PARAMETER_EVALUATION_FAILED", QString::fromStdString(error));
  return value;
}

QJsonValue parameterValue(TParam* param, double frame) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param))
    return checkedNumber(number, frame);
  if (auto* pixel = dynamic_cast<TPixelParam*>(param)) {
    // Check every channel before using the native clamping/rounding conversion.
    for (int index = 0; index < pixel->getParamCount(); ++index)
      checkedNumber(
          static_cast<TDoubleParam*>(pixel->getParam(index).getPointer()),
          frame);
    const TPixel32 value = pixel->getValue(frame);
    return QJsonArray{value.r, value.g, value.b, value.m};
  }
  if (auto* point = dynamic_cast<TPointParam*>(param))
    return QJsonArray{checkedNumber(point->getX().getPointer(), frame),
                      checkedNumber(point->getY().getPointer(), frame)};
  return discreteValue(param);
}

QJsonObject parameterDescription(TParam* param, const QString& name,
                                 double frame) {
  const QJsonObject schema = valueSchema(param);
  QJsonObject result{{"name", name},
                     {"animatable", param->isAnimatable()},
                     {"supported", !schema.isEmpty()},
                     {"schema", schema}};
  if (schema.isEmpty()) {
    result["unsupported_reason"] =
        "Complex or external-resource parameter is not editable.";
    return result;
  }
  result["value"] = parameterValue(param, frame);
  QJsonArray keys;
  std::set<double> frames;
  param->getKeyframes(frames);
  for (double key : frames) keys.append(key);
  result["keyframes"] = keys;
  return result;
}

QJsonArray parameterDescriptions(TFx* fx, double frame) {
  QJsonArray result;
  TParamContainer* params = parameterFx(fx)->getParams();
  for (int index = 0; index < params->getParamCount(); ++index) {
    const TParamVar* entry = params->getParamVar(index);
    if (entry->isHidden() || entry->isObsolete()) continue;
    result.append(parameterDescription(
        entry->getParam(), QString::fromStdString(entry->getName()), frame));
  }
  return result;
}

QJsonArray portsDescription(TFx* fx, Document* document = nullptr) {
  QJsonArray ports;
  for (int index = 0; index < fx->getInputPortCount(); ++index) {
    TFxPort* port = fx->getInputPort(index);
    QJsonObject entry{
        {"name", QString::fromStdString(fx->getInputPortName(index))},
        {"type", "raster"},
        {"control", port->isaControlPort()},
        {"connected", port->isConnected()}};
    if (document && port->isConnected())
      entry["source_fx_id"] = document->idForFx(port->getFx());
    ports.append(entry);
  }
  return ports;
}

QJsonValue effectCatalog() {
  QJsonArray effects;
  for (const auto& type : allowedTypes()) {
    try {
      TFxP fx = makeEffect(type);
      effects.append(QJsonObject{
          {"type", type},
          {"available", true},
          {"generator", dynamic_cast<TZeraryFx*>(fx.getPointer()) != nullptr},
          {"ports", portsDescription(fx.getPointer())},
          {"parameters", parameterDescriptions(fx.getPointer(), 0)}});
    } catch (const Error& error) {
      if (error.code != "UNAVAILABLE_FX") throw;
      effects.append(QJsonObject{
          {"type", type}, {"available", false}, {"reason", error.message}});
    }
  }
  return QJsonObject{{"profile", "builtin-2d-v1"},
                     {"external_dependencies", QJsonArray{}},
                     {"effects", effects},
                     {"output_creation_type", "output"}};
}

QJsonArray diagnostics(TFx* fx) {
  QJsonArray result;
  if (fx->getFxType() == "STD_glowFx" &&
      !fx->getInputPort("Light")->isConnected())
    result.append(QJsonObject{
        {"code", "GLOW_WITHOUT_LIGHT"},
        {"message",
         "Glow has no Light input; Source passes through unchanged."}});
  return result;
}

QJsonObject describeFx(Document& document, TFx* fx) {
  TFx* native = parameterFx(fx);
  QJsonObject result{
      {"fx_id", document.idForFx(fx)},
      {"type", QString::fromStdString(native->getFxType())},
      {"name", QString::fromStdWString(native->getName())},
      {"ports", portsDescription(fx, &document)},
      {"terminal", dag(document)->getTerminalFxs()->containsFx(fx)},
      {"diagnostics", diagnostics(native)}};
  if (auto* column = dynamic_cast<TColumnFx*>(fx)) {
    result["column_id"]         = document.idForColumn(column->getXshColumn());
    result["column_index"]      = column->getColumnIndex();
    result["generator_wrapper"] = dynamic_cast<TZeraryColumnFx*>(fx) != nullptr;
  }
  return result;
}

QJsonValue inspectGraph(Document& document) {
  QJsonArray effects;
  for (TFx* fx : graphMembers(document))
    effects.append(describeFx(document, fx));
  return QJsonObject{
      {"effects", effects},
      {"xsheet_fx_id", document.idForFx(dag(document)->getXsheetFx())},
      {"current_output_fx_id",
       document.idForFx(dag(document)->getCurrentOutputFx())}};
}

void assignGrammar(TParam* param, TXsheet* sheet) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param)) {
    sheet->getStageObjectTree()->setGrammar(number);
  } else if (auto* parameters = dynamic_cast<TParamSet*>(param)) {
    for (int index = 0; index < parameters->getParamCount(); ++index)
      assignGrammar(parameters->getParam(index).getPointer(), sheet);
  }
}

TFx* insertGenerator(Document& document, TFx* fx, int frameCount) {
  TXsheet* sheet             = document.scene->getXsheet();
  TXshZeraryFxColumnP column = new TXshZeraryFxColumn(frameCount);
  TZeraryColumnFx* wrapper   = column->getZeraryColumnFx();
  wrapper->setZeraryFx(fx);
  const int index = sheet->getColumnCount();
  sheet->insertColumn(index, column.getPointer());
  // New nodes are deliberately disconnected; routing is a separate explicit
  // operation.
  dag(document)->removeFromXsheet(wrapper);
  sheet->updateFrameCount();
  if (sheet->getColumn(index) != column.getPointer() ||
      wrapper->getZeraryFx() != fx)
    throw Error("NATIVE_FAILURE",
                "Generator column insertion did not take effect.");
  return wrapper;
}

QJsonValue createEffect(Context& context, Document& document,
                        const QJsonObject& params) {
  const QString type = requiredString(params, "type");
  const int frames   = integerParam(params, "frame_count", 1, 10000, 1);
  if (graphMembers(document).size() >= 4096)
    throw Error("RESOURCE_LIMIT",
                "The automation graph is limited to 4096 effect nodes.");
  TFxP effect = type == "output" ? TFxP(new TOutputFx()) : makeEffect(type);
  TFx* fx     = effect.getPointer();
  if (params.contains("name"))
    fx->setName(requiredString(params, "name").toStdWString());
  TXsheet* sheet = document.scene->getXsheet();
  for (int index = 0; index < fx->getParams()->getParamCount(); ++index)
    assignGrammar(fx->getParams()->getParam(index), sheet);
  context.markMutationStarted();
  dag(document)->assignUniqueId(fx);
  fx->setNewIdentifier();
  if (auto* output = dynamic_cast<TOutputFx*>(fx))
    dag(document)->addOutputFx(output);
  else if (dynamic_cast<TZeraryFx*>(fx))
    fx = insertGenerator(document, fx, frames);
  else
    dag(document)->getInternalFxs()->addFx(fx);
  if (graphMembers(document).count(fx) == 0)
    throw Error("NATIVE_FAILURE",
                "Created effect was not inserted into the scene graph.");
  return describeFx(document, fx);
}

void appendInputs(TFx* fx, FxDag* graph, std::vector<TFx*>& pending) {
  if (fx == graph->getXsheetFx()) {
    TFxSet* terminals = graph->getTerminalFxs();
    for (int index = 0; index < terminals->getFxCount(); ++index)
      pending.push_back(terminals->getFx(index));
    return;
  }
  fx = parameterFx(fx);
  for (int index = 0; index < fx->getInputPortCount(); ++index) {
    TFx* input = fx->getInputPort(index)->getFx();
    if (input) pending.push_back(input);
  }
}

bool dependsOn(TFx* fx, TFx* target, FxDag* graph) {
  std::vector<TFx*> pending{fx};
  std::set<TFx*> visited;
  while (!pending.empty()) {
    TFx* current = pending.back();
    pending.pop_back();
    if (current == target) return true;
    if (!visited.insert(current).second) continue;
    appendInputs(current, graph, pending);
  }
  return false;
}

void validateNode(TFx* fx, FxDag* graph) {
  if (fx == graph->getXsheetFx() || dynamic_cast<TOutputFx*>(fx)) return;
  if (dynamic_cast<TColumnFx*>(fx) && !dynamic_cast<TZeraryColumnFx*>(fx))
    return;
  requireEditable(fx);
}

void validateEdges(const std::set<TFx*>& members, FxDag* graph) {
  std::set<TFx*> visited, active;
  for (TFx* root : members) {
    std::vector<std::pair<TFx*, bool>> pending{{root, false}};
    while (!pending.empty()) {
      const auto entry = pending.back();
      pending.pop_back();
      TFx* fx = entry.first;
      if (entry.second) {
        active.erase(fx);
        continue;
      }
      if (active.count(fx))
        throw Error("GRAPH_CYCLE", "Native effect graph contains a cycle.");
      if (!visited.insert(fx).second) continue;
      active.insert(fx);
      pending.emplace_back(fx, true);
      std::vector<TFx*> inputs;
      appendInputs(fx, graph, inputs);
      for (TFx* input : inputs) {
        if (members.count(input) == 0)
          throw Error("INVALID_GRAPH",
                      "An input references an effect outside the scene graph.");
        pending.emplace_back(input, false);
      }
    }
  }
}

TFxPort* editablePort(TFx* fx, const QString& name) {
  if (!dynamic_cast<TOutputFx*>(fx)) requireEditable(fx);
  TFxPort* port = fx->getInputPort(name.toStdString());
  if (!port)
    throw Error("UNKNOWN_PORT",
                "Effect has no input port with this exact name.",
                {{"port", name}});
  return port;
}

QJsonValue connectEffect(Context& context, Document& document,
                         const QJsonObject& params, bool disconnect) {
  TFx* destination = ownedFx(document, requiredString(params, "fx_id"));
  TFxPort* port    = editablePort(destination, requiredString(params, "port"));
  TFx* source = disconnect
                    ? nullptr
                    : ownedFx(document, requiredString(params, "source_fx_id"));
  if (source && dynamic_cast<TOutputFx*>(source))
    throw Error("INVALID_CONNECTION",
                "An output node cannot be used as an input source.");
  if (source && (!dynamic_cast<TRasterFx*>(source) || !port->accepts(source)))
    throw Error("PORT_TYPE_MISMATCH",
                "Input port does not accept this source effect.");
  if (source && dependsOn(source, destination, dag(document)))
    throw Error(
        "GRAPH_CYCLE",
        "Connection would create a cycle, including Xsheet terminal links.");
  TFxP previous = port->getFx();
  context.markMutationStarted();
  try {
    port->setFx(source);
    if (port->getFx() != source)
      throw Error("NATIVE_FAILURE",
                  "Native port connection did not take effect.");
  } catch (...) {
    port->setFx(previous.getPointer());
    throw;
  }
  return describeFx(document, destination);
}

QJsonValue setTerminal(Context& context, Document& document,
                       const QJsonObject& params) {
  TFx* fx = ownedFx(document, requiredString(params, "fx_id"));
  if (!dynamic_cast<TColumnFx*>(fx)) requireEditable(fx);
  requireColumnEditable(document, fx);
  if (!params.value("enabled").isBool())
    throw Error("INVALID_PARAMS", "enabled must be a boolean.");
  const bool enabled = params.value("enabled").toBool();
  FxDag* graph       = dag(document);
  if (enabled && dependsOn(fx, graph->getXsheetFx(), graph))
    throw Error("GRAPH_CYCLE", "Xsheet terminal link would create a cycle.");
  context.markMutationStarted();
  if (enabled)
    graph->addToXsheet(fx);
  else
    graph->removeFromXsheet(fx);
  if (graph->getTerminalFxs()->containsFx(fx) != enabled)
    throw Error("NATIVE_FAILURE",
                "Native Xsheet terminal update did not take effect.");
  return describeFx(document, fx);
}

QJsonValue selectOutput(Context& context, Document& document,
                        const QJsonObject& params) {
  TFx* fx      = ownedFx(document, requiredString(params, "fx_id"));
  auto* output = dynamic_cast<TOutputFx*>(fx);
  if (!output)
    throw Error("INVALID_OUTPUT",
                "Selected effect is not a scene output node.");
  context.markMutationStarted();
  dag(document)->setCurrentOutputFx(output);
  if (dag(document)->getCurrentOutputFx() != output)
    throw Error("NATIVE_FAILURE",
                "Native output selection did not take effect.");
  return QJsonObject{{"current_output_fx_id", document.idForFx(output)}};
}

template <typename T>
void forgetEntity(QHash<QString, T*>& handles, T* value) {
  for (auto entry = handles.begin(); entry != handles.end();) {
    if (entry.value() == value)
      entry = handles.erase(entry);
    else
      ++entry;
  }
}

void removeGenerator(Document& document, TZeraryColumnFx* fx) {
  TXshColumn* column = fx->getXshColumn();
  dag(document)->removeFxId(fx->getZeraryFx());
  forgetEntity(document.effects, static_cast<TFx*>(fx->getZeraryFx()));
  forgetEntity(document.columns, column);
  forgetEntity(document.objects, columnObject(document, fx));
  TXsheet* sheet = document.scene->getXsheet();
  sheet->removeColumn(fx->getColumnIndex());
  sheet->updateFrameCount();
}

void requireDeletable(Document& document, TFx* fx) {
  if (dynamic_cast<TOutputFx*>(fx)) {
    if (dag(document)->getOutputFxCount() == 1)
      throw Error("LAST_OUTPUT",
                  "The scene must retain at least one output node.");
    return;
  }
  requireEditable(fx);
  requireColumnEditable(document, fx);
  if (auto* generator = dynamic_cast<TZeraryColumnFx*>(fx)) {
    if (columnObject(document, generator)->hasChildren())
      throw Error(
          "has_dependents",
          "Reparent the generator column's children before deleting it.");
  }
}

QJsonValue deleteEffect(Context& context, Document& document,
                        const QJsonObject& params) {
  const QString id = requiredString(params, "fx_id");
  TFxP effect      = ownedFx(document, id);
  TFx* fx          = effect.getPointer();
  auto* output     = dynamic_cast<TOutputFx*>(fx);
  requireDeletable(document, fx);
  context.markMutationStarted();
  while (fx->getOutputConnectionCount() > 0)
    fx->getOutputConnection(0)->setFx(nullptr);
  fx->disconnectAll();
  dag(document)->removeFxId(fx);
  dag(document)->removeFromXsheet(fx);
  if (output)
    dag(document)->removeOutputFx(output);
  else if (auto* generator = dynamic_cast<TZeraryColumnFx*>(fx))
    removeGenerator(document, generator);
  else
    dag(document)->getInternalFxs()->removeFx(fx);
  if (graphMembers(document).count(fx) != 0)
    throw Error("NATIVE_FAILURE",
                "Native effect deletion did not remove graph membership.");
  forgetEntity(document.effects, fx);
  return QJsonObject{{"deleted_fx_id", id}};
}

TParam* editableParameter(TFx* fx, const QString& name) {
  requireEditable(fx);
  TParamVar* entry =
      parameterFx(fx)->getParams()->getParamVar(name.toStdString());
  if (!entry || entry->isHidden() || entry->isObsolete())
    throw Error("UNKNOWN_PARAMETER",
                "Effect has no public parameter with this exact name.",
                {{"parameter", name}});
  if (valueSchema(entry->getParam()).isEmpty())
    throw Error("UNSUPPORTED_PARAMETER",
                "Complex or external-resource parameter is not editable.");
  return entry->getParam();
}

void validateNumber(const QJsonValue& value, const QJsonObject& schema) {
  if (!value.isDouble() || !std::isfinite(value.toDouble()))
    throw Error("INVALID_PARAMS",
                "Parameter value must be a finite JSON number.");
  const double number = value.toDouble();
  if (schema.value("type") == "integer" && std::floor(number) != number)
    throw Error("INVALID_PARAMS", "Parameter value must be an integer.");
  const double minimum =
      schema.value("minimum").toDouble(-std::numeric_limits<double>::max());
  const double maximum =
      schema.value("maximum").toDouble(std::numeric_limits<double>::max());
  if (number < minimum)
    throw Error("OUT_OF_RANGE", "Parameter value is below its native minimum.");
  if (number > maximum)
    throw Error("OUT_OF_RANGE", "Parameter value exceeds its native maximum.");
  if (schema.contains("enum") &&
      !schema.value("enum").toArray().contains(value))
    throw Error("OUT_OF_RANGE",
                "Parameter value is not a native enumeration choice.");
}

void validateValue(const QJsonValue& value, const QJsonObject& schema) {
  const QString type = schema.value("type").toString();
  if (type == "boolean") {
    if (!value.isBool())
      throw Error("INVALID_PARAMS", "Parameter value must be a boolean.");
    return;
  }
  if (type == "array") {
    const int count = schema.value("minItems").toInt();
    if (!value.isArray() || value.toArray().size() != count)
      throw Error("INVALID_PARAMS", "Parameter array has the wrong size.",
                  {{"size", count}});
    for (const auto& component : value.toArray())
      validateNumber(component, schema.value("items").toObject());
    return;
  }
  validateNumber(value, schema);
}

void setNumber(TDoubleParam* param, double value, int frame, bool key) {
  if (!key) {
    param->setDefaultValue(value);
    return;
  }
  TDoubleKeyframe keyframe(frame, value);
  keyframe.m_isKeyframe = true;
  keyframe.m_prevType   = TDoubleKeyframe::Linear;
  param->setKeyframe(keyframe);
}

void setDiscrete(TParam* param, const QJsonValue& value) {
  if (auto* integer = dynamic_cast<TIntParam*>(param))
    integer->setValue(value.toInt());
  else if (auto* boolean = dynamic_cast<TBoolParam*>(param))
    boolean->setValue(value.toBool());
  else if (auto* enumeration = dynamic_cast<TEnumParam*>(param))
    enumeration->setValue(value.toInt());
  else
    throw Error("UNSUPPORTED_PARAMETER",
                "Parameter has no supported native setter.");
}

void applyParameter(TParam* param, const QJsonValue& value, int frame,
                    bool key) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param)) {
    setNumber(number, value.toDouble(), frame, key);
    return;
  }
  if (auto* parameters = dynamic_cast<TParamSet*>(param)) {
    const QJsonArray values = value.toArray();
    for (int index = 0; index < values.size(); ++index) {
      auto* number =
          dynamic_cast<TDoubleParam*>(parameters->getParam(index).getPointer());
      if (!number)
        throw Error("UNSUPPORTED_PARAMETER",
                    "Compound parameter has a nonnumeric channel.");
      const double scale = dynamic_cast<TPixelParam*>(param) ? 255.0 : 1.0;
      setNumber(number, values[index].toDouble() / scale, frame, key);
    }
    return;
  }
  setDiscrete(param, value);
}

QJsonValue changeParameter(Context& context, Document& document,
                           const QJsonObject& params, bool key) {
  TFx* fx            = ownedFx(document, requiredString(params, "fx_id"));
  const QString name = requiredString(params, "parameter");
  TParam* param      = editableParameter(fx, name);
  requireColumnEditable(document, fx);
  const int frame        = integerParam(params, "frame", 0, 1000000, 0);
  const QJsonValue value = params.value("value");
  validateValue(value, valueSchema(param));
  if (key && !param->isAnimatable())
    throw Error("NOT_ANIMATABLE",
                "Parameter cannot have keyframes; use fx.parameter.set.");
  if (!key && param->hasKeyframes())
    throw Error("ANIMATED_PARAMETER",
                "Parameter has keyframes; use fx.parameter.set_key.");
  TParamP backup = param->clone();
  context.markMutationStarted();
  try {
    applyParameter(param, value, frame, key);
    if (parameterValue(param, frame) != value ||
        (key && !param->isKeyframe(frame)))
      throw Error("NATIVE_FAILURE",
                  "Native parameter update failed its readback check.");
  } catch (...) {
    param->copy(backup.getPointer());
    throw;
  }
  return parameterDescription(param, name, frame);
}

QJsonValue getParameters(Context&, Document& document,
                         const QJsonObject& params) {
  TFx* fx         = ownedFx(document, requiredString(params, "fx_id"));
  const int frame = integerParam(params, "frame", 0, 1000000, 0);
  return QJsonObject{{"fx_id", document.idForFx(fx)},
                     {"frame", frame},
                     {"parameters", parameterDescriptions(fx, frame)}};
}

}  // namespace

void validateAutomationFxGraph(ToonzScene& scene) {
  const std::set<TFx*> members = sceneMembers(scene);
  if (members.size() > 4096)
    throw Error("RESOURCE_LIMIT",
                "The automation graph is limited to 4096 effect nodes.");
  FxDag* graph = scene.getXsheet()->getFxDag();
  for (TFx* fx : members) validateNode(fx, graph);
  validateEdges(members, graph);
}

QJsonArray fxOperations() {
  const QJsonObject effect{{"document_id", stringSchema()},
                           {"fx_id", stringSchema()}};
  QJsonObject creation{
      {"document_id", stringSchema()},
      {"type", stringSchema()},
      {"name", stringSchema()},
      {"frame_count",
       QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}}};
  QJsonObject port           = effect;
  port["port"]               = stringSchema();
  QJsonObject connection     = port;
  connection["source_fx_id"] = stringSchema();
  QJsonObject terminal       = effect;
  terminal["enabled"]        = QJsonObject{{"type", "boolean"}};
  QJsonObject parameters     = effect;
  parameters["frame"]        = frameSchema();
  QJsonObject change         = effect;
  change["parameter"]        = stringSchema();
  change["value"]            = QJsonObject{
      {"description", "Value must match the native parameter schema."}};
  QJsonObject key = change;
  key["frame"]    = frameSchema();
  return {
      operation("fx.catalog",
                "Discover allowed builtin effects and native typed parameters.",
                {}, {}),
      operation("fx.graph",
                "Inspect graph, generator columns, ports, terminals, and "
                "current output.",
                {{"document_id", stringSchema()}}, {"document_id"}),
      operation(
          "fx.create",
          "Create a disconnected builtin effect, generator column, or output.",
          creation, {"document_id", "type"}, true),
      operation(
          "fx.delete",
          "Delete an effect and disconnect its edges; retain the last output.",
          effect, {"document_id", "fx_id"}, true),
      operation(
          "fx.connect",
          "Connect a named input after checking ownership, type, and cycles.",
          connection, {"document_id", "fx_id", "port", "source_fx_id"}, true),
      operation("fx.disconnect", "Disconnect an effect's named input port.",
                port, {"document_id", "fx_id", "port"}, true),
      operation("fx.terminal.set",
                "Connect or disconnect an effect from the Xsheet composite.",
                terminal, {"document_id", "fx_id", "enabled"}, true),
      operation("fx.output.select", "Select a scene output node for rendering.",
                effect, {"document_id", "fx_id"}, true),
      operation(
          "fx.parameter.get",
          "Read public parameter schemas, values, and keyframe positions.",
          parameters, {"document_id", "fx_id"}),
      operation(
          "fx.parameter.set",
          "Set a supported unanimated parameter with native range checks.",
          change, {"document_id", "fx_id", "parameter", "value"}, true),
      operation(
          "fx.parameter.set_key",
          "Set a linear key on an animatable parameter; frames start at 0.",
          key, {"document_id", "fx_id", "parameter", "value", "frame"}, true)};
}

QJsonValue executeFx(Context& context, const QString& method,
                     const QJsonObject& params) {
  if (method == "fx.catalog") return effectCatalog();
  Document& document = context.document(requiredString(params, "document_id"));
  using Handler      = QJsonValue (*)(Context&, Document&, const QJsonObject&);
  static const std::map<QString, Handler> handlers = {
      {"fx.create", createEffect},
      {"fx.delete", deleteEffect},
      {"fx.terminal.set", setTerminal},
      {"fx.output.select", selectOutput},
      {"fx.parameter.get", getParameters},
      {"fx.connect",
       [](Context& ctx, Document& doc, const QJsonObject& args) {
         return connectEffect(ctx, doc, args, false);
       }},
      {"fx.disconnect",
       [](Context& ctx, Document& doc, const QJsonObject& args) {
         return connectEffect(ctx, doc, args, true);
       }},
      {"fx.parameter.set",
       [](Context& ctx, Document& doc, const QJsonObject& args) {
         return changeParameter(ctx, doc, args, false);
       }},
      {"fx.parameter.set_key",
       [](Context& ctx, Document& doc, const QJsonObject& args) {
         return changeParameter(ctx, doc, args, true);
       }},
      {"fx.graph", [](Context&, Document& doc, const QJsonObject&) {
         return inspectGraph(doc);
       }}};
  const auto handler = handlers.find(method);
  if (handler == handlers.end())
    throw Error("METHOD_NOT_FOUND", "Unknown FX operation.");
  return handler->second(context, document, params);
}

}  // namespace GroundzeroAutomation
