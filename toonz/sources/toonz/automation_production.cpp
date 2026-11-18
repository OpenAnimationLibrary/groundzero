#include "automation_production.h"
#include "automation_files.h"
#include "automation_fx.h"
#include "automation_scene_host.h"

#include "tcolorstyles.h"
#include "tfilepath.h"
#include "tfx.h"
#include "tmacrofx.h"
#include "tpalette.h"
#include "tsystem.h"
#include "toonz/fxdag.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toutputproperties.h"
#include "toonz/sceneresources.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QSet>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <set>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxLevels   = 256;
constexpr int MaxEntities = 4096;
constexpr int MaxCells    = 100000;
constexpr int MaxRecords  = 4096;
constexpr int MaxResponse = 1048576;

QString boundedText(const QString& value, int maximum = 4096) {
  if (value.size() > maximum)
    throw Error("resource_limit",
                "Native metadata exceeds the inspection string limit.");
  return value;
}

QJsonObject limits(int levels) {
  return {{"levels", levels},
          {"columns", MaxEntities},
          {"fx_nodes", MaxEntities},
          {"scanned_cells", MaxCells},
          {"result_records", MaxRecords},
          {"response_bytes", MaxResponse}};
}

QJsonArray exclusions() {
  return {"nested_sheet_expansion",     "manifest_expansion",
          "sidecar_discovery",          "image_loading",
          "unused_style_scan",          "codec_probe",
          "compatibility_certification"};
}

struct Report final {
  Context& context;
  Document& document;
  QString method;
  QJsonArray rows;
  QJsonArray reasons;
  QMap<QString, TFx*> effects;
  QSet<TFx*> seenEffects;
  QSet<TFx*> qualifiedMacros;
  QHash<TXshLevel*, QString> levelIds;
  std::vector<TXshLevel*> levels;
  QSet<TXshLevel*> used;
  int columnCount = 0;
  int cells       = 0;
  int macroMembers = 0;
  int recordLimit  = MaxRecords;

  Report(Context& owner, Document& value, QString operation)
      : context(owner), document(value), method(std::move(operation)) {}

  bool readOnly() const { return document.readOnly != nullptr; }
  int levelLimit() const { return readOnly() ? LevelLimit : MaxLevels; }

  void incomplete(const QString& reason) {
    if (!reasons.contains(reason)) reasons.append(reason);
  }

  bool append(const QJsonObject& row) {
    if (rows.size() == recordLimit) {
      incomplete("record_limit");
      return false;
    }
    rows.append(row);
    return true;
  }

  bool validates() const { return method == "scene.validate"; }
};

QString levelFormat(TXshLevel* level) {
  if (!level->getSimpleLevel()) return "unsupported";
  switch (level->getType()) {
  case PLI_XSHLEVEL:
    return "vector";
  case OVL_XSHLEVEL:
    return "rgba8";
  case TZP_XSHLEVEL:
    return "toonzRaster";
  default:
    return "unsupported";
  }
}

QJsonObject issue(const QString& code, const QString& message) {
  QString severity = "error";
  if (code == "emptyColumn" || code == "unusedLevel") severity = "info";
  if (code.startsWith("unsupported")) severity = "warning";
  return {{"code", code},
          {"severity", severity},
          {"level_id", QJsonValue()},
          {"fx_id", QJsonValue()},
          {"column_id", QJsonValue()},
          {"column_index", QJsonValue()},
          {"first_frame", QJsonValue()},
          {"last_frame", QJsonValue()},
          {"drawing_id", QJsonValue()},
          {"message", message}};
}

void unsupportedLevel(Report& report, TXshLevel* level) {
  report.incomplete("unsupported_level");
  if (level->getChildLevel()) report.incomplete("unsupported_nested_sheet");
  if (!report.validates()) return;
  auto row = issue("unsupportedLevel",
                   "Level is outside the detached inspection profile.");
  row.insert("level_id", report.levelIds.value(level));
  report.append(row);
}

void collectLevels(Report& report) {
  const auto* cast = report.document.scene->getLevelSet();
  const int count  = std::min(cast->getLevelCount(), report.levelLimit());
  if (cast->getLevelCount() > report.levelLimit())
    report.incomplete("level_limit");
  for (int index = 0; index < count; ++index) {
    TXshLevel* level = cast->getLevel(index);
    if (!level)
      throw Error("unsupported_level",
                  "The native cast contains an empty level.");
    const QString id = report.document.idForLevel(level);
    if (!report.readOnly() && level->getLibraryProvenance().relationship ==
                                  TXshLevel::LibraryProvenance::Linked)
      throw Error("unsupported_level",
                  "Linked Library origins are outside the detached profile.",
                  {{"level_id", id}, {"document_id", report.document.id}});
    report.levels.push_back(level);
    report.levelIds.insert(level, id);
    if (levelFormat(level) == "unsupported") unsupportedLevel(report, level);
  }
}

