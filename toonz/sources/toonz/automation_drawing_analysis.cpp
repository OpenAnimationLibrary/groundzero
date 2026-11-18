#include "automation_drawing_analysis.h"

#include "automation_drawing_inspection.h"
#include "automation_jobs.h"

#include "tpalette.h"
#include "tpixel.h"
#include "traster.h"
#include "tregion.h"
#include "tstroke.h"
#include "ttoonzimage.h"
#include "tvectorimage.h"
#include "toonz/levelproperties.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace GroundzeroAutomation {
namespace {

enum class Check { StyleUsage, Regions, Roles };
enum class State {
  Preparing,
  Running,
  Completed,
  Failed,
  Canceled,
  DeadlineExceeded
};

constexpr int kMaxDrawingIds   = 4096;
constexpr int kMaxStyleIds     = 64;
constexpr int kMaxStyles       = 4096;
constexpr int kRegionStyleRows = 256;

[[noreturn]] void invalid(const QString& message) {
  throw Error("invalid_params", message);
}

void requireKeys(const QJsonObject& object, const QStringList& allowed) {
  for (auto it = object.begin(); it != object.end(); ++it)
    if (!allowed.contains(it.key())) invalid("Unknown field: " + it.key());
}

Check parseCheck(const QString& text) {
  if (text == "style_usage") return Check::StyleUsage;
  if (text == "regions") return Check::Regions;
  if (text == "roles") return Check::Roles;
  invalid("check must be style_usage, regions or roles");
}

int boundedInteger(const QJsonObject& params, const QString& key, int minimum,
                   int maximum, int fallback) {
  return integerParam(params, key, minimum, maximum, fallback);
}

TFrameId parseDrawingId(const QJsonValue& value) {
  if (!value.isObject()) invalid("Each drawing id must be an object");
  const QJsonObject object = value.toObject();
  requireKeys(object, {"number", "suffix"});
  const int number = boundedInteger(object, "number", 1, 999999, -1);
  if (number < 0) invalid("Drawing number is required");
  const QJsonValue suffix = object.value("suffix");
  if (!suffix.isString() ||
      !QRegularExpression(QRegularExpression::anchoredPattern("[a-z]{0,8}"))
           .match(suffix.toString())
           .hasMatch())
    invalid("Drawing suffix must contain zero to eight lowercase letters");
  return TFrameId(number, suffix.toString());
}

QJsonObject drawingJson(const TFrameId& frame) {
  return {{"number", frame.getNumber()}, {"suffix", frame.getLetter()}};
}

struct Estimate final {
  qint64 drawings      = 0;
  qint64 strokes       = 0;
  qint64 controlPoints = 0;
  qint64 pixels        = 0;
  QJsonObject json() const {
    return {{"drawings", double(drawings)},
            {"strokes", double(strokes)},
            {"control_points", double(controlPoints)},
            {"pixels", double(pixels)},
            {"files", 0},
            {"bytes", 0}};
  }
};

struct Cost final {
  qint64 drawingsDone   = 0;
  qint64 strokesScanned = 0;
  qint64 regionsBuilt   = 0;
  qint64 pixelsScanned  = 0;
};

struct StyleTotal final {
  qint64 drawings = 0;
  qint64 strokes  = 0;
  qint64 regions  = 0;
  qint64 ink      = 0;
  qint64 paint    = 0;
};

struct Plan final {
  Check check = Check::StyleUsage;
  QString levelId;
  std::vector<TFrameId> frames;
  std::set<int> styles;
  int deadlineMs  = 120000;
  int maxDrawings = kMaxDrawingIds;
  QJsonObject request;
};

bool isVectorLevel(const TXshSimpleLevel* level) {
  return level->getType() == PLI_XSHLEVEL;
}

TXshSimpleLevel* analysedLevel(Document& document, const QString& levelId,
                               Check check) {
  auto* level = document.level(levelId)->getSimpleLevel();
  if (!level || (!isVectorLevel(level) && level->getType() != TZP_XSHLEVEL))
    throw Error("unsupported_type",
                "Drawing analysis admits vector and Toonz raster levels.");
  if (check != Check::StyleUsage && !isVectorLevel(level))
    throw Error("unsupported_type",
                "Regions and roles are analysed on vector levels only; Toonz "
                "raster levels admit style_usage.");
  return level;
}

QJsonArray identityArray(const std::vector<TFrameId>& frames) {
  QJsonArray result;
  for (const TFrameId& frame : frames) result.append(drawingJson(frame));
  return result;
}

Plan readPlan(Document& document, const QJsonObject& params,
              const AnalysisBudget& budget) {
  requireKeys(params, {"document_id", "expected_revision", "operation_id",
                       "level_id", "check", "drawing_ids", "style_ids",
                       "deadline_ms", "max_drawings"});
  Plan plan;
  plan.check   = parseCheck(requiredString(params, "check"));
  plan.levelId = requiredString(params, "level_id");
  auto* level  = analysedLevel(document, plan.levelId, plan.check);
  plan.deadlineMs =
      boundedInteger(params, "deadline_ms", 1000, 600000, budget.deadlineMs);
  plan.maxDrawings = boundedInteger(params, "max_drawings", 1, kMaxDrawingIds,
                                    budget.drawings);
  plan.request.insert("level_id", plan.levelId);
  plan.request.insert("check", requiredString(params, "check"));
  plan.request.insert("deadline_ms", plan.deadlineMs);
  plan.request.insert("max_drawings", plan.maxDrawings);
  if (!params.contains("drawing_ids")) {
    plan.frames = level->getFids();
  } else {
    const QJsonValue ids = params.value("drawing_ids");
    if (!ids.isArray() || ids.toArray().isEmpty() ||
        ids.toArray().size() > kMaxDrawingIds)
      invalid("drawing_ids must hold 1 to 4096 drawing ids");
    std::set<std::pair<int, QString>> seen;
    for (const QJsonValue& entry : ids.toArray()) {
      const TFrameId frame = parseDrawingId(entry);
      if (!seen.insert({frame.getNumber(), frame.getLetter()}).second)
        invalid("drawing_ids must be unique");
      if (!level->isFid(frame))
        throw Error("not_found", "Drawing identity does not exist");
      plan.frames.push_back(frame);
    }
    plan.request.insert("drawing_ids", identityArray(plan.frames));
  }
  if (params.contains("style_ids")) {
    if (plan.check != Check::StyleUsage)
      invalid("style_ids applies to the style_usage check only");
    const QJsonValue ids = params.value("style_ids");
    if (!ids.isArray() || ids.toArray().isEmpty() ||
        ids.toArray().size() > kMaxStyleIds)
      invalid("style_ids must hold 1 to 64 style ids");
    QJsonArray echoed;
    for (const QJsonValue& entry : ids.toArray()) {
      const QJsonObject wrapper{{"style_id", entry}};
      plan.styles.insert(
          boundedInteger(wrapper, "style_id", 0, kMaxStyles - 1, -1));
    }
    for (int style : plan.styles) echoed.append(style);
    plan.request.insert("style_ids", echoed);
  }
  return plan;
}

Estimate estimateFor(TXshSimpleLevel* level,
                     const std::vector<TFrameId>& frames) {
  Estimate estimate;
  estimate.drawings = qint64(frames.size());
  if (isVectorLevel(level)) {
    for (const TFrameId& frame : frames) {
      const TVectorImageP image = level->getFrame(frame, false);
      if (!image) continue;
      estimate.strokes += image->getStrokeCount();
      for (UINT index = 0; index < image->getStrokeCount(); ++index)
        estimate.controlPoints +=
            image->getStroke(index)->getControlPointCount();
    }
    return estimate;
  }
  const TDimension size = level->getProperties()->getImageRes();
  estimate.pixels       = qint64(size.lx) * size.ly * qint64(frames.size());
  return estimate;
}

void refuseOverBudget(const Estimate& estimate, const Plan& plan,
                      const AnalysisBudget& budget) {
  QString limit;
  if (estimate.drawings > plan.maxDrawings)
    limit = "max_drawings";
  else if (estimate.strokes > budget.strokes)
    limit = "strokes";
  else if (estimate.pixels > budget.pixels)
    limit = "pixels";
  if (limit.isEmpty()) return;
  throw Error("resource_limit",
              "The analysis estimate is over its budget; narrow drawing_ids.",
              {{"limit", limit}, {"estimate", estimate.json()}});
}

QJsonObject drawingRow(const QString& kind, const QString& levelId,
                       const TFrameId& frame) {
  return {{"row_kind", kind},
          {"level_id", levelId},
          {"drawing_id", drawingJson(frame)}};
}

class DrawingAnalysisJob final
    : public Job,
      public std::enable_shared_from_this<DrawingAnalysisJob> {
public:
  Context* context = nullptr;
  JobCommon common;
  AnalysisBudget budget;
  Plan plan;
  Estimate estimate;
  Cost cost;
  TPaletteP palette;
  State state = State::Preparing;
  QStringList reasons;
  std::vector<QJsonObject> rows;
  std::map<int, StyleTotal> totals;
  size_t next = 0;
  std::atomic<qint64> interruptMs{-1};

  QString id() const override { return common.id; }
  JobLane lane() const override { return JobLane::Analysis; }
  QString documentId() const override { return common.documentId; }
  bool terminal() const override {
    return state == State::Completed || state == State::Failed ||
           state == State::Canceled || state == State::DeadlineExceeded;
  }
  qint64 reservedBytes() const override { return 0; }
  QJsonObject record() const override;
  QJsonValue result(Context& owner, const QJsonObject& params) override;

  // A request made on the input thread keeps the time it was made.
  void cancel() override {
    if (common.cancelRequested) return;
    const qint64 asked     = interruptMs.load();
    common.cancelRequested = true;
    common.cancelMs        = asked >= 0 ? asked : now();
  }

  // The work runs in application-thread slices that check cancel() between
  // them, so an interrupt only records when the request was made.
  void interrupt(qint64 requestedMs) override {
    qint64 unset = -1;
    interruptMs.compare_exchange_strong(unset, requestedMs);
  }

  qint64 now() const { return jobRegistry(*context).elapsedMs(); }

  void schedule() {
    QTimer::singleShot(0, QCoreApplication::instance(),
                       [self = shared_from_this()]() { self->tick(); });
  }

  // One slice of work: at most one drawing, or one batch of raster rows.
  // Returns whether more slices remain.
  bool step();

private:
  struct RasterScan final {
    TToonzImageP image;
    TFrameId frame;
    int row = 0;
    std::vector<qint64> ink;
    std::vector<qint64> paint;
  };
  std::optional<RasterScan> m_raster;

  void tick() {
    if (step()) schedule();
  }
  bool guardedStep();
  void requireRevision() const;
  bool processDrawing();
  bool stepRaster();
  void analyzeVector(const TFrameId& frame, const TVectorImageP& image,
                     std::vector<QJsonObject>& out,
                     std::map<int, StyleTotal>& drawingTotals);
  bool admitRows(const std::vector<QJsonObject>& drawingRows);
  bool stopForBudget(const QString& reason);
  void markUnreadable();
  void finish(State end);
  void fail(const QString& code, const QString& message);
  void appendTotals();
  void addTotal(int style, const StyleTotal& amount);
  bool wantsStyle(int style) const {
    return plan.styles.empty() || plan.styles.count(style) > 0;
  }
  QString statusText() const;
  QJsonObject analysisRecord() const;
};

QString DrawingAnalysisJob::statusText() const {
  switch (state) {
  case State::Completed:
    return "completed";
  case State::Failed:
    return "failed";
  case State::Canceled:
    return "canceled";
  case State::DeadlineExceeded:
    return "deadline_exceeded";
  case State::Preparing:
  case State::Running:
    break;
  }
  if (common.cancelRequested) return "cancellation_requested";
  return state == State::Preparing ? "preparing" : "running";
}

QJsonObject DrawingAnalysisJob::analysisRecord() const {
  qint64 elapsed = 0;
  if (common.startedMs >= 0)
    elapsed = (common.endedMs >= 0 ? common.endedMs : now()) - common.startedMs;
  QJsonValue complete;
  if (state == State::Completed || state == State::Canceled ||
      state == State::DeadlineExceeded)
    complete = reasons.isEmpty() && state == State::Completed;
  const QString check = plan.request.value("check").toString();
  return {{"check", check},
          {"estimate", estimate.json()},
          {"cost", QJsonObject{{"elapsed_ms", double(elapsed)},
                               {"drawings_done", double(cost.drawingsDone)},
                               {"strokes_scanned", double(cost.strokesScanned)},
                               {"regions_built", double(cost.regionsBuilt)},
                               {"pixels_scanned", double(cost.pixelsScanned)},
                               {"files_checked", 0},
                               {"bytes_read", 0},
                               {"helpers_started", 0}}},
          {"budget", QJsonObject{{"max_drawings", plan.maxDrawings},
                                 {"max_strokes", double(budget.strokes)},
                                 {"max_pixels", double(budget.pixels)},
                                 {"max_files", 1},
                                 {"max_bytes", 0}}},
          {"complete", complete},
          {"reasons", QJsonArray::fromStringList(reasons)}};
}

QJsonObject DrawingAnalysisJob::record() const {
  const QJsonObject limits{{"deadline_ms", plan.deadlineMs},
                           {"storage_reserved_bytes", 0},
                           {"cancel_grace_ms", CancelGraceMs}};
  const QJsonObject progress{{"unit", "drawings"},
                             {"done", double(cost.drawingsDone)},
                             {"total", double(estimate.drawings)}};
  const QJsonObject outputs{{"directory", QJsonValue()},
                            {"artifact_count", 0},
                            {"result_rows", int(rows.size())}};
  QJsonObject result = commonRecord(*context, common, statusText(), terminal(),
                                    limits, progress, outputs);
  result.insert("analysis", analysisRecord());
  return result;
}

QJsonValue DrawingAnalysisJob::result(Context&, const QJsonObject& params) {
  if (!terminal())
    throw Error("result_unavailable",
                "The job has not finished; poll job.status.",
                {{"status", statusText()}});
  if (state == State::Failed)
    throw Error("result_unavailable", "The job failed and has no rows.",
                {{"status", "failed"}, {"failure", common.failure}});
  const int offset = integerParam(params, "offset", 0, 65536, 0);
  const int limit  = integerParam(params, "limit", 1, 256, 64);
  QJsonArray page;
  for (int index = offset; index < int(rows.size()) && page.size() < limit;
       ++index)
    page.append(rows[size_t(index)]);
  const int end              = offset + page.size();
  const QJsonObject analysis = analysisRecord();
  return QJsonObject{
      {"job_id", common.id},
      {"kind", common.kind},
      {"check", analysis.value("check")},
      {"status", statusText()},
      {"complete", analysis.value("complete")},
      {"reasons", analysis.value("reasons")},
      {"summary", QJsonObject{{"drawings_done", double(cost.drawingsDone)},
                              {"drawings_total", double(estimate.drawings)},
                              {"strokes_scanned", double(cost.strokesScanned)},
                              {"regions_built", double(cost.regionsBuilt)},
                              {"pixels_scanned", double(cost.pixelsScanned)}}},
      {"rows", page},
      {"total_rows", int(rows.size())},
      {"offset", offset},
      {"next_offset", end < int(rows.size()) ? QJsonValue(end) : QJsonValue()},
      {"source_revision", common.revision}};
}

void DrawingAnalysisJob::requireRevision() const {
  Document& document = context->document(common.documentId);
  if (document.revisionText() != common.revision)
    throw Error("revision_changed",
                "The document changed after the analysis began.",
                {{"revision", document.revisionText()}});
}

void DrawingAnalysisJob::fail(const QString& code, const QString& message) {
  common.failure = QJsonObject{
      {"code", code}, {"phase", "analyze"}, {"message", message.left(2048)}};
  rows.clear();
  totals.clear();
  reasons.clear();
  m_raster.reset();
  state          = State::Failed;
  common.endedMs = now();
}

void DrawingAnalysisJob::finish(State end) {
  if (end == State::Canceled) reasons.append("canceled");
  if (end == State::DeadlineExceeded) reasons.append("deadline");
  appendTotals();
  m_raster.reset();
  state          = end;
  common.endedMs = now();
}

bool DrawingAnalysisJob::stopForBudget(const QString& reason) {
  if (!reasons.contains(reason)) reasons.append(reason);
  finish(State::Completed);
  return false;
}

void DrawingAnalysisJob::markUnreadable() {
  if (!reasons.contains("unreadable")) reasons.append("unreadable");
  ++next;
  ++cost.drawingsDone;
}

bool DrawingAnalysisJob::guardedStep() {
  try {
    if (common.startedMs < 0) {
      common.startedMs = now();
      state            = State::Running;
      palette          = context->document(common.documentId)
                             .level(plan.levelId)
                             ->getSimpleLevel()
                             ->getPalette();
    }
    if (common.cancelRequested) {
      finish(State::Canceled);
      return false;
    }
    if (now() - common.startedMs > plan.deadlineMs) {
      finish(State::DeadlineExceeded);
      return false;
    }
    requireRevision();
    if (m_raster) return stepRaster();
    if (next >= plan.frames.size()) {
      finish(State::Completed);
      return false;
    }
    return processDrawing();
  } catch (const Error& error) {
    fail(error.code, error.message);
  } catch (const std::exception& error) {
    fail("native_error", QString::fromUtf8(error.what()));
  } catch (...) {
    fail("native_error", "The native analysis raised an unrecognized error.");
  }
  return false;
}

bool DrawingAnalysisJob::step() {
  if (terminal()) return false;
  return guardedStep();
}

bool DrawingAnalysisJob::admitRows(
    const std::vector<QJsonObject>& drawingRows) {
  const size_t capacity = size_t(budget.rows - budget.rowsForTotals);
  if (rows.size() + drawingRows.size() > capacity) return false;
  rows.insert(rows.end(), drawingRows.begin(), drawingRows.end());
  return true;
}

void DrawingAnalysisJob::addTotal(int style, const StyleTotal& amount) {
  StyleTotal& total = totals[style];
  ++total.drawings;
  total.strokes += amount.strokes;
  total.regions += amount.regions;
  total.ink += amount.ink;
  total.paint += amount.paint;
}

void DrawingAnalysisJob::appendTotals() {
  if (plan.check != Check::StyleUsage) return;
  for (const auto& [style, total] : totals) {
    const bool defined = palette && style < palette->getStyleCount() &&
                         palette->getStyle(style) != nullptr;
    rows.push_back(QJsonObject{{"row_kind", "style_total"},
                               {"level_id", plan.levelId},
                               {"style_id", style},
                               {"defined", defined},
                               {"drawings", double(total.drawings)},
                               {"strokes", double(total.strokes)},
                               {"regions", double(total.regions)},
                               {"ink_pixels", double(total.ink)},
                               {"paint_pixels", double(total.paint)}});
  }
}

void DrawingAnalysisJob::analyzeVector(
    const TFrameId& frame, const TVectorImageP& image,
    std::vector<QJsonObject>& out, std::map<int, StyleTotal>& drawingTotals) {
  cost.strokesScanned += image->getStrokeCount();
  if (plan.check == Check::Roles) {
    const DrawingRoles roles = drawingRoles(*image);
    QJsonObject row          = drawingRow("role_summary", plan.levelId, frame);
    for (int role = 0; role < 4; ++role)
      row.insert(roleName(role), roles.counts[role]);
    row.insert("layered", roles.layered);
    row.insert("colour_first", roles.colourFirst < 0
                                   ? QJsonValue()
                                   : QJsonValue(roles.colourFirst));
    row.insert("colour_last", roles.colourLast < 0
                                  ? QJsonValue()
                                  : QJsonValue(roles.colourLast));
    out.push_back(row);
    return;
  }
  const TVectorImageP detached = image->clone();
  detached->findRegions();
  const int regionCount = int(detached->getRegionCount());
  cost.regionsBuilt += regionCount;
  std::map<int, int> byStyle;
  for (int index = 0; index < regionCount; ++index)
    ++byStyle[detached->getRegion(index)->getStyle()];
  if (plan.check == Check::Regions) {
    QJsonArray styles;
    int painted = 0;
    for (const auto& [style, count] : byStyle) {
      if (style != 0) painted += count;
      if (styles.size() < kRegionStyleRows)
        styles.append(QJsonObject{{"style_id", style}, {"regions", count}});
    }
    QJsonObject row = drawingRow("region_summary", plan.levelId, frame);
    row.insert("regions", regionCount);
    row.insert("painted", painted);
    row.insert("unpainted", regionCount - painted);
    row.insert("by_style", styles);
    row.insert("by_style_truncated", int(byStyle.size()) > kRegionStyleRows);
    row.insert("built_on", "detached_clone");
    out.push_back(row);
    return;
  }
  std::map<int, StyleTotal> usage;
  for (const auto& [style, strokes] : strokeStyleUsage(*image))
    usage[style].strokes = strokes;
  for (const auto& [style, count] : byStyle) usage[style].regions = count;
  for (const auto& [style, amount] : usage) {
    if (!wantsStyle(style)) continue;
    QJsonObject row = drawingRow("style_usage", plan.levelId, frame);
    row.insert("style_id", style);
    row.insert("strokes", double(amount.strokes));
    row.insert("regions", double(amount.regions));
    row.insert("ink_pixels", 0);
    row.insert("paint_pixels", 0);
    out.push_back(row);
    drawingTotals[style] = amount;
  }
}

bool DrawingAnalysisJob::processDrawing() {
  Document& document   = context->document(common.documentId);
  auto* level          = document.level(plan.levelId)->getSimpleLevel();
  const TFrameId frame = plan.frames[next];
  const TImageP image =
      level->isFid(frame) ? level->getFrame(frame, false) : TImageP();
  if (const TVectorImageP vector = image) {
    if (cost.strokesScanned + qint64(vector->getStrokeCount()) > budget.strokes)
      return stopForBudget("stroke_budget");
    std::vector<QJsonObject> drawingRows;
    std::map<int, StyleTotal> drawingTotals;
    analyzeVector(frame, vector, drawingRows, drawingTotals);
    if (!admitRows(drawingRows)) return stopForBudget("row_limit");
    for (const auto& [style, amount] : drawingTotals) addTotal(style, amount);
    ++next;
    ++cost.drawingsDone;
    return true;
  }
  if (const TToonzImageP raster = image) {
    const TRasterCM32P pixels = raster->getRaster();
    if (!pixels) {
      markUnreadable();
      return true;
    }
    const qint64 count = qint64(pixels->getLx()) * pixels->getLy();
    if (cost.pixelsScanned + count > budget.pixels)
      return stopForBudget("pixel_budget");
    RasterScan scan;
    scan.image = raster;
    scan.frame = frame;
    scan.ink.assign(kMaxStyles, 0);
    scan.paint.assign(kMaxStyles, 0);
    m_raster = std::move(scan);
    return stepRaster();
  }
  markUnreadable();
  return true;
}

bool DrawingAnalysisJob::stepRaster() {
  RasterScan& scan          = *m_raster;
  const TRasterCM32P pixels = scan.image->getRaster();
  const int width           = pixels->getLx();
  const int height          = pixels->getLy();
  const int batch = std::max(1, budget.pixelsPerSlice / std::max(1, width));
  const int end   = std::min(height, scan.row + batch);
  pixels->lock();
  for (int y = scan.row; y < end; ++y) {
    const TPixelCM32* pixel = pixels->pixels(y);
    for (int x = 0; x < width; ++x) {
      ++scan.ink[pixel[x].getInk() & (kMaxStyles - 1)];
      ++scan.paint[pixel[x].getPaint() & (kMaxStyles - 1)];
    }
  }
  pixels->unlock();
  cost.pixelsScanned += qint64(end - scan.row) * width;
  scan.row = end;
  if (scan.row < height) return true;
  std::vector<QJsonObject> drawingRows;
  std::map<int, StyleTotal> drawingTotals;
  for (int style = 0; style < kMaxStyles; ++style) {
    if ((scan.ink[style] == 0 && scan.paint[style] == 0) || !wantsStyle(style))
      continue;
    QJsonObject row = drawingRow("style_usage", plan.levelId, scan.frame);
    row.insert("style_id", style);
    row.insert("strokes", 0);
    row.insert("regions", 0);
    row.insert("ink_pixels", double(scan.ink[style]));
    row.insert("paint_pixels", double(scan.paint[style]));
    drawingRows.push_back(row);
    drawingTotals[style].ink   = scan.ink[style];
    drawingTotals[style].paint = scan.paint[style];
  }
  if (!admitRows(drawingRows)) return stopForBudget("row_limit");
  for (const auto& [style, amount] : drawingTotals) addTotal(style, amount);
  m_raster.reset();
  ++next;
  ++cost.drawingsDone;
  return true;
}

}  // namespace

