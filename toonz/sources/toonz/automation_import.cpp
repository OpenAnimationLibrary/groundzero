#include "automation_import.h"

#include "automation_audio.h"
#include "automation_drawing.h"
#include "automation_fx_profile.h"
#include "automation_hook.h"
#include "automation_path.h"
#include "automation_rig.h"
#include "automation_scene_host.h"
#include "automation_storyboard.h"

#include "tcolorstyles.h"
#include "tfx.h"
#include "tpalette.h"
#include "tsimplecolorstyles.h"
#include "toonz/fxdag.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QCryptographicHash>
#include <QJsonDocument>

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <tuple>
#include <typeinfo>

namespace GroundzeroAutomation {
namespace {

constexpr int DefaultSamples     = 8;
constexpr int MaximumSamples     = 16;
constexpr int ProfileLevels      = 256;
constexpr int ProfileDrawings    = 4096;
constexpr int ProfileColumns     = 4096;
constexpr qint64 ProfileCells    = 1000000;
constexpr qint64 ProfileBytes    = 256 * 1024 * 1024;
constexpr qint64 BytesPerPixel   = 4;
constexpr qint64 BytesPerDrawing = 4096;

// The loss kind and effect of each host content row reason that drops what
// it names. Style rows are covered by the palette scan, rows inside a
// sub-xsheet by the sub_xsheet loss; any other reason blocks as unclassified.
struct RowRule final {
  QString kind;
  QString effect;
  QString reason;
};

const std::map<QString, RowRule>& rowRules() {
  static const std::map<QString, RowRule> rules{
      {"unrecognized_scene_tag", {"unrecognized_scene_tag", "dropped", {}}},
      {"unsupported_column", {"column_not_admitted", "emptied", {}}},
      {"linked_library_origin", {"library_origin", "metadata_dropped", {}}},
      {"external_code", {"fx_not_admitted", "dropped", {}}},
      {"file_resource", {"fx_not_admitted", "dropped", {}}},
      {"unsupported_fx", {"fx_not_admitted", "dropped", {}}},
      {"unsupported_level_format", {"level_not_admitted", "emptied", "format"}},
      {"unknown_level", {"level_not_admitted", "emptied", "format"}},
      {"unsupported_samples", {"level_not_admitted", "emptied", "format"}},
      {"image_too_large", {"level_not_admitted", "emptied", "raster_size"}},
      {"missing_file", {"level_load_failed", "emptied", {}}},
      {"forbidden_file", {"level_load_failed", "emptied", {}}},
      {"external_file_reference", {"level_load_failed", "emptied", {}}},
      {"level_load_failed", {"level_load_failed", "emptied", {}}},
      {"partial_level", {"level_load_failed", "emptied", {}}},
      {"creator_forbidden", {"level_load_failed", "emptied", {}}}};
  return rules;
}

bool styleRow(const ContentRow& row) {
  return row.kind == "style" && (row.reason == "library_resource_unavailable" ||
                                 row.reason == "external_file_reference");
}

// A level-wide loss names its reason first, as level_not_admitted requires.
QString reasoned(const QString& reason, const QString& detail) {
  return reason.isEmpty() ? detail : reason + ": " + detail;
}

class Classifier final {
public:
  explicit Classifier(Document& document)
      : m_document(document)
      , m_source(*document.readOnly)
      , m_sheet(document.scene->getXsheet()) {}

  ImportPlan run() {
    rows();
    levels();
    effects();
    columns();
    exposures();
    validators();
    estimate();
    return std::move(m_plan);
  }

private:
  void add(const QString& kind, const QString& effect, ImportItem item) {
    ImportLoss& loss = m_plan.losses[kind];
    loss.effect      = effect;
    loss.items.push_back(std::move(item));
  }

  ImportItem levelItem(TXshLevel* level, const QString& detail) {
    ImportItem item;
    item.level   = level;
    item.levelId = m_document.idForLevel(level);
    item.name    = QString::fromStdWString(level->getName());
    item.detail  = detail;
    return item;
  }

