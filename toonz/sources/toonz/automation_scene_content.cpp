#include "automation_scene_host.h"
#include "automation_fx.h"
#include "automation_fx_profile.h"

#include "texternfx.h"
#include "tfx.h"
#include "tseexprfx.h"
#include "tmacrofx.h"
#include "tnotanimatableparam.h"
#include "tparamcontainer.h"
#include "toonz/fxdag.h"
#include "toonz/levelset.h"
#include "toonz/sceneproperties.h"
#include "toonz/sceneresources.h"
#include "toonz/tcamera.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"

#include <QFileInfo>

#include <cmath>
#include <deque>
#include <map>
#include <set>

namespace GroundzeroAutomation {
namespace {

struct SheetEntry final {
  TXsheet* sheet;
  QJsonArray path;
};

struct SceneIndex final {
  std::vector<SheetEntry> sheets;
  std::map<TXsheet*, size_t> positions;
  std::map<TXshColumn*, std::vector<TXshLevel*>> columnLevels;
};

QJsonArray childPath(Document* document, const QJsonArray& path,
                     TXshColumn* column, TXshLevel* level) {
  QJsonArray result = path;
  result.append(QJsonObject{
      {"column_id", document ? document->idForColumn(column) : QString("-")},
      {"level_id", document ? document->idForLevel(level) : QString("-")}});
  return result;
}

SceneIndex indexScene(ToonzScene& scene, Document* document) {
  SceneIndex index;
  std::deque<SheetEntry> pending{{scene.getTopXsheet(), {}}};
  while (!pending.empty()) {
    const SheetEntry entry = pending.front();
    pending.pop_front();
    if (!entry.sheet || index.positions.count(entry.sheet)) continue;
    index.positions[entry.sheet] = index.sheets.size();
    index.sheets.push_back(entry);
    for (int column = 0; column < entry.sheet->getColumnCount(); ++column) {
      TXshColumn* native = entry.sheet->getColumn(column);
      if (!native) continue;
      auto levels                = columnLevels(native);
      index.columnLevels[native] = levels;
      for (TXshLevel* level : levels)
        if (TXshChildLevel* child = level->getChildLevel())
          pending.push_back({child->getXsheet(),
                             childPath(document, entry.path, native, level)});
    }
  }
  return index;
}

std::set<TFx*> sheetMembers(TXsheet* sheet) {
  std::set<TFx*> members;
  FxDag* graph = sheet->getFxDag();
  graph->getInternalFxs()->getFxs(members);
  members.insert(graph->getXsheetFx());
  for (int index = 0; index < graph->getOutputFxCount(); ++index)
    members.insert(graph->getOutputFx(index));
  for (int index = 0; index < sheet->getColumnCount(); ++index) {
    TXshColumn* column = sheet->getColumn(index);
    if (column && column->getFx()) members.insert(column->getFx());
  }
  members.erase(nullptr);
  return members;
}

// Effect nodes, macro members included; column FX are bounded by the column
// limit instead, and a generator column counts its generator.
size_t effectCount(const std::set<TFx*>& members) {
  size_t count = 0;
  for (TFx* fx : members) {
    if (auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx)) {
      count += wrapper->getZeraryFx() ? 1 : 0;
      continue;
    }
    if (dynamic_cast<TColumnFx*>(fx)) continue;
    ++count;
    if (auto* macro = dynamic_cast<TMacroFx*>(fx))
      count += macro->getFxs().size();
  }
  return count;
}

int sheetDepth(const SceneIndex& index, TXsheet* sheet,
               std::set<TXsheet*>& stack, int depth) {
  if (depth > SheetDepthLimit)
    throw Error("scene_limit", "Sub-xsheets nest deeper than 16 levels.",
                {{"limit", "sheet_depth"}, {"value", depth}});
  if (stack.count(sheet))
    throw Error("invalid_scene", "A sub-xsheet contains itself.",
                {{"cause", "rejected"},
                 {"native_message", "cyclic sub-xsheet reference"}});
  stack.insert(sheet);
  int deepest = depth;
  for (int column = 0; column < sheet->getColumnCount(); ++column) {
    const auto found = index.columnLevels.find(sheet->getColumn(column));
    if (found == index.columnLevels.end()) continue;
    for (TXshLevel* level : found->second)
      if (TXshChildLevel* child = level->getChildLevel())
        deepest = std::max(
            deepest, sheetDepth(index, child->getXsheet(), stack, depth + 1));
  }
  stack.erase(sheet);
  return deepest;
}

QString fxType(TFx* fx) { return QString::fromStdString(fx->getFxType()); }

bool hasFileParameter(TFx* fx) {
  TParamContainer* params = fx->getParams();
  for (int index = 0; index < params->getParamCount(); ++index)
    if (dynamic_cast<TFilePathParam*>(params->getParam(index))) return true;
  return false;
}

// An effect type is built in when the application's own code declares it, and
// the registry still holds that declaration for the type: a plug-in effect, a
// shader effect or anything else registered at run time is not, and neither is
// a type the registry does not know.
bool declaredByApplication(TFx* fx) {
  const TPersistDeclaration* declared = fx->getDeclaration();
  if (!declared) return false;
  const auto* declaration = dynamic_cast<const TFxDeclaration*>(declared);
  if (!declaration || !declaration->isCompiledIn()) return false;
  return TFx::getFxInfo(declared->getId()).m_name == declared->getId();
}

// Compiled-in effects that run a program the scene or the user supplies: an
// expression document, or the neural worker with a style model.
bool runsOutsideProgram(TFx* fx) {
  return dynamic_cast<TFxSeExprSource*>(fx) ||
         fxType(fx) == "STD_neuralStyleFx";
}

// Compiled-in effects that read a file through a plain string parameter or a
// cache, which no parameter type shows.
bool readsNamedFile(TFx* fx) {
  static const QStringList files = {"STD_particleCacheFx", "STD_openExrAovFx",
                                    "STD_glbModelFx", "STD_lut3DBakeFx",
                                    "STD_referenceColorMatchFx"};
  return files.contains(fxType(fx)) || dynamic_cast<SceneFxResource*>(fx) ||
         hasFileParameter(fx);
}

QString effectClass(TFx* fx) {
  if (dynamic_cast<TExternalProgramFx*>(fx) || !declaredByApplication(fx) ||
      runsOutsideProgram(fx))
    return "external_code";
  if (readsNamedFile(fx)) return "file_resource";
  QStringList violations;
  return automationFxProfileViolations(fx, violations) ? "builtin_admitted"
                                                       : "builtin_other";
}

// The class of a graph node; a macro is classified by its worst member.
QString nodeClass(TFx* fx) {
  if (dynamic_cast<TXsheetFx*>(fx)) return "xsheet";
  if (dynamic_cast<TOutputFx*>(fx)) return "output";
  if (auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx))
    return wrapper->getZeraryFx() ? effectClass(wrapper->getZeraryFx())
                                  : QString("unknown");
  if (dynamic_cast<TColumnFx*>(fx)) return "column";
  if (dynamic_cast<TMacroFx*>(fx)) return "macro";
  return effectClass(fx);
}

