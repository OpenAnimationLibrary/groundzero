#include "automation_level_lifecycle.h"

#include "automation_drawing.h"
#include "automation_hook.h"
#include "automation_scene_shared.h"

#include "tgeometry.h"
#include "timage.h"
#include "tpalette.h"
#include "toonz/hook.h"
#include "toonz/sceneproperties.h"

#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QHash>

#include <deque>
#include <utility>

namespace GroundzeroAutomation {
namespace {

constexpr qint64 MaxScannedCells = 1000000;
constexpr int MaxSheets          = 128;
constexpr int MaxUsageRows       = 256;

bool columnLocked(TXsheet& sheet, int index, TXshColumn& column) {
  TStageObject* object = sheet.getStageObjectTree()->getStageObject(
      TStageObjectId::ColumnId(index), false);
  return column.isLocked() || (object && object->isLocked());
}

// Breadth-first walk of the top sheet and every sub-xsheet reachable from it
// or from the cast, bounded by MaxScannedCells and MaxSheets.
class UsageScan final {
public:
  UsageScan(TXshLevel* level, const std::set<TFrameId>* frames)
      : m_level(level), m_frames(frames) {}

  LevelUsage run(Document& document) {
    enqueue(document.scene->getXsheet(), nullptr);
    TLevelSet* cast = document.scene->getLevelSet();
    for (int index = 0; index < cast->getLevelCount(); ++index)
      if (TXshChildLevel* child = cast->getLevel(index)->getChildLevel())
        enqueue(child->getXsheet(), child);
    while (!m_queue.empty() && m_usage.complete) {
      const auto [sheet, owner] = m_queue.front();
      m_queue.pop_front();
      scanSheet(*sheet, owner);
    }
    m_usage.sheets = int(m_visited.size());
    return m_usage;
  }

private:
  void enqueue(TXsheet* sheet, TXshChildLevel* owner) {
    if (!sheet || m_visited.count(sheet)) return;
    if (int(m_visited.size()) == MaxSheets) {
      incomplete("sheet_limit");
      return;
    }
    m_visited.insert(sheet);
    m_queue.emplace_back(sheet, owner);
  }

  void incomplete(const QString& reason) {
    m_usage.complete = false;
    if (m_usage.reason.isEmpty()) m_usage.reason = reason;
  }

  bool matches(const TXshCell& cell) const {
    return cell.m_level.getPointer() == m_level &&
           (!m_frames || m_frames->count(cell.m_frameId));
  }

  void scanSheet(TXsheet& sheet, TXshChildLevel* owner) {
    for (int index = 0; index < sheet.getColumnCount() && m_usage.complete;
         ++index)
      scanColumn(sheet, owner, index);
  }

  void scanColumn(TXsheet& sheet, TXshChildLevel* owner, int index) {
    TXshColumn* entry = sheet.getColumn(index);
    // Sound clips expose no drawings, and their getCell() allocates per call.
    if (!entry || entry->getSoundColumn()) return;
    TXshCellColumn* column = entry->getCellColumn();
    int first = 0, last = -1;
    if (!column || !column->getRange(first, last)) return;
    m_scanned += qint64(last) - first + 1;
    if (m_scanned > MaxScannedCells) {
      incomplete("cell_limit");
      return;
    }
    LevelUsageColumn found{
        &sheet, owner, index, column, columnLocked(sheet, index, *entry), {}};
    for (int row = first; row <= last; ++row) {
      const TXshCell& cell = column->getCell(row);
      if (matches(cell)) found.rows.push_back(row);
      if (TXshChildLevel* child = cell.getChildLevel())
        enqueue(child->getXsheet(), child);
    }
    if (found.rows.empty()) return;
    m_usage.cells += qint64(found.rows.size());
    m_usage.columns.push_back(std::move(found));
  }