  ImportItem rowItem(const ContentRow& row) {
    ImportItem item;
    item.level     = row.level;
    item.column    = row.column;
    item.fx        = row.fx;
    item.levelId   = row.levelId;
    item.columnId  = row.columnId;
    item.fxId      = row.fxId;
    item.sheetPath = row.sheetPath;
    item.name      = row.name;
    item.detail    = row.detail;
    return item;
  }

  void dropLevel(const RowRule& rule, const ContentRow& row) {
    if (!m_dropped.insert(row.level).second) return;
    ImportItem item = rowItem(row);
    item.detail     = reasoned(rule.reason, row.detail);
    add(rule.kind, rule.effect, item);
  }

  void row(const ContentRow& row) {
    if (!row.sheetPath.isEmpty() || styleRow(row)) return;
    const auto found = rowRules().find(row.reason);
    if (found == rowRules().end()) {
      ImportItem item = rowItem(row);
      item.detail = "Unclassified content (" + row.reason + "): " + row.detail;
      m_plan.unclassified.push_back(item);
      return;
    }
    const RowRule& rule = found->second;
    if (rule.kind == "level_not_admitted" || rule.kind == "level_load_failed")
      return dropLevel(rule, row);
    if (rule.kind == "fx_not_admitted" && !m_droppedFx.insert(row.fx).second)
      return;
    if (rule.kind == "column_not_admitted") m_droppedColumns.insert(row.column);
    add(rule.kind, rule.effect, rowItem(row));
  }

  void rows() {
    for (const ContentRow& entry : m_source.rows) row(entry);
  }

  // Levels that are not simple drawing levels: a sub-xsheet is emptied, a
  // sound is kept for the audio profile, every other kind is not admitted.
  bool structuralLevel(TXshLevel* level, const LevelState& state) {
    if (state.kind == "sub_xsheet") {
      m_dropped.insert(level);
      add("sub_xsheet", "emptied",
          levelItem(level,
                    "A sub-xsheet: its cells are cleared and its "
                    "sheet is not imported."));
      return true;
    }
    if (state.kind == "sound") return true;
    if (level->getSimpleLevel()) return false;
    m_dropped.insert(level);
    add("level_not_admitted", "emptied",
        levelItem(level,
                  reasoned("format", "A " + state.kind +
                                         " level is outside the profile.")));
    return true;
  }

  // The palette as the import would keep it, or null without one.
  std::unique_ptr<TPalette> keptPalette(TXshSimpleLevel* level,
                                        std::vector<ImportItem>& styles) {
    if (level->getType() == OVL_XSHLEVEL || !level->getPalette())
      return nullptr;
    std::unique_ptr<TPalette> palette(level->getPalette()->clone());
    for (int id : admitImportStyles(palette.get())) {
      ImportItem item =
          levelItem(level, "Style " + QString::number(id) +
                               " becomes a solid style of its main colour; its "
                               "animation and links are dropped.");
      item.style = id;
      styles.push_back(item);
    }
    palette->setGlobalName(L"");
    return palette;
  }

  void keptLevel(TXshSimpleLevel* level, const LevelState& state) {
    std::vector<ImportItem> styles;
    const std::unique_ptr<TPalette> palette = keptPalette(level, styles);
    QString detail;
    const QString limit = drawingProfileLimit(level, palette.get(), detail);
    if (!limit.isEmpty()) {
      m_dropped.insert(level);
      add("level_not_admitted", "emptied",
          levelItem(level, reasoned(limit, detail)));
      return;
    }
    for (const ImportItem& style : styles)
      add("style_not_admitted", "replaced", style);
    if (level->getPalette() && !level->getPalette()->getGlobalName().empty())
      add("library_origin", "metadata_dropped",
          levelItem(level, "The palette's studio palette link is dropped."));
    if (state.bitDepth > 8)
      add("raster_depth_reduced", "converted",
          levelItem(level, QString("%1-bit channels become 8-bit RGBA.")
                               .arg(state.bitDepth)));
    m_kept.push_back(level);
    if (level->getType() == OVL_XSHLEVEL)
      ++m_plan.conversions["level_reencoded_lossless"];
    if (level->getType() != PLI_XSHLEVEL &&
        level->getProperties()->getDpiPolicy() != LevelProperties::DP_CustomDpi)
      ++m_plan.conversions["dpi_policy_fixed"];
    ++m_plan.conversions["assets_collected"];
  }

