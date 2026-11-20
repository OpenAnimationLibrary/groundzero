#include "automation_hook.h"
#include "automation.h"
#include "automation_rig.h"

#include "texception.h"
#include "tstream.h"
#include "toonz/hook.h"
#include "toonz/levelset.h"
#include "toonz/stageunits.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <set>
#include <tuple>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxHooks           = 99;
constexpr int MaxLevelKeys       = 8192;
constexpr int MaxSceneKeys       = 65536;
constexpr int MaxUsers           = 4096;
constexpr int MaxLevelsExposed   = 256;
constexpr double MaxCoordinate   = 1000;
constexpr qint64 MaxCells        = 1000000;
constexpr int MaxInspectionBytes = 1024 * 1024;

struct HookKey final {
  TFrameId frame;
  TPointD a;
  TPointD b;
};
using HookKeys = std::vector<HookKey>;

struct HookSpace final {
  TPointD perInch;
  QJsonObject json;
};

[[noreturn]] void invalid(const QString& message,
                          const QJsonObject& details = {}) {
  throw Error("invalid_params", message, details);
}

TStageObjectTree& stageOf(ToonzScene& scene) {
  return *scene.getXsheet()->getStageObjectTree();
}

Hook* hookAt(TXshSimpleLevel& level, int number) {
  return level.getHookSet()->getHook(number - 1);
}

HookKeys hookKeys(TXshSimpleLevel& level, const Hook& hook) {
  HookKeys keys;
  for (const auto& frame : level.getFids())
    if (hook.isKeyframe(frame))
      keys.push_back({frame, hook.getAPos(frame), hook.getBPos(frame)});
  return keys;
}

QJsonObject drawingJson(const TFrameId& frame) {
  return {{"number", frame.getNumber()}, {"suffix", frame.getLetter()}};
}

QJsonObject pointJson(const TPointD& point) {
  return {{"x", point.x}, {"y", point.y}};
}

HookSpace hookSpace(TXshSimpleLevel& level) {
  const int type = level.getType();
  if (type == PLI_XSHLEVEL)
    return {TPointD(Stage::inch, Stage::inch),
            {{"kind", "vector_stage_unit"},
             {"native_units_per_inch", Stage::inch}}};
  if (type != TZP_XSHLEVEL && type != OVL_XSHLEVEL)
    throw Error("unsupported_level",
                "Hooks require a vector, Toonz raster or RGBA8 level.");
  const auto frames = level.getFids();
  const TPointD dpi = frames.empty() ? TPointD() : level.getDpi(frames[0]);
  for (const auto& frame : frames)
    if (level.getDpi(frame) != dpi)
      throw Error("unsupported_level",
                  "Raster hooks require one DPI for every drawing.");
  if (!frames.empty() &&
      !(std::isfinite(dpi.x) && std::isfinite(dpi.y) && dpi.x > 0 && dpi.y > 0))
    throw Error("unsupported_level", "Raster hooks require a positive DPI.");
  return {dpi, {{"kind", "raster_pixel"}, {"dpi_x", dpi.x}, {"dpi_y", dpi.y}}};
}

TPointD toInch(const HookSpace& space, const TPointD& native) {
  return TPointD(native.x / space.perInch.x, native.y / space.perInch.y);
}

TPointD toNative(const HookSpace& space, const QJsonValue& inch) {
  const auto point = inch.toObject();
  return TPointD(point.value("x").toDouble() * space.perInch.x,
                 point.value("y").toDouble() * space.perInch.y);
}

QJsonArray keysJson(const HookKeys& keys, const HookSpace& space) {
  QJsonArray result;
  for (const auto& key : keys)
    result.append(QJsonObject{{"drawing_id", drawingJson(key.frame)},
                              {"a", pointJson(toInch(space, key.a))},
                              {"b", pointJson(toInch(space, key.b))},
                              {"pass", key.a != key.b}});
  return result;
}

void requireCoordinate(const HookSpace& space, const TPointD& native) {
  const TPointD inch = toInch(space, native);
  if (!std::isfinite(inch.x) || !std::isfinite(inch.y) ||
      std::abs(inch.x) > MaxCoordinate || std::abs(inch.y) > MaxCoordinate)
    invalid("Hook coordinates must be finite and within 1000 inches.");
}