void collectFx(Report& report, TFx* fx) {
  if (!fx) return;
  if (report.seenEffects.contains(fx)) return;
  if (report.effects.size() == MaxEntities) {
    report.incomplete("fx_limit");
    return;
  }
  report.seenEffects.insert(fx);
  report.effects.insert(report.document.idForFx(fx), fx);
}

void collectGraph(Report& report) {
  TXsheet* sheet = report.document.scene->getTopXsheet();
  FxDag* graph   = sheet->getFxDag();
  auto* internal = graph->getInternalFxs();
  if (internal->getFxCount() > MaxEntities) report.incomplete("fx_limit");
  for (int index = 0; index < std::min(internal->getFxCount(), MaxEntities);
       ++index)
    collectFx(report, internal->getFx(index));
  collectFx(report, graph->getXsheetFx());
  if (graph->getOutputFxCount() > MaxEntities) report.incomplete("fx_limit");
  for (int index = 0; index < std::min(graph->getOutputFxCount(), MaxEntities);
       ++index)
    collectFx(report, graph->getOutputFx(index));
  report.columnCount = std::min(sheet->getColumnCount(), MaxEntities);
  if (sheet->getColumnCount() > MaxEntities) report.incomplete("column_limit");
  for (int index = 0; index < report.columnCount; ++index) {
    TXshColumn* column = sheet->getColumn(index);
    if (column) collectFx(report, column->getFx());
  }
}

QSet<TFx*> collectMacroCandidates(Report& report, int& members) {
  QSet<TFx*> candidates;
  for (TFx* fx : report.effects) {
    auto* macro = dynamic_cast<TMacroFx*>(fx);
    if (!macro) continue;
    const auto count = macro->getFxs().size();
    if (count < 2 || count > 64) return {};
    if (count > size_t(MaxEntities - report.effects.size() - members)) {
      report.incomplete("fx_limit");
      return {};
    }
    members += static_cast<int>(count);
    candidates.insert(fx);
  }
  return candidates;
}

void qualifyMacros(Report& report) {
  if (report.reasons.contains("fx_limit") ||
      report.reasons.contains("column_limit"))
    return;
  int members     = 0;
  auto candidates = collectMacroCandidates(report, members);
  if (candidates.isEmpty()) return;
  if (report.readOnly()) {
    QSet<TFx*> admitted;
    for (TFx* macro : candidates)
      if (fxAdmitted(macro)) admitted.insert(macro);
    report.qualifiedMacros = std::move(admitted);
    report.macroMembers    = members;
    return;
  }
  try {
    validateAutomationFxGraph(*report.document.scene);
  } catch (const Error&) {
    report.incomplete("unsupported_fx");
    return;
  }
  report.qualifiedMacros = std::move(candidates);
  report.macroMembers    = members;
}

TFx* parameterFx(TFx* fx) {
  auto* column = dynamic_cast<TZeraryColumnFx*>(fx);
  return column ? column->getZeraryFx() : fx;
}

bool supportedFx(TFx* fx) {
  if (dynamic_cast<TXsheetFx*>(fx) || dynamic_cast<TOutputFx*>(fx) ||
      dynamic_cast<TLevelColumnFx*>(fx))
    return true;
  TFx* parameters = parameterFx(fx);
  return parameters && isAutomationFxTypeSupported(
                           QString::fromStdString(parameters->getFxType()));
}

void inspectFx(Report& report) {
  for (auto it = report.effects.constBegin(); it != report.effects.constEnd();
       ++it) {
    if (report.qualifiedMacros.contains(it.value()) || supportedFx(it.value()))
      continue;
    report.incomplete("unsupported_fx");
    if (!report.validates()) continue;
    // Read-only documents report external and file effects with their own
    // codes; only other builtins are unsupportedFx here.
    if (report.readOnly()) {
      const QString kind =
          fxAdmission(report.document, it.value()).value("class").toString();
      if (kind != "builtin_other" && kind != "unknown") continue;
    }
    auto row = issue("unsupportedFx",
                     "Effect is outside the detached inspection profile.");
    row.insert("fx_id", it.key());
    if (!report.append(row)) break;
  }
}