  void simpleLevel(TXshSimpleLevel* level, const LevelState& state) {
    QString kind, detail;
    if (state.loadStatus != "loaded") {
      kind   = "level_load_failed";
      detail = "The level did not load (" + state.loadStatus + ").";
    } else if (level->getType() == TZI_XSHLEVEL) {
      kind   = "level_not_admitted";
      detail = reasoned("format", "A scan level is outside the profile.");
    } else if (level->getFrameCount() == 0) {
      kind   = "empty_level";
      detail = "The level has no drawing.";
    }
    if (kind.isEmpty()) return keptLevel(level, state);
    m_dropped.insert(level);
    add(kind, kind == "empty_level" ? "dropped" : "emptied",
        levelItem(level, detail));
  }

  void levels() {
    TLevelSet* set = m_document.scene->getLevelSet();
    for (int index = 0; index < set->getLevelCount(); ++index) {
      TXshLevel* level = set->getLevel(index);
      if (m_dropped.count(level)) continue;
      const LevelState state = m_source.levels.value(level);
      if (structuralLevel(level, state)) continue;
      simpleLevel(level->getSimpleLevel(), state);
    }
  }

  // Top-sheet effects of admitted types with values outside their authoring
  // ranges are clamped when clamping admits them, and dropped otherwise.
  void effect(TFx* node) {
    TFx* fx = node;
    if (auto* wrapper = dynamic_cast<TZeraryColumnFx*>(node))
      fx = wrapper->getZeraryFx();
    QStringList violations;
    if (!fx || m_droppedFx.count(node) || !fxAdmitted(node) ||
        !automationFxProfileViolations(fx, violations) || violations.isEmpty())
      return;
    ImportItem item;
    item.fx   = node;
    item.fxId = m_document.idForFx(node);
    item.name = QString::fromStdWString(node->getName());
    const TFxP copy(fx->clone(false));
    clampAutomationFxValues(copy.getPointer());
    QStringList remaining;
    automationFxProfileViolations(copy.getPointer(), remaining);
    if (remaining.isEmpty()) {
      item.detail = "Clamped to the authoring ranges: " + violations.join(", ");
      add("fx_value_out_of_range", "clamped", item);
      return;
    }
    m_droppedFx.insert(node);
    item.detail =
        "Outside the profile even when clamped: " + remaining.join(", ");
    add("fx_not_admitted", "dropped", item);
  }

  void effects() {
    std::set<TFx*> nodes;
    m_sheet->getFxDag()->getInternalFxs()->getFxs(nodes);
    for (int index = 0; index < m_sheet->getColumnCount(); ++index)
      if (TXshColumn* column = m_sheet->getColumn(index))
        if (column->getZeraryFxColumn()) nodes.insert(column->getFx());
    std::vector<TFx*> ordered(nodes.begin(), nodes.end());
    std::sort(ordered.begin(), ordered.end(), [this](TFx* left, TFx* right) {
      return m_document.idForFx(left) < m_document.idForFx(right);
    });
    for (TFx* node : ordered) effect(node);
  }

  // Generator columns of dropped effects go with them.
  bool keptColumn(TXshColumn* column) {
    if (!column || m_droppedColumns.count(column)) return false;
    return !(column->getZeraryFxColumn() && m_droppedFx.count(column->getFx()));
  }

  void columns() {
    for (int index = 0; index < m_sheet->getColumnCount(); ++index) {
      TXshColumn* column = m_sheet->getColumn(index);
      if (!keptColumn(column) || column->isEmpty() ||
          column->getLevelColumn() || column->getZeraryFxColumn() ||
          column->getSoundColumn())
        continue;
      ImportItem item;
      item.column   = column;
      item.columnId = m_document.idForColumn(column);
      item.detail   = "A column kind outside the profile.";
      m_plan.unclassified.push_back(item);
    }
  }