void validateHookKeys(Hook& hook, const HookSpace& space,
                      const HookKeys& keys) {
  if (hook.getTrackerObjectId() >= 0)
    invalid("Hook tracker regions are not admitted in this profile.");
  Hook remainder(hook);
  for (const auto& key : keys) remainder.eraseFrame(key.frame);
  if (!remainder.isEmpty())
    invalid("Hook keys must belong to existing drawings of their level.");
  for (const auto& key : keys) {
    if (!key.frame.getLetter().isEmpty())
      invalid("Hook keys cannot use suffixed drawing IDs.",
              {{"drawing_id", drawingJson(key.frame)}});
    requireCoordinate(space, key.a);
    requireCoordinate(space, key.b);
  }
}

int validateLevelHooks(TXshSimpleLevel& level) {
  HookSet* hooks = level.getHookSet();
  if (hooks->getHookCount() > MaxHooks)
    throw Error("resource_limit", "A level holds at most 99 hooks.");
  if (hooks->getHookCount() == 0) return 0;
  const HookSpace space = hookSpace(level);
  int total             = 0;
  for (int index = 0; index < hooks->getHookCount(); ++index) {
    Hook* hook = hooks->getHook(index);
    if (!hook) continue;
    const HookKeys keys = hookKeys(level, *hook);
    validateHookKeys(*hook, space, keys);
    total += static_cast<int>(keys.size());
    if (total > MaxLevelKeys)
      throw Error("resource_limit", "A level holds at most 8192 hook keys.");
  }
  return total;
}

int sceneHookKeys(ToonzScene& scene) {
  int total       = 0;
  TLevelSet* list = scene.getLevelSet();
  for (int index = 0; index < list->getLevelCount(); ++index) {
    TXshSimpleLevel* level = list->getLevel(index)->getSimpleLevel();
    HookSet* hooks         = level ? level->getHookSet() : nullptr;
    for (int hook = 0; hooks && hook < hooks->getHookCount(); ++hook)
      if (Hook* native = hooks->getHook(hook))
        total += static_cast<int>(hookKeys(*level, *native).size());
  }
  return total;
}

QJsonObject userJson(const QString& object, const QString& column,
                     const QString& role, const QJsonValue& child) {
  return {{"object_id", object},
          {"column_id", column},
          {"role", role},
          {"child_object_id", child}};
}

std::vector<TStageObject*> sortedChildren(const TStageObject& owner) {
  std::vector<TStageObject*> children(owner.getChildren().begin(),
                                      owner.getChildren().end());
  std::sort(children.begin(), children.end(),
            [](const TStageObject* left, const TStageObject* right) {
              return left->getId() < right->getId();
            });
  return children;
}

void appendColumnUsers(Document& document, int index, const std::string& handle,
                       QJsonArray& users) {
  TXsheet* sheet = document.scene->getXsheet();
  TStageObject* owner =
      stageOf(*document.scene)
          .getStageObject(TStageObjectId::ColumnId(index), false);
  if (!owner) return;
  const QString column = document.idForColumn(sheet->getColumn(index));
  const QString object = document.idForObject(owner);
  if (owner->getHandle() == handle)
    users.append(userJson(object, column, "own_handle", QJsonValue()));
  for (TStageObject* child : sortedChildren(*owner))
    if (child->getParentHandle() == handle)
      users.append(userJson(object, column, "child_parent_handle",
                            document.idForObject(child)));
}

QJsonArray hookUsers(Document& document, const AutomationExposures& exposures,
                     TXshSimpleLevel* level, int number) {
  QJsonArray users;
  const auto found = exposures.columns.find(level);
  if (found == exposures.columns.end()) return users;
  const std::string handle = "H" + std::to_string(number);
  for (int index : found->second) {
    appendColumnUsers(document, index, handle, users);
    if (users.size() > MaxUsers)
      throw Error("resource_limit", "A hook has more than 4096 users.");
  }
  return users;
}

bool columnLocked(Document& document, int index) {
  TXshColumn* column = document.scene->getXsheet()->getColumn(index);
  TStageObject* owner =
      stageOf(*document.scene)
          .getStageObject(TStageObjectId::ColumnId(index), false);
  return (column && column->isLocked()) || (owner && owner->isLocked());
}

std::vector<int> exposingColumns(const AutomationExposures& exposures,
                                 TXshSimpleLevel* level) {
  const auto found = exposures.columns.find(level);
  return found == exposures.columns.end() ? std::vector<int>() : found->second;
}