QString memberReason(const QString& kind) {
  if (kind == "external_code" || kind == "file_resource") return kind;
  if (kind == "builtin_other") return "unsupported_fx";
  if (kind == "unknown") return "unsupported_fx";
  return QString();
}

QStringList macroReasons(TMacroFx* macro) {
  QStringList reasons;
  for (const auto& member : macro->getFxs()) {
    const QString reason = memberReason(effectClass(member.getPointer()));
    if (!reason.isEmpty() && !reasons.contains(reason)) reasons.append(reason);
  }
  return reasons;
}

QStringList nodeReasons(TFx* fx) {
  if (auto* macro = dynamic_cast<TMacroFx*>(fx)) return macroReasons(macro);
  const QString reason = memberReason(nodeClass(fx));
  return reason.isEmpty() ? QStringList() : QStringList{reason};
}

ContentRow::Rule ruleFor(const QString& reason) {
  if (reason == "external_code" || reason == "file_resource" ||
      reason == "external_file_reference")
    return ContentRow::Rule::Anywhere;
  return ContentRow::Rule::Rendered;
}

void addFxRows(Document& document, const SheetEntry& entry) {
  ReadOnlyScene& source = *document.readOnly;
  for (TFx* fx : sheetMembers(entry.sheet))
    for (const QString& reason : nodeReasons(fx)) {
      ContentRow row;
      row.kind      = "fx";
      row.reason    = reason;
      row.name      = QString::fromStdWString(fx->getName());
      row.detail    = "Type " + fxType(fx) + ".";
      row.fxId      = document.idForFx(fx);
      row.fx        = fx;
      row.sheetPath = entry.path;
      row.rule      = ruleFor(reason);
      source.rows.push_back(row);
    }
}