  // Exposures of kept levels that name a drawing the level does not hold.
  void columnExposures(TXshColumn* column, int index) {
    int first = 0, last = -1;
    column->getRange(first, last);
    std::set<std::pair<TXshSimpleLevel*, TFrameId>> missing;
    for (int row = std::max(first, 0); row <= last; ++row) {
      const TXshCell cell    = m_sheet->getCell(row, index);
      TXshSimpleLevel* level = cell.getSimpleLevel();
      if (!level || m_dropped.count(level) || level->isFid(cell.m_frameId) ||
          !missing.insert({level, cell.m_frameId}).second)
        continue;
      ImportItem item = levelItem(
          level, "Drawing " + QString::fromStdString(cell.m_frameId.expand()) +
                     " is not in the level; its cells are cleared.");
      item.column   = column;
      item.columnId = m_document.idForColumn(column);
      item.frame    = cell.m_frameId;
      add("exposure_not_admitted", "emptied", item);
    }
  }

  void exposures() {
    for (int index = 0; index < m_sheet->getColumnCount(); ++index) {
      TXshColumn* column = m_sheet->getColumn(index);
      if (keptColumn(column) && !column->isEmpty() && column->getLevelColumn())
        columnExposures(column, index);
    }
  }

  // The scene-wide profile checks of motion paths, rigs, hooks, storyboards
  // and sound: a refusal blocks as unclassified, naming the check.
  void validators() {
    ToonzScene& scene = *m_document.scene;
    const std::vector<std::pair<QString, std::function<void()>>> checks{
        {"motion paths", [&scene] { validateAutomationPaths(scene); }},
        {"rig", [&scene] { validateAutomationRig(scene); }},
        {"hooks", [&scene] { validateAutomationHooks(scene); }},
        {"storyboard", [&scene] { validateAutomationStoryboard(scene); }},
        {"sound", [&scene] { validateAutomationAudio(scene); }}};
    for (const auto& check : checks) {
      QString message;
      try {
        check.second();
        continue;
      } catch (const Error& error) {
        message = error.message;
      } catch (...) {
        message = "The check failed natively.";
      }
      ImportItem item;
      item.name = check.first;
      item.detail =
          ("The " + check.first + " profile check refuses: " + message)
              .left(512);
      m_plan.unclassified.push_back(item);
    }
  }

  qint64 cells() {
    qint64 count = 0;
    for (int index = 0; index < m_sheet->getColumnCount(); ++index) {
      TXshColumn* column = m_sheet->getColumn(index);
      if (!keptColumn(column) || column->isEmpty()) continue;
      int first = 0, last = -1;
      column->getRange(first, last);
      count += last - first + 1;
    }
    return count;
  }

  int columnCount() {
    int count = 0;
    for (int index = 0; index < m_sheet->getColumnCount(); ++index)
      if (keptColumn(m_sheet->getColumn(index))) ++count;
    return count;
  }