QJsonArray columnsAffected(Document& document,
                           const AutomationExposures& exposures,
                           TXshSimpleLevel* level) {
  QJsonArray result;
  TXsheet* sheet = document.scene->getXsheet();
  for (int index : exposingColumns(exposures, level))
    result.append(document.idForColumn(sheet->getColumn(index)));
  return result;
}

void requireEditableLevel(Document& document,
                          const AutomationExposures& exposures,
                          TXshSimpleLevel* level) {
  for (int index : exposingColumns(exposures, level))
    if (columnLocked(document, index))
      throw Error(
          "locked",
          "Unlock every column that exposes this level; hooks are "
          "shared by all of its exposures.",
          {{"column_id", document.idForColumn(
                             document.scene->getXsheet()->getColumn(index))}});
}

void requireNoPlasticUsers(Document& document,
                           const AutomationExposures& exposures,
                           TXshSimpleLevel* level, int number) {
  const std::string handle = "H" + std::to_string(number);
  for (int index : exposingColumns(exposures, level)) {
    TStageObject* owner =
        stageOf(*document.scene)
            .getStageObject(TStageObjectId::ColumnId(index), false);
    if (!owner || !owner->getPlasticSkeletonDeformation()) continue;
    QJsonArray users;
    appendColumnUsers(document, index, handle, users);
    if (!users.isEmpty())
      throw Error("unsupported_target",
                  "A Plastic-deformed column resolves this hook number to a "
                  "skeleton vertex.");
  }
}

Document& hookDocument(Context& context, const QJsonObject& params) {
  auto& document = context.document(requiredString(params, "document_id"));
  if (document.revisionText() != requiredString(params, "expected_revision"))
    throw Error(
        "revision_conflict",
        "The document changed; inspect its current revision first.",
        {{"document_id", document.id}, {"revision", document.revisionText()}});
  validateAutomationRig(*document.scene);
  return document;
}

TXshSimpleLevel& hookLevel(Document& document, const QJsonObject& params) {
  TXshSimpleLevel* level =
      document.level(requiredString(params, "level_id"))->getSimpleLevel();
  if (!level)
    throw Error("unsupported_level", "Hooks require a simple drawing level.");
  hookSpace(*level);
  return *level;
}

void setKey(Hook& hook, const HookKey& key) {
  hook.eraseFrame(key.frame);
  hook.setAPos(key.frame, key.a);
  if (key.b != key.a) hook.setBPos(key.frame, key.b);
}

void saveHooks(HookSet& hooks, const TFilePath& path) {
  TOStream output(path);
  output.openChild("hooks");
  hooks.saveData(output);
  output.closeChild();
}

void loadHooks(HookSet& hooks, const TFilePath& path) {
  TIStream input(path);
  std::string tag;
  if (!input || !input.matchTag(tag) || tag != "hooks")
    throw Error("invalid_package", "A hooks sidecar has no <hooks> root.");
  hooks.loadData(input);
}

HookKeys roundTrip(const HookKeys& keys, const QString& file) {
  const TFilePath path(file.toStdWString());
  HookSet source;
  for (const auto& key : keys) setKey(*source.touchHook(0), key);
  saveHooks(source, path);
  HookSet loaded;
  loadHooks(loaded, path);
  HookKeys result;
  for (const auto& key : keys) {
    Hook* hook = loaded.getHook(0);
    if (!hook || !hook->isKeyframe(key.frame))
      throw Error("native_error", "Native hook serialization lost a key.");
    result.push_back(
        {key.frame, hook->getAPos(key.frame), hook->getBPos(key.frame)});
  }
  return result;
}

bool sameKeys(const HookKeys& left, const HookKeys& right) {
  return std::equal(left.begin(), left.end(), right.begin(), right.end(),
                    [](const HookKey& first, const HookKey& second) {
                      return first.frame == second.frame &&
                             first.a == second.a && first.b == second.b;
                    });
}

HookKeys canonicalKeys(Context& context, const HookKeys& keys) {
  QTemporaryDir directory(context.scratchDirectory() +
                          "/hook-canonical-XXXXXX");
  if (!directory.isValid())
    throw Error("io_error", "Cannot create private hook preparation storage.");
  const HookKeys first  = roundTrip(keys, directory.filePath("first.xml"));
  const HookKeys second = roundTrip(first, directory.filePath("second.xml"));
  if (!sameKeys(first, second))
    invalid("Native hook coordinates are not stable.");
  return first;
}

double pointError(const HookSpace& space, const TPointD& input,
                  const TPointD& output) {
  const TPointD delta = toInch(space, output - input);
  return std::hypot(delta.x, delta.y);
}