QJsonObject drawingAnalysisOperation() {
  const QJsonObject text{
      {"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
  const QJsonObject drawing{
      {"type", "object"},
      {"additionalProperties", false},
      {"properties",
       QJsonObject{{"number", QJsonObject{{"type", "integer"},
                                          {"minimum", 1},
                                          {"maximum", 999999}}},
                   {"suffix", QJsonObject{{"type", "string"},
                                          {"pattern", "^[a-z]{0,8}$"}}}}},
      {"required", QJsonArray{"number", "suffix"}}};
  const auto bounded = [](int minimum, int maximum) {
    return QJsonObject{
        {"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
  };
  QJsonObject properties{
      {"document_id", text},
      {"level_id", text},
      {"check",
       QJsonObject{{"type", "string"},
                   {"enum", QJsonArray{"style_usage", "regions", "roles"}}}},
      {"drawing_ids", QJsonObject{{"type", "array"},
                                  {"items", drawing},
                                  {"minItems", 1},
                                  {"maxItems", kMaxDrawingIds}}},
      {"style_ids", QJsonObject{{"type", "array"},
                                {"items", bounded(0, kMaxStyles - 1)},
                                {"minItems", 1},
                                {"maxItems", kMaxStyleIds}}},
      {"deadline_ms", bounded(1000, 600000)},
      {"max_drawings", bounded(1, kMaxDrawingIds)}};
  QJsonObject descriptor = operation(
      "drawing.analysis.submit",
      "Start a bounded, cancellable analysis job over the drawings of one "
      "level: per-style usage of strokes, regions and Toonz raster pixels, "
      "region summaries, or art-role summaries. Regions are built on "
      "detached clones. Read the rows with job.result.",
      properties, {"document_id", "level_id", "check"}, true);
  descriptor.insert("mutates_document", false);
  QJsonObject schema   = descriptor.value("inputSchema").toObject();
  QJsonObject allProps = schema.value("properties").toObject();
  allProps.insert(
      "expected_revision",
      QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 80}});
  allProps.insert("operation_id", text);
  schema.insert("properties", allProps);
  descriptor.insert("inputSchema", schema);
  descriptor.insert("annotations", QJsonObject{{"readOnlyHint", false},
                                               {"destructiveHint", false},
                                               {"idempotentHint", true},
                                               {"openWorldHint", false}});
  return descriptor;
}

namespace {

std::shared_ptr<DrawingAnalysisJob> startAnalysis(
    Context& context, Document& document, const QJsonObject& params,
    const AnalysisBudget& budget) {
  const Plan plan       = readPlan(document, params, budget);
  JobRegistry& registry = jobRegistry(context);
  registry.admitAnalysis({}, 0);
  auto* level             = document.level(plan.levelId)->getSimpleLevel();
  const Estimate estimate = estimateFor(level, plan.frames);
  refuseOverBudget(estimate, plan, budget);
  auto job                = std::make_shared<DrawingAnalysisJob>();
  job->context            = &context;
  job->budget             = budget;
  job->plan               = plan;
  job->estimate           = estimate;
  job->common.id          = registry.newJobId();
  job->common.kind        = "drawing_analysis";
  job->common.lane        = JobLane::Analysis;
  job->common.documentId  = document.id;
  job->common.revision    = document.revisionText();
  job->common.request     = plan.request;
  job->common.submittedMs = registry.elapsedMs();
  context.markMutationStarted();
  registry.add(job);
  return job;
}

}  // namespace

QJsonObject submitDrawingAnalysis(Context& context, Document& document,
                                  const QJsonObject& params) {
  const auto job = startAnalysis(context, document, params, AnalysisBudget());
  job->schedule();
  return job->record();
}

#ifdef BUILD_UI_PORT_TESTS
QJsonObject startDrawingAnalysisUnscheduled(Context& context,
                                            Document& document,
                                            const QJsonObject& params,
                                            const AnalysisBudget& budget) {
  return startAnalysis(context, document, params, budget)->record();
}

bool stepDrawingAnalysis(Context& context, const QString& jobId) {
  const auto job = std::dynamic_pointer_cast<DrawingAnalysisJob>(
      jobRegistry(context).require(jobId));
  return job && job->step();
}
#endif

}  // namespace GroundzeroAutomation