  // What the kept content would be, and the first editable bound it breaks.
  void estimate() {
    qint64 drawings = 0, pixels = 0;
    for (TXshSimpleLevel* level : m_kept) {
      drawings += level->getFrameCount();
      const TDimension size = level->getProperties()->getImageRes();
      if (level->getType() != PLI_XSHLEVEL)
        pixels += qint64(size.lx) * size.ly * level->getFrameCount();
    }
    const qint64 levels  = qint64(m_kept.size());
    const qint64 columns = columnCount();
    const qint64 counted = cells();
    const qint64 bytes   = pixels * BytesPerPixel + drawings * BytesPerDrawing;
    const std::vector<std::tuple<const char*, qint64, qint64>> bounds{
        {"levels", levels, ProfileLevels},
        {"drawings", drawings, ProfileDrawings},
        {"columns", columns, ProfileColumns},
        {"cells", counted, ProfileCells},
        {"package_bytes", bytes, ProfileBytes}};
    for (const auto& bound : bounds)
      if (m_plan.blockedLimit.isEmpty() &&
          std::get<1>(bound) > std::get<2>(bound)) {
        m_plan.blockedLimit   = std::get<0>(bound);
        m_plan.blockedValue   = std::get<1>(bound);
        m_plan.blockedMaximum = std::get<2>(bound);
      }
    const auto capped = [](qint64 value, qint64 maximum) {
      return double(std::min(value, maximum));
    };
    m_plan.estimate = {{"levels", capped(levels, ProfileLevels)},
                       {"drawings", capped(drawings, ProfileDrawings)},
                       {"columns", capped(columns, ProfileColumns)},
                       {"cells", capped(counted, ProfileCells)},
                       {"decoded_pixels", double(pixels)},
                       {"package_bytes", capped(bytes, ProfileBytes)}};
  }