double inputError(const HookSpace& space, const HookKeys& input,
                  const HookKeys& canonical) {
  double error = 0;
  for (std::size_t index = 0; index < input.size(); ++index)
    error =
        std::max({error, pointError(space, input[index].a, canonical[index].a),
                  pointError(space, input[index].b, canonical[index].b)});
  return error;
}

void requireUnsnapped(const HookKey& key) {
  if (key.a != key.b && tdistance2(key.a, key.b) <= 1)
    invalid(
        "b differs from a by at most one native unit; native hooks would "
        "snap it onto a.",
        {{"drawing_id", drawingJson(key.frame)}});
}

HookKeys readPositions(TXshSimpleLevel& level, const QJsonObject& params,
                       const HookSpace& space) {
  std::set<TFrameId> seen;
  HookKeys keys;
  for (const auto& value : params.value("positions").toArray()) {
    const auto item    = value.toObject();
    const auto drawing = item.value("drawing_id").toObject();
    const TFrameId frame(drawing.value("number").toInt());
    if (!level.isFid(frame))
      throw Error("missing_drawing", "A hook position names a missing drawing.",
                  {{"drawing_id", drawingJson(frame)}});
    if (!seen.insert(frame).second)
      invalid("Each drawing appears at most once in positions.",
              {{"drawing_id", drawingJson(frame)}});
    const TPointD a = toNative(space, item.value("a"));
    const TPointD b = item.contains("b") ? toNative(space, item.value("b")) : a;
    keys.push_back({frame, a, b});
    requireUnsnapped(keys.back());
  }
  return keys;
}

void requireKeyBudget(ToonzScene& scene, TXshSimpleLevel& level, int levelKeys,
                      int number, const HookKeys& keys) {
  const Hook* hook = hookAt(level, number);
  int added        = 0;
  for (const auto& key : keys)
    if (!hook || !hook->isKeyframe(key.frame)) ++added;
  if (levelKeys + added > MaxLevelKeys)
    throw Error("resource_limit", "A level holds at most 8192 hook keys.");
  if (sceneHookKeys(scene) + added > MaxSceneKeys)
    throw Error("resource_limit", "A scene holds at most 65536 hook keys.");
}

bool changesHook(TXshSimpleLevel& level, int number, const HookKeys& keys) {
  const Hook* hook = hookAt(level, number);
  for (const auto& key : keys)
    if (!hook || !hook->isKeyframe(key.frame) ||
        hook->getAPos(key.frame) != key.a || hook->getBPos(key.frame) != key.b)
      return true;
  return false;
}

void publishKeys(TXshSimpleLevel& level, int number, const HookKeys& keys) {
  Hook* hook = level.getHookSet()->touchHook(number - 1);
  for (const auto& key : keys) {
    setKey(*hook, key);
    if (hook->getAPos(key.frame) != key.a || hook->getBPos(key.frame) != key.b)
      throw Error("native_error", "A canonical hook key changed on publish.");
  }
}

QJsonObject levelHeader(Document& document, TXshSimpleLevel& level,
                        int number) {
  return {{"document_id", document.id},
          {"revision", document.revisionText()},
          {"level_id", document.idForLevel(&level)},
          {"hook_number", number}};
}

QJsonValue setHook(Context& context, const QJsonObject& params) {
  auto& document        = hookDocument(context, params);
  auto& level           = hookLevel(document, params);
  const int levelKeys   = validateLevelHooks(level);
  const int number      = params.value("hook_number").toInt();
  const HookSpace space = hookSpace(level);
  const HookKeys input  = readPositions(level, params, space);
  const auto exposures  = automationExposures(*document.scene);
  requireEditableLevel(document, exposures, &level);
  requireNoPlasticUsers(document, exposures, &level, number);
  requireKeyBudget(*document.scene, level, levelKeys, number, input);
  const double limit = params.value("max_coordinate_error_inch").toDouble();
  const HookKeys canonical = canonicalKeys(context, input);
  const double error       = inputError(space, input, canonical);
  if (error > limit)
    throw Error(
        "invalid_params",
        "Native hook precision exceeds the caller's coordinate error limit.",
        {{"max_input_error_inch", error}, {"maximum", limit}});
  for (const auto& key : canonical) requireUnsnapped(key);
  if (changesHook(level, number, canonical)) {
    context.markMutationStarted();
    publishKeys(level, number, canonical);
  }
  auto result = levelHeader(document, level, number);
  result.insert("unit", "inch");
  result.insert("keys",
                keysJson(hookKeys(level, *hookAt(level, number)), space));
  result.insert("max_input_error_inch", error);
  result.insert("columns_affected",
                columnsAffected(document, exposures, &level));
  result.insert("users", hookUsers(document, exposures, &level, number));
  return result;
}