QString relationship(TXshLevel::LibraryProvenance::Relationship value) {
  using Origin = TXshLevel::LibraryProvenance;
  switch (value) {
  case Origin::NotRecorded:
    return "not_recorded";
  case Origin::Linked:
    return "linked";
  case Origin::LocalCopy:
    return "local_copy";
  case Origin::Snapshot:
    return "snapshot";
  case Origin::Replaced:
    return "replaced";
  }
  throw Error("unsupported_level",
              "Native Library relationship is unrecognized.");
}

QJsonValue libraryMetadata(TXshLevel* level) {
  const auto& origin = level->getLibraryProvenance();
  if (origin.relationship == TXshLevel::LibraryProvenance::NotRecorded)
    return QJsonValue();
  if (origin.revision < 0)
    throw Error("unsupported_level", "Native Library revision is invalid.");
  // Linked origins were refused before inspection; these stored states do not
  // require I/O.
  const QString state = relationship(origin.relationship);
  return QJsonObject{{"relationship", state},
                     {"recorded_relationship", state},
                     {"item_id", boundedText(origin.itemId)},
                     {"item_path", boundedText(origin.itemPath)},
                     {"kind", boundedText(origin.kind, 128)},
                     {"source_asset_path", boundedText(origin.sourceAssetPath)},
                     {"revision", origin.revision}};
}

bool validDrawing(const TFrameId& frame) {
  if (frame.getNumber() < 1 || frame.getNumber() > 999999 ||
      frame.getLetter().size() > 8)
    return false;
  for (const QChar letter : frame.getLetter())
    if (letter < QLatin1Char('a') || letter > QLatin1Char('z')) return false;
  return true;
}

QJsonValue drawingId(const TFrameId& frame) {
  if (!validDrawing(frame)) return QJsonValue();
  return QJsonObject{{"number", frame.getNumber()},
                     {"suffix", frame.getLetter()}};
}

QString castKind(Report& report, TXshLevel* level) {
  if (report.readOnly()) {
    const QString kind = report.document.readOnly->levels.value(level).kind;
    return kind.isEmpty() ? "unknown" : kind;
  }
  if (level->getSoundLevel()) return "sound";
  if (level->getChildLevel()) return "sub_xsheet";
  if (level->getZeraryFxLevel()) return "generator";
  switch (level->getType()) {
  case PLI_XSHLEVEL:
    return "vector";
  case OVL_XSHLEVEL:
    return "raster";
  case TZP_XSHLEVEL:
    return "toonz_raster";
  default:
    return "unknown";
  }
}

QString loadStatus(Report& report, TXshLevel* level) {
  if (!report.readOnly())
    return level->getSimpleLevel() ? "loaded" : "not_applicable";
  const QString status =
      report.document.readOnly->levels.value(level).loadStatus;
  return status.isEmpty() ? "not_applicable" : status;
}

QJsonObject castRow(Report& report, TXshLevel* level) {
  // A sound source has audio frames, not drawings.
  const int count        = level->getSoundLevel() ? 0 : level->getFrameCount();
  const QStringList tags = level->getCastTags();
  const int maximum      = report.readOnly() ? DrawingLimit : MaxRecords;
  if (count < 0 || count > maximum || tags.size() > 32)
    throw Error("resource_limit",
                "Native cast metadata exceeds the inspection limits.");
  for (const QString& tag : tags) boundedText(tag, 80);
  QJsonValue cover;
  QString coverStatus = "none";
  if (level->hasCastCover()) {
    cover       = drawingId(level->getCastCover());
    coverStatus = cover.isNull() ? "unsupported" : "ok";
    if (cover.isNull() && !report.readOnly())
      throw Error("unsupported_level",
                  "Native cast cover is not a supported drawing ID.");
  }
  TXshSimpleLevel* simple = level->getSimpleLevel();
  return {{"level_id", report.levelIds.value(level)},
          {"native_cast_id", boundedText(level->getCastId(), 128)},
          {"name", boundedText(QString::fromStdWString(level->getName()), 128)},
          {"format", levelFormat(level)},
          {"drawing_count", count},
          {"tags", QJsonArray::fromStringList(tags)},
          {"cover", cover},
          {"coded_path", boundedText(level->getPath().getQString())},
          {"dirty", simple ? QJsonValue(simple->getProperties()->getDirtyFlag())
                           : QJsonValue()},
          {"source_expected_on_disk", level->sourceExpectedOnDisk()},
          {"library", libraryMetadata(level)},
          {"kind", castKind(report, level)},
          {"load_status", loadStatus(report, level)},
          {"cover_status", coverStatus}};
}