void addColumnRows(Document& document, const SheetEntry& entry) {
  for (int index = 0; index < entry.sheet->getColumnCount(); ++index) {
    TXshColumn* column = entry.sheet->getColumn(index);
    if (!column) continue;
    QString detail;
    auto rule = ContentRow::Rule::Rendered;
    if (column->getPaletteColumn()) detail = "Palette column.";
    if (column->getMeshColumn()) detail = "Mesh column.";
    if (column->getSoundTextColumn()) {
      detail = "Sound text column; it draws nothing in renders.";
      rule   = ContentRow::Rule::Never;
    }
    if (detail.isEmpty()) continue;
    ContentRow row;
    row.kind      = "column";
    row.reason    = "unsupported_column";
    row.detail    = detail;
    row.columnId  = document.idForColumn(column);
    row.column    = column;
    row.sheetPath = entry.path;
    row.rule      = rule;
    document.readOnly->rows.push_back(row);
  }
}

bool finitePositive(double value) { return std::isfinite(value) && value > 0; }

void addSceneRows(Document& document) {
  ReadOnlyScene& source = *document.readOnly;
  for (const std::string& tag : document.scene->getUnrecognizedSceneTags()) {
    ContentRow row;
    row.kind   = "scene";
    row.reason = "unrecognized_scene_tag";
    row.name   = QString::fromStdString(tag);
    row.detail = "Skipped top-level scene data.";
    row.rule   = ContentRow::Rule::Never;
    source.rows.push_back(row);
  }
  TCamera* camera   = document.scene->getCurrentCamera();
  const TPointD dpi = camera->getDpi();
  const bool valid  = camera->getRes().lx > 0 && camera->getRes().ly > 0 &&
                      finitePositive(camera->getSize().lx) &&
                      finitePositive(camera->getSize().ly) &&
                      finitePositive(dpi.x) && finitePositive(dpi.y);
  if (valid) return;
  ContentRow row;
  row.kind   = "scene";
  row.reason = "invalid_camera";
  row.detail = "The camera size or DPI is not finite and positive.";
  row.rule   = ContentRow::Rule::Camera;
  source.rows.push_back(row);
}

void addLibraryRows(Document& document) {
  for (TXshLevel* level : distinctLevels(document.scene->getLevelSet())) {
    if (level->getLibraryProvenance().relationship !=
        TXshLevel::LibraryProvenance::Linked)
      continue;
    ContentRow row;
    row.kind    = "level";
    row.reason  = "linked_library_origin";
    row.name    = QString::fromStdWString(level->getName());
    row.detail  = "Linked Library origin, reported as metadata.";
    row.levelId = document.idForLevel(level);
    row.level   = level;
    row.rule    = ContentRow::Rule::Never;
    document.readOnly->rows.push_back(row);
  }
}

// ------------------------------------------------------------- reachability

struct Reach final {
  std::set<TFx*> effects;
  std::set<TXshColumn*> columns;
  std::set<TXshLevel*> levels;
  std::set<TXsheet*> sheets;
};

void reachSheet(const SceneIndex& index, TXsheet* sheet, TFx* start,
                Reach& reach);

void reachColumn(const SceneIndex& index, TColumnFx* fx, Reach& reach) {
  TXshColumn* column = fx->getXshColumn();
  if (!column || !column->isPreviewVisible()) return;
  reach.columns.insert(column);
  if (auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx))
    if (wrapper->getZeraryFx()) reach.effects.insert(wrapper->getZeraryFx());
  const auto found = index.columnLevels.find(column);
  if (found == index.columnLevels.end()) return;
  for (TXshLevel* level : found->second) {
    reach.levels.insert(level);
    TXshChildLevel* child = level->getChildLevel();
    if (child && child->getXsheet())
      reachSheet(index, child->getXsheet(),
                 child->getXsheet()->getFxDag()->getXsheetFx(), reach);
  }
}

void pushInputs(TFx* fx, std::vector<TFx*>& pending) {
  for (int port = 0; port < fx->getInputPortCount(); ++port)
    if (TFx* input = fx->getInputPort(port)->getFx()) pending.push_back(input);
}

void reachSheet(const SceneIndex& index, TXsheet* sheet, TFx* start,
                Reach& reach) {
  if (!sheet || !reach.sheets.insert(sheet).second) return;
  FxDag* graph = sheet->getFxDag();
  std::vector<TFx*> pending{start};
  while (!pending.empty()) {
    TFx* fx = pending.back();
    pending.pop_back();
    if (!fx || !reach.effects.insert(fx).second) continue;
    if (fx == graph->getXsheetFx()) {
      TFxSet* terminals = graph->getTerminalFxs();
      for (int item = 0; item < terminals->getFxCount(); ++item)
        pending.push_back(terminals->getFx(item));
      continue;
    }
    if (auto* column = dynamic_cast<TColumnFx*>(fx)) {
      reachColumn(index, column, reach);
      continue;
    }
    pushInputs(fx, pending);
  }
}