std::vector<TFrameId> deletedFrames(const QJsonObject& scope,
                                    const HookKeys& keys) {
  std::vector<TFrameId> frames;
  if (scope.value("kind") == "hook") {
    for (const auto& key : keys) frames.push_back(key.frame);
    return frames;
  }
  std::set<TFrameId> seen;
  for (const auto& value : scope.value("drawing_ids").toArray()) {
    const TFrameId frame(value.toObject().value("number").toInt());
    if (!seen.insert(frame).second)
      invalid("Each drawing appears at most once in drawing_ids.",
              {{"drawing_id", drawingJson(frame)}});
    if (std::none_of(keys.begin(), keys.end(), [&frame](const HookKey& key) {
          return key.frame == frame;
        }))
      throw Error("missing_key", "A listed drawing has no key on this hook.",
                  {{"drawing_id", drawingJson(frame)}});
    frames.push_back(frame);
  }
  return frames;
}

QJsonValue deleteHook(Context& context, const QJsonObject& params) {
  auto& document = hookDocument(context, params);
  auto& level    = hookLevel(document, params);
  validateLevelHooks(level);
  const int number = params.value("hook_number").toInt();
  Hook* hook       = hookAt(level, number);
  if (!hook || hook->isEmpty())
    throw Error("not_found", "The level has no keys on this hook number.");
  const HookKeys keys  = hookKeys(level, *hook);
  const auto frames    = deletedFrames(params.value("scope").toObject(), keys);
  const int remaining  = static_cast<int>(keys.size() - frames.size());
  const auto exposures = automationExposures(*document.scene);
  requireEditableLevel(document, exposures, &level);
  const QJsonArray users = hookUsers(document, exposures, &level, number);
  if (remaining == 0 && !users.isEmpty())
    throw Error("has_dependents",
                "Objects use this hook; change their handles before removing "
                "its last key.",
                {{"users", users}});
  context.markMutationStarted();
  for (const auto& frame : frames) hook->eraseFrame(frame);
  auto result = levelHeader(document, level, number);
  result.insert("removed_keys", static_cast<int>(frames.size()));
  result.insert("remaining_keys", remaining);
  result.insert("hook_exists", remaining > 0);
  result.insert("columns_affected",
                columnsAffected(document, exposures, &level));
  return result;
}

std::vector<int> requestedHooks(TXshSimpleLevel& level,
                                const QJsonObject& params) {
  std::vector<int> numbers;
  if (!params.contains("hook_numbers")) {
    for (int number = 1; number <= level.getHookSet()->getHookCount(); ++number)
      if (automationHookDefined(level, number)) numbers.push_back(number);
    return numbers;
  }
  std::set<int> seen;
  for (const auto& value : params.value("hook_numbers").toArray()) {
    if (!seen.insert(value.toInt()).second)
      invalid("hook_numbers must be unique.");
    numbers.push_back(value.toInt());
  }
  return numbers;
}

QJsonArray exposingJson(Document& document,
                        const AutomationExposures& exposures,
                        TXshSimpleLevel* level) {
  QJsonArray result;
  TXsheet* sheet = document.scene->getXsheet();
  for (int index : exposingColumns(exposures, level))
    result.append(QJsonObject{
        {"column_id", document.idForColumn(sheet->getColumn(index))},
        {"locked", columnLocked(document, index)}});
  return result;
}

QString levelFormat(int type) {
  if (type == PLI_XSHLEVEL) return "vector";
  if (type == TZP_XSHLEVEL) return "toonzRaster";
  return "rgba8";
}