void castInventory(Report& report) {
  for (TXshLevel* level : report.levels)
    if (!report.append(castRow(report, level))) break;
}

bool adjacent(const QJsonObject& previous, const QJsonObject& next) {
  return previous.value("column_index") == next.value("column_index") &&
         previous.value("last_frame").isDouble() &&
         previous.value("last_frame").toInt() + 1 ==
             next.value("first_frame").toInt();
}

bool sameRun(const QJsonObject& previous, const QJsonObject& next) {
  return adjacent(previous, next) &&
         previous.value("level_id") == next.value("level_id") &&
         previous.value("code") == next.value("code") &&
         previous.value("drawing_id") == next.value("drawing_id");
}

bool appendRun(Report& report, const QJsonObject& row) {
  if (!report.rows.isEmpty()) {
    auto previous = report.rows.last().toObject();
    if (sameRun(previous, row)) {
      previous.insert("last_frame", row.value("last_frame"));
      report.rows.replace(report.rows.size() - 1, previous);
      return true;
    }
  }
  return report.append(row);
}

QJsonObject cellLocation(Report& report, int column, int frame) {
  TXshColumn* native = report.document.scene->getTopXsheet()->getColumn(column);
  return {{"column_id", native ? QJsonValue(report.document.idForColumn(native))
                               : QJsonValue()},
          {"column_index", column},
          {"first_frame", frame},
          {"last_frame", frame}};
}

void putLocation(QJsonObject& row, const QJsonObject& location) {
  for (auto it = location.constBegin(); it != location.constEnd(); ++it)
    row.insert(it.key(), it.value());
}

QString cellIssue(Report& report, const TXshCell& cell) {
  TXshLevel* level = cell.m_level.getPointer();
  if (!report.levelIds.contains(level)) {
    if (report.reasons.contains("level_limit")) return QString();
    report.incomplete("unsupported_level");
    return "invalidExposure";
  }
  report.used.insert(level);
  if (levelFormat(level) == "unsupported") return QString();
  if (!validDrawing(cell.m_frameId)) return "invalidExposure";
  if (!level->getSimpleLevel()->isFid(cell.m_frameId)) return "missingDrawing";
  return QString();
}

bool inspectCell(Report& report, int column, int frame, const TXshCell& cell) {
  if (cell.isEmpty()) return true;
  TXshLevel* level      = cell.m_level.getPointer();
  const QString problem = cellIssue(report, cell);
  const auto location   = cellLocation(report, column, frame);
  if (!report.validates()) {
    if (!report.levelIds.contains(level)) return true;
    auto row = location;
    row.insert("level_id", report.levelIds.value(level));
    return appendRun(report, row);
  }
  if (problem.isEmpty()) return true;
  auto row =
      issue(problem,
            "Exposure references an unavailable native drawing or cast level.");
  putLocation(row, location);
  if (report.levelIds.contains(level))
    row.insert("level_id", report.levelIds.value(level));
  row.insert("drawing_id", drawingId(cell.m_frameId));
  return appendRun(report, row);
}

bool columnIssue(Report& report, int index, const QString& code,
                 const QString& message) {
  if (!report.validates()) return true;
  auto row = issue(code, message);
  row.insert("column_index", index);
  auto* column = report.document.scene->getTopXsheet()->getColumn(index);
  if (column) row.insert("column_id", report.document.idForColumn(column));
  return report.append(row);
}

bool scanColumn(Report& report, int index, TXshColumn* column) {
  int first = 0, last = -1;
  column->getRange(first, last);
  if (first < 0 || last > 1000000)
    throw Error("unsupported_column",
                "Native exposure coordinates exceed the detached profile.");
  TXsheet* sheet = report.document.scene->getTopXsheet();
  for (int frame = first; frame <= last; ++frame) {
    if (report.cells == MaxCells) {
      report.incomplete("cell_limit");
      return false;
    }
    ++report.cells;
    if (!inspectCell(report, index, frame, sheet->getCell(frame, index)))
      return false;
  }
  return true;
}