  Document& m_document;
  ReadOnlyScene& m_source;
  TXsheet* m_sheet;
  ImportPlan m_plan;
  std::set<TXshLevel*> m_dropped;
  std::set<TFx*> m_droppedFx;
  std::set<TXshColumn*> m_droppedColumns;
  std::vector<TXshSimpleLevel*> m_kept;
};

QJsonValue optionalText(const QString& value, int maximum) {
  return value.isEmpty() ? QJsonValue() : QJsonValue(value.left(maximum));
}

QJsonObject itemObject(const ImportItem& item) {
  return {{"level_id", optionalText(item.levelId, 128)},
          {"column_id", optionalText(item.columnId, 128)},
          {"fx_id", optionalText(item.fxId, 128)},
          {"sheet_path", item.sheetPath},
          {"name", optionalText(item.name, 256)},
          {"detail", optionalText(item.detail, 512)}};
}

// Names and details only: what two handle sets of one source share.
QJsonObject shapeObject(const ImportItem& item) {
  return {{"name", item.name}, {"detail", item.detail}};
}

using ItemWriter = std::function<QJsonObject(const ImportItem&)>;

QJsonArray itemArray(const std::vector<ImportItem>& items, int limit,
                     const ItemWriter& writer) {
  QJsonArray array;
  for (const ImportItem& item : items) {
    if (limit >= 0 && array.size() >= limit) break;
    array.append(writer(item));
  }
  return array;
}

QJsonArray conversionArray(const ImportPlan& plan) {
  QJsonArray array;
  for (const auto& entry : plan.conversions)
    array.append(QJsonObject{{"kind", entry.first}, {"count", entry.second}});
  return array;
}

// Losses with every item (limit < 0, as "items") or with samples.
QJsonArray lossArray(const ImportPlan& plan, int limit,
                     const ItemWriter& writer) {
  QJsonArray array;
  const QString field = limit < 0 ? "items" : "samples";
  for (const auto& entry : plan.losses)
    array.append(
        QJsonObject{{"kind", entry.first},
                    {"effect", entry.second.effect},
                    {"count", double(entry.second.items.size())},
                    {field, itemArray(entry.second.items, limit, writer)}});
  return array;
}

QJsonArray blockerArray(const ImportPlan& plan, int limit,
                        const ItemWriter& writer) {
  QJsonArray array;
  const QString field = limit < 0 ? "items" : "samples";
  if (!plan.blockedLimit.isEmpty())
    array.append(QJsonObject{{"kind", "profile_limit"},
                             {"limit", plan.blockedLimit},
                             {"value", double(plan.blockedValue)},
                             {"maximum", double(plan.blockedMaximum)},
                             {"count", 1},
                             {field, QJsonArray()}});
  if (!plan.unclassified.empty())
    array.append(
        QJsonObject{{"kind", "unclassified"},
                    {"limit", QJsonValue()},
                    {"value", QJsonValue()},
                    {"maximum", QJsonValue()},
                    {"count", double(plan.unclassified.size())},
                    {field, itemArray(plan.unclassified, limit, writer)}});
  return array;
}

QJsonObject importPlan(Context& context, const QJsonObject& params) {
  const QString id   = requiredString(params, "document_id");
  Document& document = context.document(id);
  if (!document.readOnly)
    throw Error("unsupported_target",
                "scene.import_plan reads a document opened with "
                "scene.open_read_only; this one is already editable.",
                {{"reason", "editable_document"}});
  const int samples =
      integerParam(params, "sample_limit", 1, MaximumSamples, DefaultSamples);
  requireUnchangedSources(document.readOnly->pins);
  const ImportPlan plan     = classifyImport(document);
  const QString fingerprint = document.readOnly->fingerprint;
  return {{"document_id", id},
          {"source_fingerprint", fingerprint},
          {"profile", "worker-generated-v1"},
          {"classifier_version", ImportClassifierVersion},
          {"plan_digest", importPlanDigest(plan, fingerprint)},
          {"importable", plan.importable()},
          {"blockers", blockerArray(plan, samples, itemObject)},
          {"losses", lossArray(plan, samples, itemObject)},
          {"conversions", conversionArray(plan)},
          {"estimate", plan.estimate},
          {"source_copy", "attach"}};
}

QByteArray digestOf(const QJsonObject& canonical) {
  return QCryptographicHash::hash(
             QJsonDocument(canonical).toJson(QJsonDocument::Compact),
             QCryptographicHash::Sha256)
      .toHex();
}

}  // namespace

std::vector<int> admitImportStyles(TPalette* palette) {
  std::vector<int> replaced;
  const auto admitted = [](const TColorStyle* style) {
    return style && typeid(*style) == typeid(TSolidColorStyle) &&
           style->getGlobalName().empty() && style->getOriginalName().empty();
  };
  for (int id = 0; id < palette->getStyleCount(); ++id) {
    TColorStyle* style = palette->getStyle(id);
    bool keep          = admitted(style);
    for (const auto& key : palette->getStyleAnimation(id))
      keep = keep && admitted(key.second.style.getPointer());
    if (keep || !style) continue;
    auto* solid = new TSolidColorStyle(style->getMainColor());
    solid->setName(style->getName());
    palette->setStyle(id, solid);
    replaced.push_back(id);
  }
  return replaced;
}

ImportPlan classifyImport(Document& document) {
  return Classifier(document).run();
}

QString importPlanDigest(const ImportPlan& plan, const QString& fingerprint) {
  const QJsonObject canonical{{"classifier_version", ImportClassifierVersion},
                              {"source_fingerprint", fingerprint},
                              {"blockers", blockerArray(plan, -1, itemObject)},
                              {"losses", lossArray(plan, -1, itemObject)},
                              {"conversions", conversionArray(plan)}};
  return "plan:" + QString::fromLatin1(digestOf(canonical));
}

QByteArray importPlanShape(const ImportPlan& plan) {
  return QJsonDocument(
             QJsonObject{{"blockers", blockerArray(plan, -1, shapeObject)},
                         {"losses", lossArray(plan, -1, shapeObject)},
                         {"conversions", conversionArray(plan)}})
      .toJson(QJsonDocument::Compact);
}

QJsonArray importOperations() {
  return {operation(
      "scene.import_plan",
      "Plan the import of a read-only document as an editable worker "
      "package: every item an editable copy could not hold, by loss kind and "
      "effect with samples, the blockers that refuse the import, the "
      "lossless conversions and the size of the result, with a plan_digest "
      "for scene.import_editable to quote. The source scene is always "
      "attached to the package. Runs in the document's scene host and "
      "changes nothing.",
      {{"document_id",
        QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
       {"sample_limit",
        QJsonObject{
            {"type", "integer"}, {"minimum", 1}, {"maximum", MaximumSamples}}}},
      {"document_id"})};
}

QJsonValue executeImport(Context& context, const QString& method,
                         const QJsonObject& params) {
  if (method == "scene.import_plan") return importPlan(context, params);
  throw Error("method_not_found", "The import operation is not available.");
}

}  // namespace GroundzeroAutomation