QJsonValue inspectHooks(Context& context, const QJsonObject& params) {
  auto& document = hookDocument(context, params);
  auto& level    = hookLevel(document, params);
  validateLevelHooks(level);
  const HookSpace space = hookSpace(level);
  const auto exposures  = automationExposures(*document.scene);
  QJsonArray hooks;
  for (int number : requestedHooks(level, params)) {
    const Hook* hook = hookAt(level, number);
    hooks.append(QJsonObject{
        {"hook_number", number},
        {"keys", hook ? keysJson(hookKeys(level, *hook), space) : QJsonArray()},
        {"users", hookUsers(document, exposures, &level, number)}});
  }
  const QJsonObject result{
      {"document_id", document.id},
      {"revision", document.revisionText()},
      {"level_id", document.idForLevel(&level)},
      {"level_format", levelFormat(level.getType())},
      {"unit", "inch"},
      {"native_space", space.json},
      {"columns_exposing", exposingJson(document, exposures, &level)},
      {"hooks", hooks}};
  if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() >
      MaxInspectionBytes)
    throw Error("resource_limit",
                "Hook inspection exceeds 1 MiB; request fewer hook_numbers.");
  return result;
}

QByteArray hookDigest(TXshSimpleLevel& level) {
  QJsonArray hooks;
  HookSet* set = level.getHookSet();
  for (int index = 0; index < set->getHookCount(); ++index) {
    QJsonArray keys;
    if (Hook* hook = set->getHook(index))
      for (const auto& key : hookKeys(level, *hook))
        keys.append(QJsonArray{key.frame.getNumber(), key.frame.getLetter(),
                               key.a.x, key.a.y, key.b.x, key.b.y});
    hooks.append(keys);
  }
  return QCryptographicHash::hash(
             QJsonDocument(hooks).toJson(QJsonDocument::Compact),
             QCryptographicHash::Sha256)
      .toHex();
}

using RenumberKey = std::tuple<int, int, QString>;

QJsonArray keyRecords(const std::map<RenumberKey, QJsonArray>& records) {
  QJsonArray result;
  for (const auto& record : records) result.append(record.second);
  return result;
}