bool inspectColumn(Report& report, int index) {
  TXshColumn* column = report.document.scene->getTopXsheet()->getColumn(index);
  if (!column || column->isEmpty())
    return columnIssue(report, index, "emptyColumn",
                       "Root column has no exposure cells.");
  if (column->getZeraryFxColumn()) return true;
  if (!column->getLevelColumn()) {
    report.incomplete("unsupported_level");
    return columnIssue(report, index, "invalidExposure",
                       "Column is outside the detached profile.");
  }
  return scanColumn(report, index, column);
}

// A read-only scene whose output frame rate or camera resolution differs from
// its project's scene defaults.
void projectMismatches(Report& report) {
  const auto project = report.document.readOnly
                           ? report.document.readOnly->project
                           : std::shared_ptr<TProject>();
  if (!project || project->getFolderCount() == 0) return;
  const TSceneProperties& defaults = project->getSceneProperties();
  ToonzScene& scene                = *report.document.scene;
  const double fps =
      scene.getProperties()->getOutputProperties()->getFrameRate();
  const double projectFps = defaults.getOutputProperties()->getFrameRate();
  if (std::abs(fps - projectFps) > 1e-6) {
    auto row =
        issue("frameRateMismatch",
              QString("Scene frame rate %1 differs from the project's %2.")
                  .arg(fps)
                  .arg(projectFps));
    row.insert("severity", "info");
    report.append(row);
  }
  if (defaults.getCameras().empty()) return;
  const TDimension res     = scene.getCurrentCamera()->getRes();
  const TDimension planned = defaults.getCameras().front()->getRes();
  if (res == planned) return;
  auto row = issue("resolutionMismatch",
                   QString("Camera %1x%2 differs from the project's %3x%4.")
                       .arg(res.lx)
                       .arg(res.ly)
                       .arg(planned.lx)
                       .arg(planned.ly));
  row.insert("severity", "info");
  report.append(row);
}

void usage(Report& report) {
  for (int index = 0; index < report.columnCount; ++index)
    if (!inspectColumn(report, index)) return;
  if (report.validates()) {
    for (const QJsonValue& row : readOnlyIssues(report.document))
      if (!report.append(row.toObject())) return;
    projectMismatches(report);
  }
  if (!report.validates() || !report.reasons.isEmpty()) return;
  for (TXshLevel* level : report.levels) {
    if (report.used.contains(level)) continue;
    auto row =
        issue("unusedLevel",
              "Cast level is unused in the completely inspected root sheet.");
    row.insert("level_id", report.levelIds.value(level));
    if (!report.append(row)) return;
  }
}

QJsonObject dependencyRow(const QString& owner, const QString& id,
                          const QString& role, const TFilePath& path) {
  return {{"owner_kind", owner},
          {"owner_id", id},
          {"role", role},
          {"coded_path", boundedText(path.getQString())},
          {"resolved_path", QJsonValue()},
          {"path_kind", path.isEmpty() ? "empty" : "file"},
          {"authority", "unresolved"},
          {"availability", path.isEmpty() ? "unsaved" : "unresolved"},
          {"size_bytes", QJsonValue()},
          {"pinned", false}};
}

bool knownAlias(ToonzScene& scene, const TFilePath& path) {
  std::wstring head;
  TFilePath tail;
  path.split(head, tail);
  if (head.empty()) return true;
  if (head.front() == L'+')
    return !scene.getProject()
                ->getFolder(
                    QString::fromStdWString(head.substr(1)).toStdString())
                .isEmpty();
  if (head.front() == L'$') return head == L"$scenefolder";
  return true;
}

void validateAliasLayout(ToonzScene& scene, const TFilePath& path) {
  std::wstring head;
  TFilePath tail;
  path.split(head, tail);
  if (head.empty() || head.front() != L'+') return;
  const auto project = scene.getProject();
  TFilePath root     = project->getFolder(TProject::Scenes);
  if (!root.isEmpty() && !root.isAbsolute())
    root = project->getProjectFolder() + root;
  if (TSystem::isUNC(scene.getScenePath()) && !TSystem::isUNC(root))
    throw Error("path_denied",
                "Dependency alias requires implicit network-share resolution.");
}

QString resolveDependency(Report& report, const TFilePath& path) {
  if (!knownAlias(*report.document.scene, path)) return QString();
  validateAliasLayout(*report.document.scene, path);
  const auto actual = report.document.scene->decodeFilePath(path);
  if (!actual.isAbsolute()) return QString();
  if (actual.getQString().contains(QChar::Null))
    throw Error("invalid_path", "Dependency paths cannot contain NUL.");
  if (actual.isLevelName()) {
    const QString parent =
        report.context.resolvePath(actual.getParentDir().getQString(), false);
    return QDir(parent).filePath(
        QString::fromStdWString(actual.getLevelNameW()));
  }
  return report.context.resolvePath(actual.getQString(), false);
}