Reach reachOutput(Document& document, const SceneIndex& index) {
  Reach reach;
  TXsheet* top  = document.scene->getTopXsheet();
  TFx* output   = top->getFxDag()->getCurrentOutputFx();
  TFx* rendered = output && output->getInputPortCount() > 0
                      ? output->getInputPort(0)->getFx()
                      : nullptr;
  if (rendered) reachSheet(index, top, rendered, reach);
  return reach;
}

bool reached(const ContentRow& row, const Reach& reach) {
  if (row.fx) return reach.effects.count(row.fx) > 0;
  if (row.column) return reach.columns.count(row.column) > 0;
  if (row.level) return reach.levels.count(row.level) > 0;
  return false;
}

QJsonObject issueRow(const QString& code, const QString& severity,
                     const ContentRow& row) {
  return {
      {"code", code},
      {"severity", severity},
      {"level_id",
       row.levelId.isEmpty() ? QJsonValue() : QJsonValue(row.levelId)},
      {"fx_id", row.fxId.isEmpty() ? QJsonValue() : QJsonValue(row.fxId)},
      {"column_id",
       row.columnId.isEmpty() ? QJsonValue() : QJsonValue(row.columnId)},
      {"column_index", QJsonValue()},
      {"first_frame", QJsonValue()},
      {"last_frame", QJsonValue()},
      {"drawing_id", QJsonValue()},
      {"message", (row.detail.isEmpty() ? row.reason : row.detail).left(512)}};
}

QString issueCode(const QString& reason) {
  static const QMap<QString, QString> codes = {
      {"unsupported_column", "unsupportedColumn"},
      {"external_code", "externalCodeFx"},
      {"file_resource", "fileResourceFx"},
      {"missing_file", "missingDependency"},
      {"forbidden_file", "forbiddenDependency"},
      {"external_file_reference", "forbiddenDependency"},
      {"level_load_failed", "levelLoadFailed"},
      {"partial_level", "levelLoadFailed"},
      {"unsupported_samples", "levelLoadFailed"},
      {"creator_forbidden", "levelLoadFailed"},
      {"image_too_large", "levelLoadFailed"},
      {"unrecognized_scene_tag", "unrecognizedSceneData"}};
  return codes.value(reason);
}

}  // namespace

std::vector<TXshLevel*> columnLevels(TXshColumn* column) {
  std::vector<TXshLevel*> result;
  if (!column || column->getSoundColumn() || column->getZeraryFxColumn())
    return result;
  TXshCellColumn* cells = column->getCellColumn();
  if (!cells) return result;
  int first = 0, last = -1;
  column->getRange(first, last);
  std::set<TXshLevel*> seen;
  TXshLevel* previous = nullptr;
  for (int row = std::max(first, 0); row <= last; ++row) {
    TXshLevel* level = cells->getCell(row).m_level.getPointer();
    if (!level || level == previous) continue;
    previous = level;
    if (seen.insert(level).second) result.push_back(level);
  }
  return result;
}

void requireReadOnlyStructure(ToonzScene& scene) {
  const SceneIndex index = indexScene(scene, nullptr);
  if (index.sheets.size() > size_t(SheetLimit))
    throw Error("scene_limit", "The scene has more than 1024 sheets.",
                {{"limit", "sheets"}, {"value", int(index.sheets.size())}});
  std::set<TXsheet*> stack;
  sheetDepth(index, scene.getTopXsheet(), stack, 0);
  size_t total = 0;
  for (const SheetEntry& entry : index.sheets) {
    if (entry.sheet->getColumnCount() >= TXsheet::maxColumnCount)
      throw Error("scene_limit", "A sheet has 10,000 or more columns.",
                  {{"limit", "columns_per_sheet"},
                   {"value", entry.sheet->getColumnCount()}});
    const size_t nodes = effectCount(sheetMembers(entry.sheet));
    total += nodes;
    if (nodes > size_t(FxNodesPerSheetLimit))
      throw Error("scene_limit",
                  "A sheet has more than 4096 effects besides its column FX.",
                  {{"limit", "fx_nodes_per_sheet"}, {"value", int(nodes)}});
    const int objects =
        entry.sheet->getStageObjectTree()->getStageObjectCount();
    if (objects > StageObjectsPerSheetLimit)
      throw Error("scene_limit", "A sheet has more than 16,384 stage objects.",
                  {{"limit", "stage_objects_per_sheet"}, {"value", objects}});
  }
  if (total > size_t(FxNodesPerSceneLimit))
    throw Error("scene_limit",
                "The scene has more than 16,384 effects besides its column FX.",
                {{"limit", "fx_nodes"}, {"value", int(total)}});
}