QJsonObject handleSchema() {
  return {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
}

QJsonObject integerSchema(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject strictSchema(const QJsonObject& properties,
                         const QJsonArray& required) {
  return {{"type", "object"},
          {"properties", properties},
          {"required", required},
          {"additionalProperties", false}};
}

QJsonObject drawingSchema() {
  return strictSchema({{"number", integerSchema(1, 999999)},
                       {"suffix", QJsonObject{{"const", ""}}}},
                      {"number", "suffix"});
}

QJsonObject pointSchema() {
  const QJsonObject coordinate{{"type", "number"},
                               {"minimum", -MaxCoordinate},
                               {"maximum", MaxCoordinate}};
  return strictSchema({{"x", coordinate}, {"y", coordinate}}, {"x", "y"});
}

QJsonObject arraySchema(const QJsonObject& items, int minimum, int maximum) {
  return {{"type", "array"},
          {"items", items},
          {"minItems", minimum},
          {"maxItems", maximum}};
}

void addTool(QJsonArray& catalog, const QString& name,
             const QString& description, QJsonObject fields,
             QJsonArray required, bool writes) {
  fields.insert("document_id", handleSchema());
  fields.insert("expected_revision", handleSchema());
  required.prepend("expected_revision");
  required.prepend("document_id");
  if (writes) {
    fields.insert("operation_id", handleSchema());
    required.insert(2, "operation_id");
  }
  auto tool = operation(name, description, {}, {}, writes);
  tool.insert("inputSchema", strictSchema(fields, required));
  catalog.append(tool);
}

QJsonObject setFields() {
  const QJsonObject position = strictSchema({{"drawing_id", drawingSchema()},
                                             {"a", pointSchema()},
                                             {"b", pointSchema()}},
                                            {"drawing_id", "a"});
  return {{"level_id", handleSchema()},
          {"hook_number", integerSchema(1, MaxHooks)},
          {"unit", QJsonObject{{"const", "inch"}}},
          {"positions", arraySchema(position, 1, 4096)},
          {"coordinate_policy", QJsonObject{{"const", "native_roundtrip"}}},
          {"max_coordinate_error_inch",
           QJsonObject{{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}};
}

QJsonObject deleteFields() {
  const QJsonObject drawings =
      strictSchema({{"kind", QJsonObject{{"const", "drawings"}}},
                    {"drawing_ids", arraySchema(drawingSchema(), 1, 4096)}},
                   {"kind", "drawing_ids"});
  const QJsonObject whole =
      strictSchema({{"kind", QJsonObject{{"const", "hook"}}}}, {"kind"});
  return {{"level_id", handleSchema()},
          {"hook_number", integerSchema(1, MaxHooks)},
          {"scope", QJsonObject{{"oneOf", QJsonArray{drawings, whole}}}}};
}

}  // namespace

AutomationExposures automationExposures(ToonzScene& scene) {
  std::map<TXshSimpleLevel*, int> order;
  TLevelSet* list = scene.getLevelSet();
  for (int index = 0; index < list->getLevelCount(); ++index)
    order.emplace(list->getLevel(index)->getSimpleLevel(), index);
  const auto rank = [&order](TXshSimpleLevel* level) {
    const auto found = order.find(level);
    return found == order.end() ? INT_MAX : found->second;
  };
  AutomationExposures result;
  TXsheet* sheet = scene.getXsheet();
  qint64 cells   = 0;
  for (int index = 0; index < sheet->getColumnCount(); ++index) {
    TXshColumn* column       = sheet->getColumn(index);
    TXshLevelColumn* exposed = column ? column->getLevelColumn() : nullptr;
    int first = 0, last = -1;
    if (!exposed || !exposed->getRange(first, last)) continue;
    cells += static_cast<qint64>(last) - first + 1;
    if (cells > MaxCells)
      throw Error("resource_limit",
                  "Rig scans admit at most one million cells.");
    std::set<TXshSimpleLevel*> seen;
    for (int row = first; row <= last; ++row)
      if (TXshSimpleLevel* level = exposed->getCell(row).getSimpleLevel())
        seen.insert(level);
    std::vector<TXshSimpleLevel*> levels(seen.begin(), seen.end());
    std::stable_sort(levels.begin(), levels.end(),
                     [&rank](TXshSimpleLevel* left, TXshSimpleLevel* right) {
                       return rank(left) < rank(right);
                     });
    if (static_cast<int>(levels.size()) > MaxLevelsExposed)
      throw Error("resource_limit", "A column exposes at most 256 levels.");
    for (TXshSimpleLevel* level : levels)
      result.columns[level].push_back(index);
    result.levels.emplace(index, std::move(levels));
  }
  return result;
}

bool automationHookDefined(TXshSimpleLevel& level, int hookNumber) {
  const Hook* hook = hookAt(level, hookNumber);
  return hook && !hook->isEmpty();
}

void validateAutomationHooks(ToonzScene& scene) {
  int total       = 0;
  TLevelSet* list = scene.getLevelSet();
  for (int index = 0; index < list->getLevelCount(); ++index) {
    TXshSimpleLevel* level = list->getLevel(index)->getSimpleLevel();
    if (!level) continue;
    total += validateLevelHooks(*level);
    if (total > MaxSceneKeys)
      throw Error("resource_limit", "A scene holds at most 65536 hook keys.");
  }
}

QJsonValue writeAutomationHooks(TXshSimpleLevel& level,
                                const QString& levelFile) {
  if (level.getHookSet()->getHookCount() == 0) return QJsonValue();
  const TFilePath path =
      TXshSimpleLevel::getHookPath(TFilePath(levelFile.toStdWString()));
  saveHooks(*level.getHookSet(), path);
  if (!QFileInfo(path.getQString()).isFile())
    throw Error("package_write_failed", "Cannot write a level hooks sidecar.");
  return QString::fromLatin1(hookDigest(level));
}

void readAutomationHooks(ToonzScene& scene, TXshSimpleLevel& level,
                         const QJsonValue& digest) {
  const TFilePath decoded  = scene.decodeFilePath(level.getPath());
  const TFilePath existing = TXshSimpleLevel::getExistingHookFile(decoded);
  HookSet* hooks           = level.getHookSet();
  hooks->clearHooks();
  if (digest.isNull()) {
    if (!existing.isEmpty())
      throw Error("invalid_package", "A level has an unpublished hooks file.");
    return;
  }
  if (existing != TXshSimpleLevel::getHookPath(decoded))
    throw Error("invalid_package", "A published hooks sidecar is missing.");
  try {
    loadHooks(*hooks, existing);
  } catch (const TException&) {
    throw Error("invalid_package", "A hooks sidecar cannot be parsed.");
  }
  if (QString::fromLatin1(hookDigest(level)) != digest.toString())
    throw Error("invalid_package",
                "Reopened hooks differ from their source level.");
}

int automationHookKeys(TXshSimpleLevel& level, const TFrameId& frame) {
  int count    = 0;
  HookSet* set = level.getHookSet();
  for (int index = 0; index < set->getHookCount(); ++index)
    if (const Hook* hook = set->getHook(index))
      if (hook->isKeyframe(frame)) ++count;
  return count;
}

void requireAutomationHookRemoval(Document& document, TXshSimpleLevel& level,
                                  const TFrameId& frame) {
  HookSet* set = level.getHookSet();
  QJsonArray hooks;
  std::unique_ptr<AutomationExposures> exposures;
  for (int index = 0; index < set->getHookCount(); ++index) {
    const Hook* hook = set->getHook(index);
    if (!hook || !hook->isKeyframe(frame)) continue;
    Hook remainder(*hook);
    remainder.eraseFrame(frame);
    if (!remainder.isEmpty()) continue;
    if (!exposures)
      exposures = std::make_unique<AutomationExposures>(
          automationExposures(*document.scene));
    const QJsonArray users = hookUsers(document, *exposures, &level, index + 1);
    if (!users.isEmpty())
      hooks.append(QJsonObject{{"hook_number", index + 1}, {"users", users}});
  }
  if (!hooks.isEmpty())
    throw Error("has_dependents",
                "Removing this drawing would empty hooks that objects use.",
                {{"hooks", hooks}});
}

AutomationHookRenumber prepareAutomationHookRenumber(
    TXshSimpleLevel& level, const std::map<TFrameId, TFrameId>& mapping) {
  AutomationHookRenumber result;
  std::map<RenumberKey, QJsonArray> records;
  HookSet* set = level.getHookSet();
  for (int index = 0; index < set->getHookCount(); ++index) {
    const Hook* hook = set->getHook(index);
    if (!hook) continue;
    for (const auto& key : hookKeys(level, *hook)) {
      const auto moved = mapping.find(key.frame);
      const TFrameId target =
          moved == mapping.end() ? key.frame : moved->second;
      if (target != key.frame) ++result.moved;
      if (target != key.frame && !target.getLetter().isEmpty())
        throw Error("unsupported_drawing_id",
                    "Hook-keyed drawings cannot take suffixed IDs; native "
                    "hook files store drawing numbers only.",
                    {{"drawing_id", drawingJson(key.frame)}});
      records[{index, target.getNumber(), target.getLetter()}] =
          QJsonArray{key.a.x, key.a.y, key.b.x, key.b.y};
    }
  }
  result.expected = keyRecords(records);
  return result;
}

void verifyAutomationHookRenumber(TXshSimpleLevel& level,
                                  const AutomationHookRenumber& prepared) {
  std::map<RenumberKey, QJsonArray> records;
  HookSet* set = level.getHookSet();
  for (int index = 0; index < set->getHookCount(); ++index)
    if (const Hook* hook = set->getHook(index))
      for (const auto& key : hookKeys(level, *hook))
        records[{index, key.frame.getNumber(), key.frame.getLetter()}] =
            QJsonArray{key.a.x, key.a.y, key.b.x, key.b.y};
  if (keyRecords(records) != prepared.expected)
    throw Error("native_error",
                "Native renumbering did not move the level's hook keys.");
}

QJsonArray automationHookUsers(Document& document,
                               const AutomationExposures& exposures,
                               TXshSimpleLevel& level, int hookNumber) {
  return hookUsers(document, exposures, &level, hookNumber);
}

QJsonArray hookOperations() {
  QJsonArray catalog;
  addTool(catalog, "hook.inspect",
          "Read a level's hooks: keyed drawing IDs with A/B (pass) positions "
          "in inches, the native coordinate space, and every column/object "
          "that uses each hook.",
          {{"level_id", handleSchema()},
           {"hook_numbers", arraySchema(integerSchema(1, MaxHooks), 1, 99)}},
          {"level_id"}, false);
  addTool(catalog, "hook.set",
          "Create or update one hook's A/B positions on explicit unsuffixed "
          "drawings of a level. Positions are canonicalized through native "
          "hook serialization before publication.",
          setFields(),
          {"level_id", "hook_number", "unit", "positions", "coordinate_policy",
           "max_coordinate_error_inch"},
          true);
  addTool(catalog, "hook.delete",
          "Delete a hook's keys on explicit drawings, or the whole hook. "
          "Removing the last key or the hook refuses while any object uses "
          "that hook number.",
          deleteFields(), {"level_id", "hook_number", "scope"}, true);
  return catalog;
}

QJsonValue executeHook(Context& context, const QString& method,
                       const QJsonObject& params) {
  if (method == "hook.inspect") return inspectHooks(context, params);
  if (method == "hook.set") return setHook(context, params);
  if (method == "hook.delete") return deleteHook(context, params);
  throw Error("unknown_method", "Unknown detached hook operation.");
}

}  // namespace GroundzeroAutomation