// Whether a lookup failed because access was denied rather than because the
// entry is absent. The UTF-16 path converts to the native encoding on every
// platform.
bool lookupDenied(const QString& path) {
  std::error_code failure;
  const std::filesystem::file_status status = std::filesystem::symlink_status(
      std::filesystem::path(path.toStdU16String()), failure);
  return status.type() == std::filesystem::file_type::none &&
         (failure == std::errc::permission_denied ||
          failure == std::errc::operation_not_permitted);
}

void fileObservation(QJsonObject& row, const QString& path) {
  const QFileInfo file(path);
  if (file.isSymLink()) {
    row.insert("authority", "denied");
    row.insert("availability", "forbidden");
    row.insert("resolved_path", QJsonValue());
    return;
  }
  if (!file.exists()) {
    row.insert("availability", lookupDenied(path) ? "unreadable" : "missing");
    return;
  }
  if (!file.isFile()) {
    row.insert("availability", "unsupported");
    row.insert("path_kind", "unsupported");
    return;
  }
  QFile probe;
  OpenResult opened = OpenResult::Failed;
  try {
    opened = openApprovedFile(probe, path);
  } catch (const Error&) {
    row.insert("authority", "denied");
    row.insert("availability", "forbidden");
    row.insert("resolved_path", QJsonValue());
    return;
  }
  if (opened != OpenResult::Opened) {
    row.insert("availability", "unreadable");
    return;
  }
  const qint64 bytes = file.size();
  if (bytes < 0 || bytes > 9007199254740991LL)
    throw Error("resource_limit",
                "Dependency size cannot be represented exactly.");
  row.insert("availability", "present");
  row.insert("size_bytes", static_cast<double>(bytes));
}

QString dependencyAuthority(const Context& context, const QString& path) {
  const QString root   = context.workspace();
  const QString prefix = root.endsWith('/') ? root : root + '/';
  return path == root || path.startsWith(prefix) ? "workspace" : "input_root";
}

QJsonObject observeDependency(Report& report, QJsonObject row,
                              const TFilePath& path) {
  if (path.isEmpty()) return row;
  QString resolved;
  try {
    resolved = resolveDependency(report, path);
  } catch (const Error& error) {
    if (error.code != "path_denied" && error.code != "invalid_path") throw;
    row.insert("authority", "denied");
    row.insert("availability", "forbidden");
    return row;
  }
  if (resolved.isEmpty()) return row;
  if (report.readOnly())
    row.insert("pinned",
               report.document.readOnly->pinnedPaths.contains(resolved));
  row.insert("resolved_path", boundedText(resolved));
  row.insert("authority", dependencyAuthority(report.context, resolved));
  if (path.isLevelName()) {
    row.insert("path_kind", "sequence_pattern");
    row.insert("availability", "not_checked");
  } else {
    fileObservation(row, resolved);
  }
  return row;
}

bool levelDependency(Report& report, TXshLevel* level, const QString& role,
                     const TFilePath& path) {
  auto row = dependencyRow("level", report.levelIds.value(level), role, path);
  if (levelFormat(level) == "unsupported") {
    row.insert("path_kind", "unsupported");
    row.insert("availability", "unsupported");
  } else {
    row = observeDependency(report, row, path);
  }
  return report.append(row);
}

bool extraLevelDependencies(Report& report, TXshLevel* level,
                            TXshSimpleLevel* simple) {
  const auto scan = simple->getScannedPath();
  if (!scan.isEmpty() && !levelDependency(report, level, "scan", scan))
    return false;
  const TPalette* palette = simple->getPalette();
  if (!palette || palette->getRefImgPath().isEmpty()) return true;
  return levelDependency(report, level, "reference_image",
                         palette->getRefImgPath());
}

bool observedDependency(Report& report, const QString& owner, const QString& id,
                        const QString& role, const TFilePath& path) {
  return report.append(
      observeDependency(report, dependencyRow(owner, id, role, path), path));
}