QJsonObject readOnlyCounts(Document& document) {
  const SceneIndex index = indexScene(*document.scene, nullptr);
  int columns = 0, effects = 0, objects = 0, sounds = 0;
  for (const SheetEntry& entry : index.sheets) {
    columns += entry.sheet->getColumnCount();
    effects += int(effectCount(sheetMembers(entry.sheet)));
    objects += entry.sheet->getStageObjectTree()->getStageObjectCount();
  }
  const auto levels = distinctLevels(document.scene->getLevelSet());
  for (TXshLevel* level : levels)
    if (level->getSoundLevel()) ++sounds;
  return {{"sheets", int(index.sheets.size())},
          {"levels", int(levels.size())},
          {"columns", columns},
          {"fx_nodes", effects},
          {"frames", document.scene->getFrameCount()},
          {"drawings", 0},
          {"sound_levels", sounds},
          {"stage_objects", objects}};
}

void classifyReadOnlyContent(Document& document) {
  const SceneIndex index = indexScene(*document.scene, &document);
  addSceneRows(document);
  addLibraryRows(document);
  for (const SheetEntry& entry : index.sheets) {
    addColumnRows(document, entry);
    addFxRows(document, entry);
  }
}

std::vector<ContentRow> renderBlockers(Document& document) {
  std::vector<ContentRow> blockers;
  if (!document.readOnly) return blockers;
  const SceneIndex index = indexScene(*document.scene, nullptr);
  const Reach reach      = reachOutput(document, index);
  for (const ContentRow& row : document.readOnly->rows) {
    if (row.rule == ContentRow::Rule::Never) continue;
    if (row.rule == ContentRow::Rule::Rendered && !reached(row, reach))
      continue;
    blockers.push_back(row);
  }
  return blockers;
}

QJsonObject renderReport(Document& document) {
  const std::vector<ContentRow> blockers = renderBlockers(document);
  QJsonArray rows;
  for (const ContentRow& row : blockers)
    if (rows.size() < BlockerRowLimit) rows.append(row.blockerRow());
  return {{"renderable", blockers.empty()},
          {"blockers", rows},
          {"blocker_count", int(blockers.size())},
          {"snapshot_inputs", linkedSnapshotInputs()},
          {"formats", QJsonArray{"png"}}};
}

QString fxNodeClass(TFx* fx) { return nodeClass(fx); }

QJsonObject fxAdmission(Document& document, TFx* fx) {
  const QString kind  = nodeClass(fx);
  QStringList reasons = nodeReasons(fx);
  QStringList violations;
  TFx* native = fx;
  if (auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx))
    native = wrapper->getZeraryFx();
  if (document.readOnly && native && kind == "builtin_admitted")
    automationFxProfileViolations(native, violations);
  const bool editable =
      !document.readOnly && reasons.isEmpty() && kind != "builtin_other";
  return {{"editable", editable},
          {"render", reasons.isEmpty() ? "supported" : "blocked"},
          {"class", kind},
          {"reasons", QJsonArray::fromStringList(reasons.mid(0, 8))},
          {"authoring_range_violations",
           QJsonArray::fromStringList(violations.mid(0, 16))}};
}

bool fxAdmitted(TFx* fx) {
  const QString kind = nodeClass(fx);
  return nodeReasons(fx).isEmpty() && kind != "builtin_other" &&
         kind != "unknown";
}

QJsonArray readOnlyIssues(Document& document) {
  QJsonArray issues;
  if (!document.readOnly) return issues;
  for (const ContentRow& row : document.readOnly->rows) {
    const QString code = issueCode(row.reason);
    if (code.isEmpty()) continue;
    QString severity = "error";
    if (code == "unsupportedColumn" || code == "externalCodeFx" ||
        code == "fileResourceFx" || code == "unrecognizedSceneData")
      severity = "warning";
    issues.append(issueRow(code, severity, row));
  }
  const SceneIndex index = indexScene(*document.scene, nullptr);
  for (TFx* fx : sheetMembers(index.sheets.front().sheet)) {
    TFx* native = fx;
    if (auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx))
      native = wrapper->getZeraryFx();
    QStringList violations;
    if (!native || nodeClass(fx) != "builtin_admitted" ||
        !automationFxProfileViolations(native, violations) ||
        violations.isEmpty())
      continue;
    ContentRow row;
    row.fxId = document.idForFx(fx);
    row.detail =
        "Values outside the authoring ranges: " + violations.join(", ");
    issues.append(issueRow("authoringRangeExceeded", "info", row));
  }
  return issues;
}

}  // namespace GroundzeroAutomation