  TXshLevel* m_level;
  const std::set<TFrameId>* m_frames;
  qint64 m_scanned = 0;
  std::deque<std::pair<TXsheet*, TXshChildLevel*>> m_queue;
  std::set<TXsheet*> m_visited;
  LevelUsage m_usage;
};

QJsonObject usageRow(Document& document, const LevelUsageColumn& column) {
  const bool top = !column.sheetLevel;
  return {
      {"sheet_level_id",
       top ? QJsonValue() : QJsonValue(document.idForLevel(column.sheetLevel))},
      {"column_id", top ? QJsonValue(document.idForColumn(
                              column.sheet->getColumn(column.index)))
                        : QJsonValue()},
      {"column_index", column.index},
      {"locked", column.locked},
      {"cells", int(column.rows.size())},
      {"first_frame", column.rows.front()},
      {"last_frame", column.rows.back()}};
}

// ------------------------------------------------------------ settings

enum FormatBit { Vector = 1, ToonzRaster = 2, Rgba8 = 4 };

struct SettingRule final {
  const char* name;
  int formats;
  bool needsClean;
};

const std::vector<SettingRule>& settingRules() {
  static const std::vector<SettingRule> rules{
      {"dpi", ToonzRaster | Rgba8, false},
      {"premultiply", Rgba8, false},
      {"white_transparent", Rgba8, false},
      {"antialias_softness", ToonzRaster | Rgba8, false},
      {"subsampling", ToonzRaster | Rgba8, true},
      // Linear (EXR) raster only; no admitted format is linear.
      {"color_space_gamma", 0, true}};
  return rules;
}

int formatBit(const TXshSimpleLevel& level) {
  if (level.getType() == PLI_XSHLEVEL) return Vector;
  if (level.getType() == TZP_XSHLEVEL) return ToonzRaster;
  return Rgba8;
}

QJsonValue settingValue(const TXshSimpleLevel& level, const QString& name) {
  const LevelProperties& properties = *level.getProperties();
  if (name == "dpi") {
    if (level.getType() == PLI_XSHLEVEL) return QJsonValue();
    return properties.getDpiPolicy() == LevelProperties::DP_CustomDpi
               ? properties.getDpi().x
               : properties.getImageDpi().x;
  }
  if (name == "premultiply") return properties.doPremultiply();
  if (name == "white_transparent") return properties.whiteTransp();
  if (name == "antialias_softness") return properties.antialiasSoftness();
  if (name == "subsampling") return properties.getSubsampling();
  return properties.colorSpaceGamma();
}

// The reason a setting cannot be written now, or an empty string.
QString settingRefusal(Document& document, const TXshSimpleLevel& level,
                       const QString& name, bool locked) {
  for (const SettingRule& rule : settingRules()) {
    if (name != rule.name) continue;
    if (!(rule.formats & formatBit(level)))
      return rule.formats == 0 && level.getType() != PLI_XSHLEVEL
                 ? "linear_raster_only"
                 : "not_applicable_to_format";
    if (document.readOnly) return "read_only_document";
    if (level.isReadOnly()) return "read_only_level";
    if (rule.needsClean && level.getProperties()->getDirtyFlag())
      return "unsaved_level";
    if (locked) return "locked_exposure";
    return QString();
  }
  throw Error("invalid_params", "Unknown level setting: " + name);
}

}  // namespace

bool LevelUsage::locked() const {
  for (const LevelUsageColumn& column : columns)
    if (column.locked) return true;
  return false;
}

LevelUsage scanLevelUsage(Document& document, TXshLevel* level,
                          const std::set<TFrameId>* frames) {
  return UsageScan(level, frames).run(document);
}

QJsonObject levelUsageJson(Document& document, const LevelUsage& usage) {
  QJsonArray rows;
  for (const LevelUsageColumn& column : usage.columns) {
    if (rows.size() == MaxUsageRows) break;
    rows.append(usageRow(document, column));
  }
  QJsonObject result{
      {"complete", usage.complete},
      {"cells", usage.cells},
      {"columns", int(usage.columns.size())},
      {"sheets", usage.sheets},
      {"rows", rows},
      {"rows_truncated", int(usage.columns.size()) > rows.size()}};
  if (!usage.complete) result.insert("incomplete_reason", usage.reason);
  return result;
}

namespace {

// Rewrite the given rows of one column run by run and compare every row of
// the column's former range with what was asked.
qint64 rewriteColumn(const LevelUsageColumn& usage, TXshLevel* replacement,
                     const TFrameId& frame) {
  TXshCellColumn& column = *usage.column;
  int first = 0, last = -1;
  column.getRange(first, last);
  std::vector<TXshCell> expected;
  for (int row = first; row <= last; ++row)
    expected.push_back(column.getCell(row));
  for (int row : usage.rows) {
    const TFrameId id =
        frame.isEmptyFrame() ? expected[row - first].m_frameId : frame;
    expected[row - first] =
        replacement ? TXshCell(replacement, id) : TXshCell();
  }
  for (size_t start = 0; start < usage.rows.size();) {
    size_t end = start + 1;
    while (end < usage.rows.size() &&
           usage.rows[end] == usage.rows[end - 1] + 1)
      ++end;
    const int row   = usage.rows[start];
    const int count = int(end - start);
    if (replacement) {
      if (!column.setCells(row, count, &expected[row - first]))
        throw Error("native_error", "A native cell write was refused.",
                    {{"column_index", usage.index}, {"frame", row}});
    } else {
      column.clearCells(row, count);
    }
    start = end;
  }
  for (int row = first; row <= last; ++row)
    if (!(column.getCell(row) == expected[row - first]))
      throw Error("native_error",
                  "The native cells differ from the planned rewrite.",
                  {{"column_index", usage.index}, {"frame", row}});
  return qint64(usage.rows.size());
}

}  // namespace

qint64 rewriteLevelUsage(const LevelUsage& usage, TXshLevel* replacement,
                         const TFrameId& frame) {
  qint64 rewritten = 0;
  for (const LevelUsageColumn& column : usage.columns)
    rewritten += rewriteColumn(column, replacement, frame);
  return rewritten;
}

void requireEditableUsage(Document& document, const LevelUsage& usage) {
  if (!usage.complete)
    throw Error("resource_limit",
                "The level's exposures exceed the scan bound of 1,000,000 "
                "cells or 128 sheets; no change relies on a partial scan.",
                {{"limit", usage.reason}});
  QJsonArray locked, ids;
  for (const LevelUsageColumn& column : usage.columns) {
    if (!column.locked || locked.size() == MaxUsageRows) continue;
    const QJsonObject row = usageRow(document, column);
    locked.append(row);
    if (row.value("column_id").isString()) ids.append(row.value("column_id"));
  }
  if (!locked.isEmpty())
    throw Error("locked",
                "Unlock every column that exposes the level; the change "
                "reaches all of its exposures.",
                {{"columns", locked}, {"column_ids", ids}});
}

QJsonObject levelSettingsReport(Document& document, TXshSimpleLevel* level,
                                const LevelUsage& usage) {
  const LevelProperties& properties = *level->getProperties();
  const bool raster                 = level->getType() != PLI_XSHLEVEL;
  QJsonObject result{
      {"dpi_policy", properties.getDpiPolicy() == LevelProperties::DP_CustomDpi
                         ? "custom"
                         : "image"},
      {"image_dpi_x", raster ? properties.getImageDpi().x : 0.0},
      {"image_dpi_y", raster ? properties.getImageDpi().y : 0.0},
      {"image_width_px", raster ? properties.getImageRes().lx : 0},
      {"image_height_px", raster ? properties.getImageRes().ly : 0},
      {"dirty", properties.getDirtyFlag()}};
  for (const SettingRule& rule : settingRules()) {
    const QString name = rule.name;
    const QString reason =
        settingRefusal(document, *level, name, usage.locked());
    const bool applicable =
        reason != "not_applicable_to_format" && reason != "linear_raster_only";
    result.insert(
        name,
        QJsonObject{
            {"value", applicable ? settingValue(*level, name) : QJsonValue()},
            {"applicable", applicable},
            {"writable", reason.isEmpty()},
            {"reason", reason.isEmpty() ? QJsonValue() : QJsonValue(reason)}});
  }
  return result;
}

// ------------------------------------------------------ level.settings.set

namespace {

using namespace SceneShared;

[[noreturn]] void invalidParams(const QString& message, const QString& reason) {
  throw Error("invalid_params", message, {{"reason", reason}});
}

QString formatName(const TXshSimpleLevel& level) {
  if (level.getType() == PLI_XSHLEVEL) return "vector";
  if (level.getType() == TZP_XSHLEVEL) return "toonzRaster";
  return "rgba8";
}

// A vector, Toonz raster or RGBA level of the document: the formats the
// editable profile admits. Sound and sub-xsheet levels have their own owners.
TXshSimpleLevel* lifecycleLevel(Document& document, const QString& handle) {
  TXshLevel* level        = document.level(handle);
  TXshSimpleLevel* simple = level->getSimpleLevel();
  const int type          = simple ? simple->getType() : NO_XSHLEVEL;
  if (type != PLI_XSHLEVEL && type != TZP_XSHLEVEL && type != OVL_XSHLEVEL)
    throw Error("unsupported_level",
                "Only vector, Toonz raster and RGBA levels have a lifecycle "
                "here; sound and sub-xsheet levels belong to their own tools.",
                {{"level_id", handle}});
  return simple;
}

QJsonObject levelRecord(Document& document, TXshSimpleLevel* level) {
  return {{"level_id", document.idForLevel(level)},
          {"name", QString::fromStdWString(level->getName())},
          {"format", formatName(*level)}};
}

int hookCount(const TXshSimpleLevel& level) {
  int count = 0;
  for (int index = 0; index < level.getHookSet()->getHookCount(); ++index)
    if (level.getHookSet()->getHook(index)) ++count;
  return count;
}

QJsonValue stageSize(const TXshSimpleLevel& level) {
  if (level.getType() == PLI_XSHLEVEL) return QJsonValue();
  const QJsonValue dpi = settingValue(level, "dpi");
  const TDimension res = level.getProperties()->getImageRes();
  if (dpi.toDouble() <= 0) return QJsonValue();
  return QJsonObject{{"width", res.lx / dpi.toDouble()},
                     {"height", res.ly / dpi.toDouble()}};
}

QString requestedName(Document& document, TXshSimpleLevel* level,
                      const QJsonObject& params) {
  if (!params.contains("name")) return QString();
  const QString name = nameParam(params);
  TXshLevel* holder =
      document.scene->getLevelSet()->getLevel(name.toStdWString());
  if (holder && holder != level)
    throw Error("name_conflict", "Another level already has that name.",
                {{"name", name}});
  return name;
}

void requireSettingsAdmitted(Document& document, TXshSimpleLevel* level,
                             const QJsonObject& settings) {
  for (auto entry = settings.begin(); entry != settings.end(); ++entry) {
    const QString reason = settingRefusal(document, *level, entry.key(), false);
    if (reason == "read_only_level")
      throw Error("locked", "The level is read-only.",
                  {{"setting", entry.key()}});
    if (!reason.isEmpty() && reason != "read_only_document")
      throw Error("unsupported_target",
                  "The setting does not apply to this level now.",
                  {{"setting", entry.key()},
                   {"format", formatName(*level)},
                   {"reason", reason}});
  }
}

// Premultiply and white transparency clear each other in the native popup;
// here turning one on while the other stays on needs an explicit coupling.
QStringList coupledClears(const TXshSimpleLevel& level,
                          const QJsonObject& settings,
                          const QString& coupling) {
  const bool premultiplyOn = settings.value("premultiply").toBool();
  const bool whiteOn       = settings.value("white_transparent").toBool();
  if (premultiplyOn && whiteOn)
    invalidParams("Premultiply and white transparency cannot both be on.",
                  "both_on");
  const LevelProperties& properties = *level.getProperties();
  const bool otherOn = (premultiplyOn && properties.whiteTransp() &&
                        !settings.contains("white_transparent")) ||
                       (whiteOn && properties.doPremultiply() &&
                        !settings.contains("premultiply"));
  if (!otherOn) return {};
  if (coupling != "clear_other")
    invalidParams(
        "The other of premultiply and white transparency is on; "
        "set coupling to clear_other to turn it off.",
        "coupling_required");
  return {premultiplyOn ? "white_transparent" : "premultiply"};
}

QJsonObject targetSettings(QJsonObject settings, const QStringList& cleared) {
  for (const QString& name : cleared) settings.insert(name, false);
  return settings;
}

QStringList changedSettings(const TXshSimpleLevel& level,
                            const QJsonObject& target) {
  QStringList changed;
  for (auto entry = target.begin(); entry != target.end(); ++entry) {
    const bool policy =
        entry.key() == "dpi" &&
        level.getProperties()->getDpiPolicy() != LevelProperties::DP_CustomDpi;
    if (policy || settingValue(level, entry.key()) != entry.value())
      changed.append(entry.key());
  }
  return changed;
}

void applySetting(TXshSimpleLevel& level, const QString& name,
                  const QJsonValue& value) {
  LevelProperties& properties = *level.getProperties();
  if (name == "dpi") {
    properties.setDpiPolicy(LevelProperties::DP_CustomDpi);
    properties.setDpi(TPointD(value.toDouble(), value.toDouble()));
  } else if (name == "premultiply") {
    properties.setDoPremultiply(value.toBool());
  } else if (name == "white_transparent") {
    properties.setWhiteTransp(value.toBool());
  } else if (name == "antialias_softness") {
    properties.setDoAntialias(value.toInt());
  } else if (name == "subsampling") {
    properties.setSubsampling(value.toInt());
    level.invalidateFrames();
  } else {
    properties.setColorSpaceGamma(value.toDouble());
  }
}

QJsonObject requestedSettings(const QJsonObject& params) {
  const QJsonObject settings = params.value("settings").toObject();
  if (settings.isEmpty())
    invalidParams("Name at least one setting.", "nothing_to_change");
  return settings;
}

QJsonArray topColumnIds(Document& document, const LevelUsage& usage) {
  QJsonArray ids;
  for (const LevelUsageColumn& column : usage.columns)
    if (!column.sheetLevel && ids.size() < MaxEntities)
      ids.append(document.idForColumn(column.sheet->getColumn(column.index)));
  return ids;
}

QJsonValue setLevelSettings(Context& context, const QJsonObject& params) {
  Document& document         = getDocument(context, params);
  const QJsonValue handle    = params.value("level_id");
  TXshSimpleLevel* level     = lifecycleLevel(document, handle.toString());
  const QJsonObject settings = requestedSettings(params);
  requireSettingsAdmitted(document, level, settings);
  const QStringList cleared =
      coupledClears(*level, settings, params.value("coupling").toString());
  const QJsonObject target  = targetSettings(settings, cleared);
  const QStringList changed = changedSettings(*level, target);
  const LevelUsage usage    = scanLevelUsage(document, level);
  if (!changed.isEmpty()) requireEditableUsage(document, usage);
  QJsonObject result{
      {"level_id", handle},
      {"settings_before", levelSettingsReport(document, level, usage)},
      {"stage_size_in_before", stageSize(*level)},
      {"changed", QJsonArray::fromStringList(changed)},
      {"coupled_cleared",
       cleared.isEmpty() ? QJsonValue() : QJsonValue(cleared.front())},
      {"exposures", usage.cells},
      {"columns", topColumnIds(document, usage)},
      {"hooks_moved_in_inches",
       changed.contains("dpi") ? hookCount(*level) : 0}};
  if (!changed.isEmpty()) context.markMutationStarted();
  for (const QString& setting : changed)
    applySetting(*level, setting, target.value(setting));
  result.insert("settings", levelSettingsReport(document, level, usage));
  result.insert("stage_size_in_after", stageSize(*level));
  result.insert("pixels_resampled", false);
  result.insert("camera_changed", false);
  return result;
}

// ------------------------------------------------------------ level.rename

QJsonValue renameLevel(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  TXshSimpleLevel* level =
      lifecycleLevel(document, params["level_id"].toString());
  const QString name   = requestedName(document, level, params);
  const QString before = QString::fromStdWString(level->getName());
  const bool renamed   = name != before;
  if (renamed) {
    context.markMutationStarted();
    if (!document.scene->getLevelSet()->renameLevel(level, name.toStdWString()))
      throw Error("native_error", "The native cast refused the new name.");
  }
  return QJsonObject{{"level", levelRecord(document, level)},
                     {"name_before", before},
                     {"renamed", renamed},
                     {"cast_id_kept", true}};
}

// --------------------------------------------------------- level.duplicate

QString levelExtension(const TXshSimpleLevel& level) {
  if (level.getType() == PLI_XSHLEVEL) return "pli";
  if (level.getType() == TZP_XSHLEVEL) return "tlv";
  return "png";
}

QStringList applicableSettings(const TXshSimpleLevel& level) {
  QStringList names;
  for (const SettingRule& rule : settingRules())
    if (rule.formats & formatBit(level)) names.append(rule.name);
  return names;
}

// A full copy outside the cast: drawings, an independent palette, every
// level option, hooks, tags and cover; the cast id is the copy's own.
std::unique_ptr<TXshSimpleLevel> copyLevel(Context& context, Document& document,
                                           TXshSimpleLevel& source,
                                           const QString& name) {
  auto copy = std::make_unique<TXshSimpleLevel>(name.toStdWString());
  copy->setScene(document.scene.get());
  copy->setType(source.getType());
  copy->setPath(newLevelPath(context, levelExtension(source)), true);
  *copy->getProperties() = *source.getProperties();
  TPalette* palette =
      source.getPalette() ? source.getPalette()->clone() : nullptr;
  if (palette) copy->setPalette(palette);
  for (const TFrameId& frame : source.getFids()) {
    const TImageP image = source.getFrame(frame, false);
    if (!image)
      throw Error("asset_unavailable", "A source drawing could not be read.",
                  {{"drawing_id", drawingId(frame)}});
    TImageP clone(image->cloneImage());
    if (palette) clone->setPalette(palette);
    copy->setFrame(frame, clone);
  }
  *copy->getHookSet() = *source.getHookSet();
  copy->setCastTags(source.getCastTags());
  if (source.hasCastCover()) copy->setCastCover(source.getCastCover());
  copy->setDirtyFlag(true);
  return copy;
}

qint64 levelBytes(TXshSimpleLevel& level) {
  qint64 bytes =
      level.getPalette() ? 2 * AutomationStyleBytes + AutomationPageBytes : 0;
  for (const TFrameId& frame : level.getFids())
    bytes += automationImageBytes(level.getFrame(frame, false));
  return bytes;
}

// Register the copy, then require the scene's hook budgets; a refusal takes
// the copy out again, so nothing has changed.
void insertCopy(Context& context, Document& document, TXshSimpleLevel* copy) {
  TLevelSet* cast = document.scene->getLevelSet();
  if (!cast->insertLevel(copy))
    throw Error("native_error", "The native cast refused the copy.");
  try {
    validateAutomationHooks(*document.scene);
  } catch (...) {
    cast->removeLevel(copy, false);
    throw;
  }
  context.markMutationStarted();
}

QJsonValue duplicateLevel(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  TXshSimpleLevel* source =
      lifecycleLevel(document, params["level_id"].toString());
  const QString name = nameParam(params);
  if (document.scene->getLevelSet()->getLevel(name.toStdWString()))
    throw Error("name_conflict", "A level with that name already exists.",
                {{"name", name}});
  requireAssetGrowth(*document.scene, levelBytes(*source),
                     source->getFrameCount(), 1);
  auto copy = copyLevel(context, document, *source, name);
  insertCopy(context, document, copy.get());
  TXshSimpleLevel* result = copy.release();
  QJsonObject level       = levelRecord(document, result);
  level.insert("drawing_count", result->getFrameCount());
  return QJsonObject{
      {"level", level},
      {"source_level_id", params["level_id"]},
      {"settings_copied",
       QJsonArray::fromStringList(applicableSettings(*source))},
      {"hooks_copied", hookCount(*result)},
      {"palette", result->getPalette() ? "cloned" : "none"},
      {"cast_metadata",
       QJsonObject{{"cast_id", "new"},
                   {"tags", "copied"},
                   {"cover", source->hasCastCover() ? "copied" : "none"},
                   {"folder", "default"},
                   {"collections", "none"}}},
      {"exposed", false}};
}

// ------------------------------------------------------------ level.remove

void requirePolicyFields(const QJsonObject& params, const QString& policy) {
  const bool replace = policy == "replace";
  if (replace != params.contains("replacement_level_id") ||
      (!replace && params.contains("allow_type_change")))
    invalidParams(
        "replacement_level_id is required with replace and "
        "allowed only there, as is allow_type_change.",
        "policy_fields");
}

std::set<TFrameId> exposedFrames(const LevelUsage& usage) {
  std::set<TFrameId> frames;
  for (const LevelUsageColumn& column : usage.columns)
    for (int row : column.rows)
      frames.insert(column.column->getCell(row).m_frameId);
  return frames;
}

void requireReplacement(TXshSimpleLevel& level, TXshSimpleLevel& replacement,
                        const LevelUsage& usage, bool allowTypeChange) {
  if (&level == &replacement)
    invalidParams("Replace a level by a different level.", "same_level");
  if (level.getType() != replacement.getType() && !allowTypeChange)
    throw Error("unsupported_target",
                "The replacement has another format; set allow_type_change "
                "to accept that its exposures render as that format.",
                {{"reason", "type_change"},
                 {"format", formatName(level)},
                 {"replacement_format", formatName(replacement)}});
  QJsonArray missing;
  for (const TFrameId& frame : exposedFrames(usage))
    if (!replacement.isFid(frame) && missing.size() < MaxUsageRows)
      missing.append(drawingId(frame));
  if (!missing.isEmpty())
    throw Error("missing_drawing",
                "The replacement lacks drawings the level's exposures show.",
                {{"drawings", missing}});
}

int hookNumber(const std::string& handle) {
  if (handle.size() < 2 || handle[0] != 'H') return 0;
  bool ok          = false;
  const int number = QString::fromStdString(handle.substr(1)).toInt(&ok);
  return ok ? number : 0;
}

void appendHookUser(Document& document, const LevelUsageColumn& column,
                    TStageObject& object, int number, QJsonArray& users) {
  if (users.size() == MaxUsageRows) return;
  users.append(QJsonObject{
      {"sheet_level_id",
       column.sheetLevel ? QJsonValue(document.idForLevel(column.sheetLevel))
                         : QJsonValue()},
      {"column_index", column.index},
      {"object_id", column.sheetLevel
                        ? QJsonValue()
                        : QJsonValue(document.idForObject(&object))},
      {"hook_number", number}});
}

// Objects attached to a hook of the level's exposures: the column itself by
// its own handle, or a child by its parent handle. A replacement keeps them
// only when it defines the same hook.
QJsonArray hookUsers(Document& document, const LevelUsage& usage,
                     TXshSimpleLevel* replacement) {
  QJsonArray users;
  for (const LevelUsageColumn& column : usage.columns) {
    TStageObjectTree* tree = column.sheet->getStageObjectTree();
    TStageObject* owner =
        tree->getStageObject(TStageObjectId::ColumnId(column.index), false);
    if (!owner) continue;
    std::vector<std::pair<TStageObject*, int>> attached{
        {owner, hookNumber(owner->getHandle())}};
    for (TStageObject* child : owner->getChildren())
      attached.emplace_back(child, hookNumber(child->getParentHandle()));
    for (const auto& [object, number] : attached)
      if (number > 0 &&
          !(replacement && automationHookDefined(*replacement, number)))
        appendHookUser(document, column, *object, number, users);
  }
  return users;
}

int dormantMemberships(Document& document, const TXshLevel& level) {
  int count = 0;
  for (const auto& collection :
       document.scene->getProperties()->getCastCollections())
    if (collection.members.contains(level.getCastId())) ++count;
  return count;
}

void eraseLevelHandles(Document& document, TXshLevel* level) {
  for (auto entry = document.levels.begin(); entry != document.levels.end();) {
    if (entry.value() == level)
      entry = document.levels.erase(entry);
    else
      ++entry;
  }
}

void requireRemovable(Document& document, const LevelUsage& usage,
                      const QString& policy, TXshSimpleLevel* replacement) {
  if (!usage.complete) requireEditableUsage(document, usage);
  if (policy == "refuse" && usage.cells > 0)
    throw Error("in_use",
                "The level is exposed; choose clear or replace for its "
                "exposures.",
                {{"usage", levelUsageJson(document, usage)}});
  requireEditableUsage(document, usage);
  const QJsonArray users = hookUsers(document, usage, replacement);
  if (!users.isEmpty())
    throw Error("has_dependents",
                "Objects are attached to hooks of the level's exposures; "
                "the removal would move them.",
                {{"users", users}});
}

QJsonValue removeLevel(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  TXshSimpleLevel* level =
      lifecycleLevel(document, params["level_id"].toString());
  const QString policy = params.value("exposure_policy").toString();
  requirePolicyFields(params, policy);
  TXshSimpleLevel* replacement =
      policy == "replace"
          ? lifecycleLevel(document, params["replacement_level_id"].toString())
          : nullptr;
  const LevelUsage usage = scanLevelUsage(document, level);
  if (replacement)
    requireReplacement(*level, *replacement, usage,
                       params.value("allow_type_change").toBool());
  requireRemovable(document, usage, policy, replacement);
  QJsonObject result{
      {"removed_level_id", params["level_id"]},
      {"name", QString::fromStdWString(level->getName())},
      {"exposure_policy", policy},
      {"usage_before", levelUsageJson(document, usage)},
      {"replacement_level_id",
       replacement ? params["replacement_level_id"] : QJsonValue()},
      {"type_changed",
       replacement && replacement->getType() != level->getType()},
      {"collection_memberships_dormant", dormantMemberships(document, *level)},
      {"files_deleted", 0},
      {"cast_ids_removed", 1}};
  context.markMutationStarted();
  const qint64 rewritten = rewriteLevelUsage(usage, replacement);
  result.insert("cells_cleared", replacement ? 0 : rewritten);
  result.insert("cells_replaced", replacement ? rewritten : 0);
  eraseLevelHandles(document, level);
  TLevelSet* cast = document.scene->getLevelSet();
  cast->removeLevel(level);
  for (int index = 0; index < cast->getLevelCount(); ++index)
    if (cast->getLevel(index) == level)
      throw Error("native_error", "The native cast kept the removed level.");
  return result;
}

QJsonObject settingsSchema() {
  return {{"type", "object"},
          {"additionalProperties", false},
          {"properties",
           QJsonObject{{"dpi", numberSchema(1, 2400)},
                       {"premultiply", QJsonObject{{"type", "boolean"}}},
                       {"white_transparent", QJsonObject{{"type", "boolean"}}},
                       {"antialias_softness", integerSchema(0, 100)},
                       {"subsampling", integerSchema(1, 100)},
                       {"color_space_gamma", numberSchema(0.1, 10)}}}};
}

QJsonObject handleSchema() {
  return {{"type", "string"},
          {"minLength", 1},
          {"maxLength", 128},
          {"pattern", "^[A-Za-z0-9_-]+$"}};
}

QJsonObject nameSchema() { return boundedNameSchema(128); }

}  // namespace

QJsonArray levelLifecycleOperations() {
  QJsonArray catalog;
  catalog.append(operation(
      "level.settings.set",
      "Change a level's interpretation settings: custom DPI, premultiply, "
      "white as transparent, antialias softness, subsampling and colour-space "
      "gamma, each only where the native level type and state admit it "
      "(level.inspect reports which). Turning premultiply or white "
      "transparency on while the other is on needs coupling clear_other. "
      "Never resamples pixels and never changes the camera.",
      {{"document_id", textSchema()},
       {"level_id", handleSchema()},
       {"settings", settingsSchema()},
       {"coupling", enumSchema({"none", "clear_other"})}},
      {"document_id", "level_id", "settings"}, true));
  catalog.append(operation(
      "level.rename",
      "Rename a level. Its handle, cast id and exposures stay; the name must "
      "not be another level's. Admitted under a locked exposure, since no "
      "picture changes.",
      {{"document_id", textSchema()},
       {"level_id", handleSchema()},
       {"name", nameSchema()}},
      {"document_id", "level_id", "name"}, true));
  catalog.append(operation(
      "level.duplicate",
      "Duplicate a level into a new, independent level: every drawing, a "
      "cloned palette, every setting, the hooks, tags and cover; a new cast "
      "id; exposed nowhere. Retarget exposures with cells.replace.",
      {{"document_id", textSchema()},
       {"level_id", handleSchema()},
       {"name", nameSchema()}},
      {"document_id", "level_id", "name"}, true));
  catalog.append(operation(
      "level.remove",
      "Remove a level from the cast with an explicit plan for its exposures "
      "in every sheet: refuse while it is exposed (the refusal lists them), "
      "clear them without shifting later cells, or replace them by the same "
      "drawings of another level. Never deletes a file.",
      {{"document_id", textSchema()},
       {"level_id", handleSchema()},
       {"exposure_policy", enumSchema({"refuse", "clear", "replace"})},
       {"replacement_level_id", handleSchema()},
       {"allow_type_change", QJsonObject{{"type", "boolean"}}}},
      {"document_id", "level_id", "exposure_policy"}, true));
  return catalog;
}

QJsonValue executeLevelLifecycle(Context& context, const QString& method,
                                 const QJsonObject& params) {
  static const std::map<QString, SceneShared::SceneHandler> handlers{
      {"level.settings.set", setLevelSettings},
      {"level.rename", renameLevel},
      {"level.duplicate", duplicateLevel},
      {"level.remove", removeLevel}};
  const auto handler = handlers.find(method);
  if (handler == handlers.end())
    throw Error("unknown_method", "Unknown level operation: " + method);
  return handler->second(context, params);
}

}  // namespace GroundzeroAutomation