// Texture styles name image files: shipped library textures have no folder.
bool styleDependencies(Report& report, TXshLevel* level,
                       TXshSimpleLevel* simple) {
  const TPalette* palette = simple->getPalette();
  for (int id = 0; palette && id < palette->getStyleCount(); ++id) {
    TColorStyle* style = palette->getStyle(id);
    for (int param = 0; style && param < style->getParamCount(); ++param) {
      if (style->getParamType(param) != TColorStyle::FILEPATH) continue;
      const TFilePath path =
          style->getParamValue(TColorStyle::TFilePath_tag(), param);
      if (path.isEmpty()) continue;
      auto row =
          dependencyRow("level", report.levelIds.value(level), "texture", path);
      if (path.getParentDir().isEmpty()) {
        row.insert("path_kind", "unsupported");
        row.insert("availability", "unsupported");
      } else {
        row = observeDependency(report, row, path);
      }
      if (!report.append(row)) return false;
    }
  }
  return true;
}

bool hookDependency(Report& report, TXshLevel* level) {
  if (level->getPath().isEmpty()) return true;
  const TFilePath hook = TXshSimpleLevel::getExistingHookFile(
      report.document.scene->decodeFilePath(level->getPath()));
  return hook.isEmpty() ||
         observedDependency(report, "level", report.levelIds.value(level),
                            "hook_sidecar", hook);
}

bool levelDependencies(Report& report, TXshLevel* level) {
  if (level->getSoundLevel())
    return observedDependency(report, "level", report.levelIds.value(level),
                              "sound", level->getPath());
  if (!levelDependency(report, level, "primary", level->getPath()))
    return false;
  TXshSimpleLevel* simple = level->getSimpleLevel();
  if (!simple) return true;
  if (!hookDependency(report, level) ||
      !styleDependencies(report, level, simple))
    return false;
  if (level->getType() == TZP_XSHLEVEL && !level->getPath().isEmpty())
    if (!levelDependency(report, level, "palette",
                         level->getPath().withType("tpl")))
      return false;
  return extraLevelDependencies(report, level, simple);
}

// The project's raster drawing palette, read when the scene has raster
// levels and the project defines a palettes folder.
bool rasterPaletteDependency(Report& report) {
  const auto project = report.document.scene->getProject();
  if (!report.readOnly() || !project || project->getFolderIndex("palettes") < 0)
    return true;
  bool raster = false;
  for (TXshLevel* level : report.levels)
    raster = raster || level->getType() == OVL_XSHLEVEL;
  return !raster ||
         observedDependency(report, "scene", report.document.id,
                            "raster_drawing_palette",
                            TFilePath("+palettes/Raster_Drawing_Palette.tpl"));
}

bool fxDependency(Report& report, const QString& id, SceneFxResource* fx) {
  if (report.readOnly())
    return observedDependency(report, "fx", id, "fx_file_resource",
                              fx->resourcePath());
  auto row = dependencyRow("fx", id, "fx_resource", fx->resourcePath());
  row.insert("path_kind", "unsupported");
  row.insert("availability", "unsupported");
  return report.append(row);
}

// The rows owned by the scene rather than one level.
void sceneDependencies(Report& report) {
  if (!rasterPaletteDependency(report)) return;
  for (auto it = report.effects.constBegin(); it != report.effects.constEnd();
       ++it) {
    auto* resource = dynamic_cast<SceneFxResource*>(parameterFx(it.value()));
    if (resource && !fxDependency(report, it.key(), resource)) return;
  }
}

void dependencies(Report& report) {
  for (TXshLevel* level : report.levels)
    if (!levelDependencies(report, level)) return;
  sceneDependencies(report);
}

QJsonObject page(Report& report, const QJsonObject& params) {
  const int offset = integerParam(params, "offset", 0, MaxRecords, 0);
  const int limit  = integerParam(params, "limit", 1, 128, 32);
  if (offset > report.rows.size())
    throw Error("invalid_params", "Offset exceeds discovered records.");
  const int end = std::min(offset + limit, report.rows.size());
  QJsonArray items;
  for (int index = offset; index < end; ++index)
    items.append(report.rows.at(index));
  const QJsonObject scanned{
      {"levels", static_cast<int>(report.levels.size())},
      {"columns", report.columnCount},
      {"fx_nodes", report.effects.size() + report.macroMembers},
      {"scanned_cells", report.cells}};
  QJsonObject result{{"document_id", report.document.id},
                     {"source_revision", report.document.revisionText()},
                     {"profile", report.readOnly() ? readOnlyProfileName()
                                                   : "worker-generated-v1"},
                     {"scope", "root_sheet"},
                     {"offset", offset},
                     {"limit", limit},
                     {"items", items},
                     {"discovered_records", report.rows.size()},
                     {"next_offset", end < report.rows.size() ? QJsonValue(end)
                                                              : QJsonValue()},
                     {"traversal_complete", report.reasons.isEmpty()},
                     {"reasons", report.reasons},
                     {"scanned", scanned},
                     {"limits", limits(report.levelLimit())},
                     {"excluded_checks", exclusions()}};
  // A native request ID has at most 128 UTF-16 code units; escaped controls are
  // the largest encoding.
  const QJsonObject envelope{{"id", QString(128, QChar(1))},
                             {"result", result}};
  if (QJsonDocument(envelope).toJson(QJsonDocument::Compact).size() + 1 >
      MaxResponse)
    throw Error("resource_limit",
                "Inspection response exceeds one MiB; reduce limit.");
  return result;
}

