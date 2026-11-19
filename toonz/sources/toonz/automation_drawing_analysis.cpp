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
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QJsonDocument>
#include <QRunnable>
#include <QThreadPool>
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

// Drawings estimated before submit returns; the job estimates the rest in
// slices, so submit's work is bounded however large the level is.
constexpr size_t kSubmitEstimateDrawings = 32;
constexpr int kEstimateSliceMs           = 20;

void addDrawingEstimate(TXshSimpleLevel* level, const TFrameId& frame,
                        Estimate& estimate) {
  const TVectorImageP image = level->getFrame(frame, false);
  if (!image) return;
  estimate.strokes += image->getStrokeCount();
  for (UINT index = 0; index < image->getStrokeCount(); ++index)
    estimate.controlPoints += image->getStroke(index)->getControlPointCount();
}

// The estimate of the first `count` drawings of a vector level, or of the
// whole level when it is raster, whose pixels follow from its resolution.
Estimate estimateFor(TXshSimpleLevel* level,
                     const std::vector<TFrameId>& frames, size_t count) {
  Estimate estimate;
  estimate.drawings = qint64(frames.size());
  if (isVectorLevel(level)) {
    for (size_t index = 0; index < std::min(count, frames.size()); ++index)
      addDrawingEstimate(level, frames[index], estimate);
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

constexpr int kMaxDrawingStrokes = 4096;

// Regions of one detached clone, copied and built off the application thread.
// The copy reads the document's drawing under the drawing's own lock, as the
// library's other readers do; everything after it touches only the clone.
struct Rebuild final {
  TVectorImageP source;
  TVectorImageP image;
  std::map<int, int> byStyle;
  int regions = 0;
  QString error;
  std::atomic<bool> done{false};
  // The job was cancelled: nothing is delivered and nothing shared is touched
  // from here on, apart from a copy already under way.
  std::atomic<bool> abandoned{false};
};

void runRebuild(const std::shared_ptr<Rebuild>& rebuild) {
  try {
    if (!rebuild->abandoned) {
      {
        QMutexLocker lock(rebuild->source->getMutex());
        rebuild->image = rebuild->source->cloneWithoutRegions();
      }
      rebuild->source = TVectorImageP();
      rebuild->image->findRegions();
      rebuild->regions = int(rebuild->image->getRegionCount());
      for (int index = 0; index < rebuild->regions; ++index)
        ++rebuild->byStyle[rebuild->image->getRegion(index)->getStyle()];
    }
  } catch (const std::exception& error) {
    rebuild->error = QString::fromUtf8(error.what());
  } catch (...) {
    rebuild->error = "The native region rebuild raised an unrecognized error.";
  }
  rebuild->source = TVectorImageP();
  rebuild->image  = TVectorImageP();
  rebuild->done   = true;
}

// One region rebuild runs at a time, as the lane admits one analysis job; a
// cancelled job's rebuild runs to its end on the same thread, so the next job's
// rebuild waits for it. The worker does not wait for a rebuild at exit: the
// task works on a private clone, and the process ends with it.
QThreadPool& rebuildPool() {
  static QThreadPool* pool = []() {
    auto* created = new QThreadPool();
    created->setMaxThreadCount(1);
    return created;
  }();
  return *pool;
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
  // The first drawing whose strokes the estimate has not counted yet.
  size_t estimateNext = 0;
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
    // A region rebuild in flight cannot be stopped, but its result is no
    // longer wanted: the job ends now and the task, which works on a private
    // clone and touches nothing shared, finishes unseen or ends with the
    // process.
    if (m_pending && !terminal()) {
      m_pending->rebuild->abandoned = true;
      m_pending.reset();
      finish(State::Canceled);
    }
  }

  // Any thread: a poll's cancellation, recorded as made at `requestedMs`
  // until cancel() runs on the application thread.
  void interrupt(qint64 requestedMs) override {
    qint64 unset = -1;
    interruptMs.compare_exchange_strong(unset, requestedMs);
  }

  // Run region rebuilds in the slice (test hook for stepping by hand).
  bool rebuildInline = false;
  // The longest slice the application thread spent in this job.
  qint64 longestSliceMs = 0;

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
  struct Pending final {
    TFrameId frame;
    std::map<int, int> strokes;
    std::shared_ptr<Rebuild> rebuild;
  };
  std::optional<Pending> m_pending;

  void rebuilt() {
    if (!terminal()) schedule();
  }
  bool startRebuild(const TFrameId& frame, const TVectorImageP& image);
  bool finishRebuild();
  // Each slice is a posted event, so the event loop does not go idle while
  // the job runs; publishing here keeps polls answered elsewhere current.
  void tick() {
    const bool more = step();
    jobRegistry(*context).publish();
    if (more) schedule();
  }
  bool guardedStep();
  bool estimateSlice();
  void requireRevision() const;
  bool processDrawing();
  bool stepRaster();
  bool roleRow(const TFrameId& frame, const TVectorImageP& image);
  void regionRows(const Pending& pending, std::vector<QJsonObject>& out,
                  std::map<int, StyleTotal>& drawingTotals);
  bool admitDrawing(const std::vector<QJsonObject>& drawingRows,
                    const std::map<int, StyleTotal>& drawingTotals);
  bool admitRows(const std::vector<QJsonObject>& drawingRows);
  bool stopForBudget(const QString& reason);
  void markUnreadable();
  void finish(State end);
  void fail(const QString& code, const QString& message,
            const QString& phase = "analyze");
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

void DrawingAnalysisJob::fail(const QString& code, const QString& message,
                              const QString& phase) {
  common.failure = QJsonObject{
      {"code", code}, {"phase", phase}, {"message", message.left(2048)}};
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
    if (estimateNext < plan.frames.size()) return estimateSlice();
    if (m_pending) return m_pending->rebuild->done && finishRebuild();
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

// The first phase of a job whose level is larger than submit estimates: count
// the remaining drawings' strokes for a few milliseconds per slice, then refuse
// the whole request, as submit would have, if the estimate is over budget.
bool DrawingAnalysisJob::estimateSlice() {
  auto* level = context->document(common.documentId)
                    .level(plan.levelId)
                    ->getSimpleLevel();
  QElapsedTimer timer;
  timer.start();
  do {
    addDrawingEstimate(level, plan.frames[estimateNext], estimate);
    ++estimateNext;
  } while (estimateNext < plan.frames.size() &&
           timer.elapsed() < kEstimateSliceMs);
  try {
    refuseOverBudget(estimate, plan, budget);
  } catch (const Error& error) {
    fail(error.code,
         error.message + " " +
             QString::fromUtf8(
                 QJsonDocument(error.details).toJson(QJsonDocument::Compact)),
         "prepare");
    return false;
  }
  return true;
}

bool DrawingAnalysisJob::step() {
  if (terminal()) return false;
  QElapsedTimer timer;
  timer.start();
  const bool more = guardedStep();
  longestSliceMs  = std::max<qint64>(longestSliceMs, timer.elapsed());
  return more;
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

bool DrawingAnalysisJob::roleRow(const TFrameId& frame,
                                 const TVectorImageP& image) {
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
  return admitDrawing({row}, {});
}

void DrawingAnalysisJob::regionRows(const Pending& pending,
                                    std::vector<QJsonObject>& out,
                                    std::map<int, StyleTotal>& drawingTotals) {
  const Rebuild& built = *pending.rebuild;
  if (plan.check == Check::Regions) {
    QJsonArray styles;
    int painted = 0;
    for (const auto& [style, count] : built.byStyle) {
      if (style != 0) painted += count;
      if (styles.size() < kRegionStyleRows)
        styles.append(QJsonObject{{"style_id", style}, {"regions", count}});
    }
    QJsonObject row = drawingRow("region_summary", plan.levelId, pending.frame);
    row.insert("regions", built.regions);
    row.insert("painted", painted);
    row.insert("unpainted", built.regions - painted);
    row.insert("by_style", styles);
    row.insert("by_style_truncated",
               int(built.byStyle.size()) > kRegionStyleRows);
    row.insert("built_on", "detached_clone");
    out.push_back(row);
    return;
  }
  std::map<int, StyleTotal> usage;
  for (const auto& [style, strokes] : pending.strokes)
    usage[style].strokes = strokes;
  for (const auto& [style, count] : built.byStyle) usage[style].regions = count;
  for (const auto& [style, amount] : usage) {
    if (!wantsStyle(style)) continue;
    QJsonObject row = drawingRow("style_usage", plan.levelId, pending.frame);
    row.insert("style_id", style);
    row.insert("strokes", double(amount.strokes));
    row.insert("regions", double(amount.regions));
    row.insert("ink_pixels", 0);
    row.insert("paint_pixels", 0);
    out.push_back(row);
    drawingTotals[style] = amount;
  }
}

bool DrawingAnalysisJob::admitDrawing(
    const std::vector<QJsonObject>& drawingRows,
    const std::map<int, StyleTotal>& drawingTotals) {
  if (!admitRows(drawingRows)) return stopForBudget("row_limit");
  for (const auto& [style, amount] : drawingTotals) addTotal(style, amount);
  ++next;
  ++cost.drawingsDone;
  return true;
}

// The clone is made here; its regions are built on the rebuild pool so the
// application thread keeps answering. Returns whether slices remain to run.
bool DrawingAnalysisJob::startRebuild(const TFrameId& frame,
                                      const TVectorImageP& image) {
  Pending pending;
  pending.frame           = frame;
  pending.strokes         = strokeStyleUsage(*image);
  pending.rebuild         = std::make_shared<Rebuild>();
  pending.rebuild->source = image;
  const auto rebuild      = pending.rebuild;
  m_pending               = std::move(pending);
  if (rebuildInline) {
    runRebuild(rebuild);
    return finishRebuild();
  }
  const auto self = std::weak_ptr<DrawingAnalysisJob>(shared_from_this());
  rebuildPool().start(QRunnable::create([rebuild, self]() {
    runRebuild(rebuild);
    if (rebuild->abandoned) return;
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [self]() {
          if (const auto job = self.lock()) job->rebuilt();
        },
        Qt::QueuedConnection);
  }));
  return false;
}

bool DrawingAnalysisJob::finishRebuild() {
  const Pending pending = std::move(*m_pending);
  m_pending.reset();
  if (!pending.rebuild->error.isEmpty())
    throw Error("native_error", pending.rebuild->error);
  cost.regionsBuilt += pending.rebuild->regions;
  std::vector<QJsonObject> drawingRows;
  std::map<int, StyleTotal> drawingTotals;
  regionRows(pending, drawingRows, drawingTotals);
  return admitDrawing(drawingRows, drawingTotals);
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
    if (vector->getStrokeCount() > kMaxDrawingStrokes) {
      markUnreadable();
      return true;
    }
    cost.strokesScanned += vector->getStrokeCount();
    if (plan.check == Check::Roles) return roleRow(frame, vector);
    return startRebuild(frame, vector);
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
  descriptor.insert("starts_job", true);
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
  auto* level = document.level(plan.levelId)->getSimpleLevel();
  const Estimate estimate =
      estimateFor(level, plan.frames, kSubmitEstimateDrawings);
  refuseOverBudget(estimate, plan, budget);
  auto job          = std::make_shared<DrawingAnalysisJob>();
  job->context      = &context;
  job->budget       = budget;
  job->plan         = plan;
  job->estimate     = estimate;
  job->estimateNext = isVectorLevel(level) ? std::min(kSubmitEstimateDrawings,
                                                      plan.frames.size())
                                           : plan.frames.size();
  job->common.id    = registry.newJobId();
  job->common.kind  = "drawing_analysis";
  job->common.lane  = JobLane::Analysis;
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
  const auto job     = startAnalysis(context, document, params, budget);
  job->rebuildInline = true;
  return job->record();
}

QJsonObject startDrawingAnalysisScheduled(Context& context, Document& document,
                                          const QJsonObject& params,
                                          const AnalysisBudget& budget) {
  const auto job = startAnalysis(context, document, params, budget);
  job->schedule();
  return job->record();
}

bool stepDrawingAnalysis(Context& context, const QString& jobId) {
  const auto job = std::dynamic_pointer_cast<DrawingAnalysisJob>(
      jobRegistry(context).require(jobId));
  return job && job->step();
}

qint64 drawingAnalysisLongestSliceMs(Context& context, const QString& jobId) {
  const auto job = std::dynamic_pointer_cast<DrawingAnalysisJob>(
      jobRegistry(context).require(jobId));
  return job ? job->longestSliceMs : -1;
}
#endif

}  // namespace GroundzeroAutomation