QJsonObject inputProperties() {
  const QJsonObject handle{
      {"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
  return {{"document_id", handle},
          {"expected_revision", handle},
          {"offset", QJsonObject{{"type", "integer"},
                                 {"minimum", 0},
                                 {"maximum", MaxRecords},
                                 {"default", 0}}},
          {"limit", QJsonObject{{"type", "integer"},
                                {"minimum", 1},
                                {"maximum", 128},
                                {"default", 32}}}};
}

}  // namespace

struct DependencyWalk::Imp final {
  Report report;
  std::size_t next = 0;
  bool scene       = false;

  Imp(Context& context, Document& document)
      : report(context, document, "scene.dependencies") {}
};

DependencyWalk::DependencyWalk(Context& context, Document& document,
                               int rowLimit)
    : m_imp(std::make_unique<Imp>(context, document)) {
  Report& report     = m_imp->report;
  report.recordLimit = rowLimit;
  collectLevels(report);
  collectGraph(report);
  qualifyMacros(report);
  inspectFx(report);
}

DependencyWalk::~DependencyWalk() = default;

bool DependencyWalk::step() {
  Report& report = m_imp->report;
  if (report.reasons.contains("record_limit")) return false;
  if (m_imp->next < report.levels.size()) {
    levelDependencies(report, report.levels[m_imp->next++]);
    return true;
  }
  if (!m_imp->scene) {
    m_imp->scene = true;
    sceneDependencies(report);
  }
  return false;
}

int DependencyWalk::levels() const { return int(m_imp->report.levels.size()); }

int DependencyWalk::levelsDone() const { return int(m_imp->next); }

const QJsonArray& DependencyWalk::rows() const { return m_imp->report.rows; }

QStringList DependencyWalk::reasons() const {
  QStringList reasons;
  for (const QJsonValue& reason : m_imp->report.reasons)
    reasons.append(reason.toString());
  return reasons;
}

QJsonArray productionOperations() {
  QJsonArray result;
  const QMap<QString, QString> descriptions{
      {"cast.list",
       "Read native cast metadata in the current detached profile."},
      {"cast.usage",
       "Read root-sheet cast exposure runs with bounded traversal."},
      {"scene.dependencies",
       "Read declared dependency references and authorized file metadata. "
       "availability is present, missing, unreadable (it exists but cannot "
       "be opened, or its folder denies access), forbidden, unsupported, "
       "not_checked, unresolved or unsaved; unreadable files report no "
       "size."},
      {"scene.validate",
       "Read basic in-memory scene issues without loading artwork or media."}};
  for (auto it = descriptions.constBegin(); it != descriptions.constEnd(); ++it)
    result.append(operation(it.key(), it.value(), inputProperties(),
                            {"document_id", "expected_revision"}));
  return result;
}

QJsonValue executeProduction(Context& context, const QString& method,
                             const QJsonObject& params) {
  Document& document = context.document(requiredString(params, "document_id"));
  if (requiredString(params, "expected_revision") != document.revisionText())
    throw Error(
        "stale_revision", "Refresh scene.summary before continuing inspection.",
        {{"document_id", document.id}, {"revision", document.revisionText()}});
  Report report(context, document, method);
  collectLevels(report);
  collectGraph(report);
  qualifyMacros(report);
  inspectFx(report);
  if (method == "cast.list")
    castInventory(report);
  else if (method == "cast.usage" || method == "scene.validate")
    usage(report);
  else if (method == "scene.dependencies")
    dependencies(report);
  else
    throw Error("unknown_method", "Unknown production inspection method.");
  return page(report, params);
}

}  // namespace GroundzeroAutomation
