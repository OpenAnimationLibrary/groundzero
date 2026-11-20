#include "automation_scene_check.h"

#include "automation_jobs.h"
#include "automation_production.h"
#include "automation_scene_host.h"
#include "automation_files.h"
#include "automation_media_decode.h"
#include "automation_shared_parse.h"

#include "tcolorstyles.h"
#include "texception.h"
#include "tpalette.h"
#include "trastercm.h"
#include "tregion.h"
#include "tstroke.h"
#include "tsystem.h"
#include "ttoonzimage.h"
#include "tvectorimage.h"
#include "toonz/levelproperties.h"
#include "toonz/scenechecker.h"
#include "toonz/levelset.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshpalettelevel.h"
#include "toonz/txshsimplelevel.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <memory>
#include <map>
#include <set>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxRows           = 65536;
constexpr int MaxLevelIds       = 256;
constexpr qint64 MaxDrawings    = 1048576;
constexpr qint64 MaxPixels      = 17179869184LL;
constexpr int DefaultDeadlineMs = 300000;
constexpr int DefaultPage       = 64;
constexpr int MaxPage           = 256;
constexpr int MaxResultBytes    = 1048576;
// The media_decode budgets of the contract; these checks open no files of
// their own, so they report them unchanged.
constexpr int UnusedFileBudget    = 4096;
constexpr qint64 UnusedByteBudget = 2147483648LL;
constexpr int DefaultMediaFiles    = UnusedFileBudget;
constexpr int MaxMediaFiles        = 65536;
constexpr qint64 DefaultMediaBytes = UnusedByteBudget;
constexpr qint64 MaxMediaBytes     = 17179869184LL;

enum class State { Running, Completed, Failed, Canceled, DeadlineExceeded };

QString stateText(State state) {
  switch (state) {
  case State::Running:
    return "running";
  case State::Completed:
    return "completed";
  case State::Failed:
    return "failed";
  case State::Canceled:
    return "canceled";
  case State::DeadlineExceeded:
    return "deadline_exceeded";
  }
  return "failed";
}

struct Cost final {
  qint64 drawings = 0;
  qint64 strokes  = 0;
  qint64 regions  = 0;
  qint64 pixels   = 0;
  qint64 files    = 0;
  qint64 bytes    = 0;
  qint64 helpers  = 0;
};

// How long a job that waits on a helper process sleeps between polls.
constexpr int HelperPollMs = 10;

// One scene check. Work runs on the application thread in steps of at most
// SceneCheckSliceMs plus the item in progress; cancel() ends the job at once
// with the rows gathered so far.
class SceneCheckJob : public Job,
                      public std::enable_shared_from_this<SceneCheckJob> {
public:
  SceneCheckJob(Context& context, Document& document, QString check,
                int deadlineMs)
      : m_context(context)
      , m_documentId(document.id)
      , m_check(std::move(check))
      , m_deadlineMs(deadlineMs) {
    JobRegistry& registry = jobRegistry(context);
    common.id             = registry.newJobId();
    common.kind           = "scene_check";
    common.lane           = JobLane::Analysis;
    common.documentId     = document.id;
    common.revision       = document.revisionText();
    common.submittedMs    = registry.elapsedMs();
    if (document.readOnly) {
      common.sourceFingerprint = document.readOnly->fingerprint;
      m_pins                   = document.readOnly->pins;
    }
  }

  JobCommon common;

  QString id() const override { return common.id; }
  JobLane lane() const override { return JobLane::Analysis; }
  QString documentId() const override { return common.documentId; }
  bool terminal() const override { return m_state != State::Running; }
  qint64 reservedBytes() const override { return 0; }

  void start() {
    common.startedMs = now();
    m_clock.start();
    schedule();
  }

  void cancel() override {
    if (!common.cancelRequested) {
      const qint64 asked     = m_interruptMs.load();
      common.cancelRequested = true;
      common.cancelMs        = asked >= 0 ? asked : now();
    }
    if (!terminal()) end(State::Canceled, "canceled");
  }

  void interrupt(qint64 requestedMs) override {
    qint64 unset = -1;
    m_interruptMs.compare_exchange_strong(unset, requestedMs);
    m_interrupted = true;
  }

  QJsonObject record() const override {
    const QJsonObject limits{{"deadline_ms", m_deadlineMs},
                             {"storage_reserved_bytes", 0},
                             {"cancel_grace_ms", CancelGraceMs}};
    const QJsonObject progress{{"unit", progressUnit()},
                               {"done", double(progressDone())},
                               {"total", double(progressTotal())}};
    const QJsonObject outputs{{"directory", QJsonValue()},
                              {"artifact_count", 0},
                              {"result_rows", m_rows.size()}};
    QJsonObject result = commonRecord(m_context, common, stateText(m_state),
                                      terminal(), limits, progress, outputs);
    result.insert("analysis", analysis());
    return result;
  }

  QJsonValue result(Context&, const QJsonObject& params) override {
    if (m_state == State::Running || m_state == State::Failed)
      throw Error(
          "result_unavailable",
          "Scene check rows are readable once the job completed, was "
          "canceled or reached its deadline.",
          {{"status", stateText(m_state)}, {"failure", common.failure}});
    const int offset = integerParam(params, "offset", 0, MaxRows, 0);
    const int limit  = integerParam(params, "limit", 1, MaxPage, DefaultPage);
    const int end    = std::min(offset + limit, int(m_rows.size()));
    QJsonArray rows;
    for (int index = offset; index < end; ++index)
      rows.append(m_rows.at(index));
    const QJsonObject page{
        {"job_id", common.id},
        {"kind", common.kind},
        {"check", m_check},
        {"status", stateText(m_state)},
        {"complete", m_reasons.isEmpty()},
        {"reasons", QJsonArray::fromStringList(m_reasons)},
        {"summary", summary()},
        {"rows", rows},
        {"total_rows", m_rows.size()},
        {"offset", offset},
        {"next_offset",
         end < m_rows.size() ? QJsonValue(end) : QJsonValue(QJsonValue::Null)},
        {"source_revision", common.revision}};
    if (QJsonDocument(page).toJson(QJsonDocument::Compact).size() >
        MaxResultBytes)
      throw Error("response_too_large",
                  "The result page exceeds 1 MiB; request fewer rows.");
    return page;
  }

#ifdef BUILD_UI_PORT_TESTS
  int steps() const { return m_steps; }
  int longestStepItems() const { return m_longestStepItems; }
  static int& testItemDelayMs() {
    static int delay = 0;
    return delay;
  }

  // Test hook: a slow scan, from a domain test or the slow-scan child fault.
  void testDelay() const {
    constexpr int slowScanMs = 25;
    const int delay = testItemDelayMs() > 0                 ? testItemDelayMs()
                      : m_context.childFault == "slow-scan" ? slowScanMs
                                                            : 0;
    if (delay > 0) QThread::msleep(delay);
  }
#endif

protected:
  // One unit of work: a drawing or a cast level. False when none is left.
  virtual bool work(Document& document) = 0;
  virtual QJsonObject summary() const   = 0;
  virtual QString progressUnit() const  = 0;
  virtual qint64 progressDone() const   = 0;
  virtual qint64 progressTotal() const  = 0;
  virtual QJsonObject estimate() const  = 0;
  virtual QJsonObject budget() const    = 0;
  // Drop pointers into the document once the job ended.
  virtual void release() = 0;
  // Whether the job waits on a process: its slice ends and the next one
  // starts after HelperPollMs instead of at once.
  virtual bool waiting() const { return false; }

  // Keep a row; false (and row_limit) once the job holds MaxRows rows.
  bool append(const QJsonObject& row) {
    if (m_rows.size() >= MaxRows) {
      incomplete("row_limit");
      return false;
    }
    m_rows.append(row);
    return true;
  }

  void incomplete(const QString& reason) {
    if (!m_reasons.contains(reason)) m_reasons.append(reason);
  }

  qint64 now() const { return jobRegistry(m_context).elapsedMs(); }

  Context& m_context;
  Cost m_cost;

private:
  QJsonObject analysis() const {
    const QJsonObject cost{{"elapsed_ms", double(elapsedMs())},
                           {"drawings_done", double(m_cost.drawings)},
                           {"strokes_scanned", double(m_cost.strokes)},
                           {"regions_built", double(m_cost.regions)},
                           {"pixels_scanned", double(m_cost.pixels)},
                           {"files_checked", double(m_cost.files)},
                           {"bytes_read", double(m_cost.bytes)},
                           {"helpers_started", double(m_cost.helpers)}};
    return {{"check", m_check},
            {"estimate", estimate()},
            {"cost", cost},
            {"budget", budget()},
            {"complete", terminal() && m_state != State::Failed
                             ? QJsonValue(m_reasons.isEmpty())
                             : QJsonValue(QJsonValue::Null)},
            {"reasons", QJsonArray::fromStringList(m_reasons)}};
  }

  qint64 elapsedMs() const {
    if (common.startedMs < 0) return 0;
    return (common.endedMs >= 0 ? common.endedMs : now()) - common.startedMs;
  }

  void schedule() {
    const auto self = shared_from_this();
    QTimer::singleShot(waiting() ? HelperPollMs : 0,
                       QCoreApplication::instance(),
                       [self]() { self->step(); });
  }

  // The document as it was at submission, or null after failing the job.
  Document* bound() {
    Document* document = nullptr;
    try {
      document = &m_context.document(m_documentId);
    } catch (const Error&) {
      fail("revision_changed", "The document was closed during the check.");
      return nullptr;
    }
    if (document->revisionText() == common.revision) return document;
    fail("revision_changed", "The document changed during the check.");
    return nullptr;
  }

  // Each slice is a posted event, so the event loop does not go idle while
  // the job runs; publishing here keeps polls answered elsewhere current.
  void step() {
    runSlice();
    jobRegistry(m_context).publish();
  }

  void runSlice() {
    if (terminal() || m_interrupted) return;
    Document* document = bound();
    if (!document) return;
    if (m_clock.elapsed() > m_deadlineMs)
      return end(State::DeadlineExceeded, "deadline");
    QElapsedTimer slice;
    slice.start();
    bool more = true;
    int items = 0;
    try {
      while (more && (items == 0 || slice.elapsed() < SceneCheckSliceMs) &&
             !(items > 0 && waiting())) {
        more = work(*document);
        ++items;
#ifdef BUILD_UI_PORT_TESTS
        testDelay();
#endif
      }
    } catch (const Error& error) {
      return fail("native_error", error.message);
    } catch (const TException& error) {
      return fail("native_error", QString::fromStdWString(error.getMessage()));
    } catch (const std::exception& error) {
      return fail("native_error", QString::fromUtf8(error.what()));
    }
    ++m_steps;
    m_longestStepItems = std::max(m_longestStepItems, items);
    if (more) return schedule();
    finish();
  }

  void finish() {
    const QStringList changed = changedSourcePins(m_pins);
    if (!changed.isEmpty())
      return fail("source_changed",
                  QString("%1 pinned source file(s) changed during the check, "
                          "including %2")
                      .arg(changed.size())
                      .arg(changed.first()));
    end(State::Completed, QString());
  }

  void fail(const QString& code, const QString& message) {
    common.failure = QJsonObject{
        {"code", code}, {"phase", "analyze"}, {"message", message.left(2048)}};
    end(State::Failed, QString());
  }

  void end(State state, const QString& reason) {
    if (!reason.isEmpty()) incomplete(reason);
    m_state        = state;
    common.endedMs = now();
    release();
  }

  QString m_documentId;
  QString m_check;
  int m_deadlineMs;
  std::vector<SourcePin> m_pins;
  QElapsedTimer m_clock;
  State m_state = State::Running;
  QJsonArray m_rows;
  QStringList m_reasons;
  std::atomic<qint64> m_interruptMs{-1};
  std::atomic<bool> m_interrupted{false};
  int m_steps            = 0;
  int m_longestStepItems = 0;
};

// One palette and the drawings of the used vector and Toonz raster levels
// painted with it, as SceneChecker::checkPalettes groups them.
struct PaletteScan final {
  TPalette* palette = nullptr;
  QJsonObject owner;
  std::vector<std::pair<TXshSimpleLevel*, TFrameId>> drawings;
  std::set<int> used;
  int scanned = 0;
  int total   = 0;
  int unused  = 0;
};

// Only vector and Toonz raster drawings carry palette style ids.
bool styledLevel(TXshSimpleLevel* level) {
  return level->getType() == PLI_XSHLEVEL || level->getType() == TZP_XSHLEVEL;
}

// A saved level whose file is gone has no drawings to scan.
bool levelFilePresent(ToonzScene& scene, TXshSimpleLevel* level) {
  const TFilePath path = level->getPath();
  return path.isEmpty() || level->getProperties()->getDirtyFlag() ||
         TSystem::doesExistFileOrLevel(scene.decodeFilePath(path));
}

class UnusedStylesJob final : public SceneCheckJob {
public:
  UnusedStylesJob(Context& context, Document& document, int deadlineMs,
                  const QSet<TXshLevel*>& selected)
      : SceneCheckJob(context, document, "unused_styles", deadlineMs) {
    plan(document, selected);
  }

protected:
  bool work(Document&) override {
    if (m_palette >= m_scans.size()) return false;
    PaletteScan& scan = m_scans[m_palette];
    if (scan.scanned < int(scan.drawings.size())) {
      if (!scanDrawing(scan, scan.drawings[scan.scanned])) return false;
      ++scan.scanned;
      return true;
    }
    const bool kept = unusedRows(scan);
    ++m_palette;
    return kept && m_palette < m_scans.size();
  }

  QJsonObject summary() const override {
    QJsonArray palettes;
    for (const PaletteScan& scan : m_summaries)
      palettes.append(paletteSummary(scan));
    return {{"palettes", palettes}};
  }

  QString progressUnit() const override { return "drawings"; }
  qint64 progressDone() const override { return m_cost.drawings; }
  qint64 progressTotal() const override { return m_drawings; }

  QJsonObject estimate() const override {
    return {{"drawings", double(m_drawings)},
            {"strokes", 0},
            {"control_points", 0},
            {"pixels", 0},
            {"files", 0},
            {"bytes", 0}};
  }

  QJsonObject budget() const override {
    return {{"max_drawings", double(MaxDrawings)},
            {"max_strokes", 0},
            {"max_pixels", double(MaxPixels)},
            {"max_files", UnusedFileBudget},
            {"max_bytes", double(UnusedByteBudget)}};
  }

  void release() override {
    for (std::size_t index = m_palette; index < m_scans.size(); ++index)
      m_summaries.push_back(m_scans[index]);
    for (PaletteScan& scan : m_summaries) {
      scan.palette = nullptr;
      scan.drawings.clear();
    }
    m_scans.clear();
  }

private:
  void plan(Document& document, const QSet<TXshLevel*>& selected) {
    ToonzScene& scene = *document.scene;
    std::set<TXshLevel*> used;
    scene.getTopXsheet()->getUsedLevels(used);
    std::map<TPalette*, std::size_t> index;
    const TLevelSet* cast = scene.getLevelSet();
    for (int i = 0; i < cast->getLevelCount(); ++i) {
      TXshLevel* level  = cast->getLevel(i);
      TPalette* palette = level ? paletteOf(level) : nullptr;
      if (!palette || (!selected.isEmpty() && !selected.contains(level)))
        continue;
      if (!index.count(palette)) {
        index[palette] = m_scans.size();
        PaletteScan scan;
        scan.palette = palette;
        scan.total   = styleCount(palette);
        scan.owner   = QJsonObject{
            {"kind", level->getPaletteLevel() ? "palette_level" : "level"},
            {"id", document.idForLevel(level)}};
        m_scans.push_back(scan);
      }
      addDrawings(scene, used, level, m_scans[index[palette]]);
    }
    // As the native rule, a palette no used level is drawn with is skipped.
    m_scans.erase(std::remove_if(m_scans.begin(), m_scans.end(),
                                 [](const PaletteScan& scan) {
                                   return scan.drawings.empty();
                                 }),
                  m_scans.end());
  }

  // The palette's styles on its pages, style 0 ("none") left out.
  static int styleCount(TPalette* palette) {
    int count = 0;
    for (int page = 0; page < palette->getPageCount(); ++page) {
      TPalette::Page* styles = palette->getPage(page);
      for (int position = 0; position < styles->getStyleCount(); ++position)
        if (styles->getStyleId(position) > 0) ++count;
    }
    return count;
  }

  static TPalette* paletteOf(TXshLevel* level) {
    if (TXshSimpleLevel* simple = level->getSimpleLevel())
      return simple->getPalette();
    if (TXshPaletteLevel* palette = level->getPaletteLevel())
      return palette->getPalette();
    return nullptr;
  }

  void addDrawings(ToonzScene& scene, const std::set<TXshLevel*>& used,
                   TXshLevel* level, PaletteScan& scan) {
    TXshSimpleLevel* simple = level->getSimpleLevel();
    if (!simple || !used.count(level) || !styledLevel(simple)) return;
    if (!levelFilePresent(scene, simple)) {
      incomplete("unreadable");
      return;
    }
    for (const TFrameId& fid : simple->getFids()) {
      if (m_drawings >= MaxDrawings) {
        incomplete("drawing_budget");
        return;
      }
      scan.drawings.push_back({simple, fid});
      ++m_drawings;
    }
  }

  // Ink and paint of a Toonz raster drawing, strokes and regions of a vector
  // one; regions are built on a clone, never on the document's image.
  bool scanDrawing(PaletteScan& scan,
                   const std::pair<TXshSimpleLevel*, TFrameId>& drawing) {
    const TImageP image = drawing.first->getFrame(drawing.second, false);
    if (TVectorImageP vector = image) {
      scanVector(scan, vector);
    } else if (TToonzImageP raster = image) {
      if (!scanRaster(scan, raster)) return false;
    } else {
      incomplete("unreadable");
    }
    ++m_cost.drawings;
    return true;
  }

  void scanVector(PaletteScan& scan, const TVectorImageP& document) {
    // findRegions() computes regions only when the clone's are not valid.
    TVectorImageP clone = document->clone();
    clone->findRegions();
    m_cost.regions += clone->getRegionCount();
    for (UINT i = 0; i < clone->getStrokeCount(); ++i)
      if (TStroke* stroke = clone->getStroke(i))
        scan.used.insert(stroke->getStyle());
    for (UINT i = 0; i < clone->getRegionCount(); ++i)
      if (TRegion* region = clone->getRegion(i))
        scan.used.insert(region->getStyle());
    m_cost.strokes += clone->getStrokeCount();
  }

  bool scanRaster(PaletteScan& scan, const TToonzImageP& image) {
    TRasterCM32P raster = image->getRaster();
    if (!raster) {
      incomplete("unreadable");
      return true;
    }
    const qint64 pixels = qint64(raster->getLx()) * raster->getLy();
    if (m_cost.pixels + pixels > MaxPixels) {
      incomplete("pixel_budget");
      return false;
    }
    raster->lock();
    for (int y = 0; y < raster->getLy(); ++y) {
      const TPixelCM32* pixel = raster->pixels(y);
      const TPixelCM32* end   = pixel + raster->getLx();
      for (; pixel < end; ++pixel) {
        scan.used.insert(pixel->getInk());
        scan.used.insert(pixel->getPaint());
      }
    }
    raster->unlock();
    m_cost.pixels += pixels;
    return true;
  }

  // The palette's styles no scanned drawing uses; as Erase Unused Styles, the
  // first two styles of the first page and style 0 are never reported.
  bool unusedRows(PaletteScan& scan) {
    m_summaries.push_back(scan);
    PaletteScan& summary = m_summaries.back();
    TPalette* palette    = scan.palette;
    for (int page = 0; page < palette->getPageCount(); ++page) {
      TPalette::Page* styles = palette->getPage(page);
      for (int position = 0; position < styles->getStyleCount(); ++position) {
        const int id = styles->getStyleId(position);
        if ((page == 0 && position < 2) || id <= 0) continue;
        if (scan.used.count(id)) continue;
        ++summary.unused;
        TColorStyle* style = palette->getStyle(id);
        const QJsonObject row{
            {"row_kind", "unused_style"},
            {"palette_owner", scan.owner},
            {"style_id", id},
            {"page_index", page},
            {"position", position},
            {"name", style ? QString::fromStdWString(style->getName()).left(256)
                           : QString()}};
        if (!append(row)) return false;
      }
    }
    return true;
  }

  QJsonObject paletteSummary(const PaletteScan& scan) const {
    return {{"palette_owner", scan.owner},
            {"styles_total", scan.total},
            {"styles_used", scan.total - scan.unused},
            {"styles_unused", scan.unused},
            {"drawings_scanned", scan.scanned}};
  }

  std::vector<PaletteScan> m_scans;
  std::vector<PaletteScan> m_summaries;
  std::size_t m_palette = 0;
  qint64 m_drawings     = 0;
};

// The traversal reasons of the dependency walk and the sheet walk as analysis
// reasons: the sheet walk's depth_limit and cycle keep their names.
QString analysisReason(const QString& reason) {
  static const QStringList limits{"record_limit", "level_limit", "fx_limit",
                                  "column_limit", "row_limit"};
  if (reason == "depth_limit" || reason == "cycle") return reason;
  return limits.contains(reason) ? "row_limit" : "unsupported_level";
}

class InventoryJob final : public SceneCheckJob {
public:
  InventoryJob(Context& context, Document& document, int deadlineMs)
      : SceneCheckJob(context, document, "inventory", deadlineMs)
      , m_walk(std::make_unique<DependencyWalk>(context, document, MaxRows)) {
    QStringList reasons;
    const QHash<TXshLevel*, QJsonArray> paths =
        levelSheetPaths(document, reasons);
    for (auto it = paths.constBegin(); it != paths.constEnd(); ++it)
      m_paths.insert(document.idForLevel(it.key()), it.value());
    for (const QString& reason : reasons) incomplete(analysisReason(reason));
    m_levels = m_walk->levels();
  }

protected:
  bool work(Document&) override {
    const bool more         = m_walk->step();
    const QJsonArray& items = m_walk->rows();
    for (; m_taken < items.size(); ++m_taken) {
      const QJsonObject item = items.at(m_taken).toObject();
      const QJsonArray path =
          item.value("owner_kind") == "level"
              ? m_paths.value(item.value("owner_id").toString())
              : QJsonArray();
      if (!append({{"row_kind", "dependency"},
                   {"sheet_path", path},
                   {"item", item}}))
        return false;
    }
    m_levelsDone = m_walk->levelsDone();
    if (!more)
      for (const QString& reason : m_walk->reasons())
        incomplete(analysisReason(reason));
    return more;
  }

  QJsonObject summary() const override {
    return {{"levels", m_levels},
            {"levels_done", m_levelsDone},
            {"dependencies", m_taken}};
  }

  QString progressUnit() const override { return "records"; }
  qint64 progressDone() const override { return m_levelsDone; }
  qint64 progressTotal() const override { return m_levels; }

  QJsonObject estimate() const override {
    return {{"drawings", 0}, {"strokes", 0}, {"control_points", 0},
            {"pixels", 0},   {"files", 0},   {"bytes", 0}};
  }

  QJsonObject budget() const override {
    return {{"max_drawings", 1},
            {"max_strokes", 0},
            {"max_pixels", 0},
            {"max_files", UnusedFileBudget},
            {"max_bytes", double(UnusedByteBudget)}};
  }

  void release() override { m_walk.reset(); }

private:
  std::unique_ptr<DependencyWalk> m_walk;
  QHash<QString, QJsonArray> m_paths;
  int m_taken      = 0;
  int m_levels     = 0;
  int m_levelsDone = 0;
};

// The native compatibility report as a job: the rules that load no drawing in
// the first step, then each vector drawing in its own unit of work.
class CompatibilityJob final : public SceneCheckJob {
public:
  CompatibilityJob(Context& context, Document& document, int deadlineMs)
      : SceneCheckJob(context, document, "compatibility", deadlineMs) {
    QStringList reasons;
    m_sheets = sheetPaths(document, reasons);
    for (const QString& reason : reasons) incomplete(analysisReason(reason));
    for (TXshLevel* entry : distinctLevels(document.scene->getLevelSet())) {
      TXshSimpleLevel* level = entry ? entry->getSimpleLevel() : nullptr;
      if (!level || level->getType() != PLI_XSHLEVEL) continue;
      m_levels.push_back({level, level->getFids()});
      m_drawings += level->getFrameCount();
    }
  }

protected:
  bool work(Document& document) override {
    if (!m_sceneDone) {
      m_sceneDone = true;
      return report(document, SceneChecker::sceneCompatibilityReport(
                                  document.scene.get())) &&
             !m_levels.empty();
    }
    VectorLevel& level = m_levels[m_level];
    if (m_drawing < level.fids.size()) {
      const int profiled = SceneChecker::opacityEnvelopeDrawing(
          level.level, level.fids[m_drawing++]);
      if (profiled > 0) ++level.profiled;
      if (profiled < 0) level.incomplete = true;
      ++m_cost.drawings;
      return true;
    }
    const bool kept =
        report(document, SceneChecker::levelDrawingCompatibility(
                             level.level, level.profiled, level.incomplete));
    m_drawing = 0;
    return kept && ++m_level < m_levels.size();
  }

  QJsonObject summary() const override {
    return {
        {"findings", double(m_findings)},
        {"by_certainty", QJsonObject{{"confirmed_loss", m_byCertainty[0]},
                                     {"refusal_risk", m_byCertainty[1]},
                                     {"unknown_support", m_byCertainty[2]}}},
        {"drawings_scanned", double(m_cost.drawings)},
        {"reader", "legacy_opentoonz"}};
  }

  QString progressUnit() const override { return "drawings"; }
  qint64 progressDone() const override { return m_cost.drawings; }
  qint64 progressTotal() const override { return m_drawings; }

  QJsonObject estimate() const override {
    return {{"drawings", double(m_drawings)},
            {"strokes", 0},
            {"control_points", 0},
            {"pixels", 0},
            {"files", 0},
            {"bytes", 0}};
  }

  QJsonObject budget() const override {
    return {{"max_drawings", double(MaxDrawings)},
            {"max_strokes", 0},
            {"max_pixels", 0},
            {"max_files", UnusedFileBudget},
            {"max_bytes", double(UnusedByteBudget)}};
  }

  void release() override {
    m_levels.clear();
    m_sheets.clear();
  }

private:
  struct VectorLevel final {
    TXshSimpleLevel* level;
    std::vector<TFrameId> fids;
    int profiled    = 0;
    bool incomplete = false;
  };

  bool report(Document& document,
              const std::vector<SceneChecker::Issue>& issues) {
    for (const SceneChecker::Issue& issue : issues) {
      ++m_findings;
      ++m_byCertainty[issue.certainty];
      if (!append(row(document, issue))) return false;
    }
    return true;
  }

  // As scene.validate rows: info for unknown support, a warning otherwise.
  QJsonObject row(Document& document, const SceneChecker::Issue& issue) const {
    TXshLevel* level =
        issue.levelName.empty()
            ? nullptr
            : document.scene->getLevelSet()->getLevel(issue.levelName);
    const bool unknown = issue.certainty == SceneChecker::UnknownSupport;
    return {{"row_kind", "compatibility"},
            {"code", QString::fromStdString(issue.code)},
            {"certainty", SceneChecker::certaintyName(issue.certainty)},
            {"reader", "legacy_opentoonz"},
            {"severity", unknown ? "info" : "warning"},
            {"message", issue.text.left(2048)},
            {"sheet_path", m_sheets.value(issue.xsheet)},
            {"sheet_name", QString::fromStdWString(issue.subXsheet)},
            {"level_id", level ? QJsonValue(document.idForLevel(level))
                               : QJsonValue(QJsonValue::Null)},
            {"column_index", issue.column >= 0 ? QJsonValue(issue.column)
                                               : QJsonValue(QJsonValue::Null)},
            {"frame", issue.row >= 0 ? QJsonValue(issue.row)
                                     : QJsonValue(QJsonValue::Null)}};
  }

  QHash<TXsheet*, QJsonArray> m_sheets;
  std::vector<VectorLevel> m_levels;
  std::size_t m_level   = 0;
  std::size_t m_drawing = 0;
  qint64 m_drawings     = 0;
  bool m_sceneDone      = false;
  int m_findings        = 0;
  int m_byCertainty[3]  = {0, 0, 0};
};

// One dependency file media_decode looks at, and what is known before decoding.
struct MediaFile final {
  QJsonObject owner;
  QString role;
  QString path;
  QString format;
  QString result;  // set when the file is not decoded
  QString message;
  qint64 size = -1;
};

QString mediaFormat(const QString& path) {
  return QFileInfo(path).suffix().toLower().left(16);
}

// A dependency availability that already decides the result.
QString availabilityResult(const QString& availability) {
  if (availability == "missing" || availability == "unresolved")
    return "missing";
  if (availability == "forbidden") return "outside_roots";
  if (availability == "unreadable") return "unreadable";
  if (availability == "unsupported") return "unsupported_format";
  return {};
}

QJsonObject mediaOwner(const QJsonObject& item) {
  const QString role = item.value("role").toString();
  if (role == "raster_drawing_palette")
    return {{"kind", "palette"}, {"id", item.value("owner_id")}};
  return {{"kind", role == "sound" ? "sound" : "level"},
          {"id", item.value("owner_id")}};
}

QString mediaRole(const QString& role) {
  return role == "raster_drawing_palette" ? QString("palette") : role;
}

// media_decode of a read-only document: the worker reads the dependency list
// from the scene host, copies each image and sound file it may read, and has
// parse helpers decode the copies, so a decoder crash costs one helper.
class MediaDecodeJob final : public SceneCheckJob {
public:
  MediaDecodeJob(Context& context, Document& document, int deadlineMs,
                 int maxFiles, qint64 maxBytes)
      : SceneCheckJob(context, document, "media_decode", deadlineMs)
      , m_revision(document.revisionText())
      , m_maxFiles(maxFiles)
      , m_maxBytes(maxBytes) {}

protected:
  bool work(Document& document) override {
    if (m_listing) return listPage(document);
    if (m_expand < m_patterns.size()) return expand(m_patterns[m_expand++]);
    if (m_helper) return pollHelper();
    if (!m_retry.empty()) return startRetry();
    if (m_copy) return copyChunk();
    if (m_next < m_files.size()) return nextFile();
    if (!m_batch.empty()) return startHelper();
    return false;
  }

  bool waiting() const override { return m_helper != nullptr; }

  QJsonObject summary() const override {
    QJsonObject results;
    for (auto it = m_results.constBegin(); it != m_results.constEnd(); ++it)
      results.insert(it.key(), it.value());
    return {{"files", int(m_files.size())},
            {"files_reported", int(m_reported)},
            {"results", results}};
  }

  QString progressUnit() const override { return "files"; }
  qint64 progressDone() const override { return m_reported; }
  qint64 progressTotal() const override { return qint64(m_files.size()); }

  QJsonObject estimate() const override {
    qint64 bytes = 0;
    for (const MediaFile& file : m_files)
      bytes += std::max<qint64>(file.size, 0);
    return {{"drawings", 0},
            {"strokes", 0},
            {"control_points", 0},
            {"pixels", 0},
            {"files", double(m_files.size())},
            {"bytes", double(bytes)}};
  }

  QJsonObject budget() const override {
    // The job record's budget has no zero drawing bound; this check reads
    // no drawing of the document.
    return {{"max_drawings", 1},
            {"max_strokes", 0},
            {"max_pixels", 0},
            {"max_files", m_maxFiles},
            {"max_bytes", double(m_maxBytes)}};
  }

  void release() override {
    if (m_helper) m_helper->kill();
    m_helper.reset();
    m_copy.reset();
    m_batch.clear();
    m_retry.clear();
    m_directory.reset();
  }

private:
  struct Copy final {
    std::size_t index;
    QFile source;
    QFile target;
    QDateTime modified;
  };

  struct Batched final {
    std::size_t index;
    QString file;
  };

  bool listPage(Document& document) {
    const QJsonObject page =
        readFromSceneHost(m_context, document.id, "scene.dependencies",
                          {{"document_id", document.id},
                           {"expected_revision", m_revision},
                           {"offset", m_offset},
                           {"limit", 128}})
            .toObject();
    for (const QJsonValue& item : page.value("items").toArray())
      addDependency(item.toObject());
    if (page.value("next_offset").isDouble()) {
      m_offset = page.value("next_offset").toInt();
      return true;
    }
    if (!page.value("traversal_complete").toBool(true)) incomplete("row_limit");
    m_listing = false;
    return true;
  }

  void addDependency(const QJsonObject& item) {
    static const QStringList roles{"primary", "palette",
                                   "scan",    "reference_image",
                                   "sound",   "raster_drawing_palette"};
    const QString role = item.value("role").toString();
    if (!roles.contains(role)) return;
    MediaFile file{mediaOwner(item),
                   mediaRole(role),
                   item.value("resolved_path")
                       .toString(item.value("coded_path").toString()),
                   QString(),
                   QString(),
                   QString(),
                   -1};
    file.format = mediaFormat(file.path);
    file.result = availabilityResult(item.value("availability").toString());
    if (item.value("availability") == "unsaved") return;
    if (item.value("path_kind") == "sequence_pattern" &&
        file.result.isEmpty()) {
      m_patterns.push_back(file);
      return;
    }
    if (item.value("availability") == "present")
      file.size = qint64(item.value("size_bytes").toDouble(-1));
    addFile(file);
  }

  void addFile(MediaFile file) {
    if (file.path.isEmpty()) return;
    if (file.result.isEmpty() && !mediaDecodeFormats().contains(file.format))
      file.result = "unsupported_format";
    if (m_files.size() >= std::size_t(MaxRows)) {
      incomplete("row_limit");
      return;
    }
    m_files.push_back(file);
  }

  // Each frame file of a sequence, found by its level name in its folder.
  bool expand(const MediaFile& pattern) {
    const TFilePath level(pattern.path.toStdWString());
    const QDir folder(QFileInfo(pattern.path).path());
    const QStringList names =
        folder.entryList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
    for (const QString& name : names) {
      const QString path = folder.filePath(name);
      if (TFilePath(path.toStdWString()).getLevelNameW() !=
          level.getLevelNameW())
        continue;
      MediaFile frame = pattern;
      frame.role      = "frame";
      frame.path      = path;
      frame.size      = QFileInfo(path).size();
      addFile(frame);
    }
    return true;
  }

  void report(std::size_t index, const QJsonObject& decoded) {
    MediaFile& file      = m_files[index];
    const QString result = decoded.value("result").toString(file.result);
    m_results[result]    = m_results.value(result) + 1;
    ++m_reported;
    const QJsonValue size =
        file.size >= 0 ? QJsonValue(double(file.size)) : QJsonValue();
    append({{"row_kind", "media_file"},
            {"owner", file.owner},
            {"role", file.role},
            {"path", file.path.left(4096)},
            {"format", file.format},
            {"size_bytes", size},
            {"result", result},
            {"frames_checked", decoded.value("frames_checked").toInt(0)},
            {"width", decoded.value("width")},
            {"height", decoded.value("height")},
            {"message",
             decoded.value("message").toString(file.message).left(512)}});
  }

  void skip(std::size_t index, const QString& result, const QString& reason,
            const QString& message = QString()) {
    if (!reason.isEmpty()) incomplete(reason);
    m_files[index].message = message;
    report(index, {{"result", result},
                   {"width", QJsonValue()},
                   {"height", QJsonValue()}});
  }

  // Admit the next file to a batch, or report why it is not decoded.
  bool nextFile() {
    const std::size_t index = m_next++;
    MediaFile& file         = m_files[index];
    if (!file.result.isEmpty()) {
      skip(index, file.result, QString());
      return true;
    }
    if (m_admitted >= m_maxFiles) {
      skip(index, "skipped_budget", "file_budget");
      return true;
    }
    if (file.size > MediaDecodeFileLimitBytes) {
      skip(index, "too_large", QString(), "Larger than 256 MiB.");
      return true;
    }
    if (m_bytes + std::max<qint64>(file.size, 0) > m_maxBytes) {
      skip(index, "skipped_budget", "byte_budget");
      return true;
    }
    return openCopy(index);
  }

  bool openCopy(std::size_t index) {
    MediaFile& file   = m_files[index];
    auto copy         = std::make_unique<Copy>();
    copy->index       = index;
    OpenResult opened = OpenResult::Failed;
    try {
      m_context.resolvePath(file.path, false);
      opened = openApprovedFile(copy->source, file.path);
    } catch (const Error&) {
      skip(index, "outside_roots", QString());
      return true;
    }
    if (opened != OpenResult::Opened) {
      skip(index, "unreadable", QString(), "The file cannot be opened.");
      return true;
    }
    if (!ensureDirectory()) return false;
    file.size      = copy->source.size();
    copy->modified = QFileInfo(file.path).lastModified();
    copy->target.setFileName(m_directory->filePath(
        QString("file%1.%2").arg(m_batch.size()).arg(file.format)));
    if (!copy->target.open(QIODevice::WriteOnly))
      throw Error("io_error", "Cannot write the private media copy.");
    ++m_admitted;
    m_copy = std::move(copy);
    return true;
  }

  bool ensureDirectory() {
    if (m_directory) return true;
    if (m_context.runtimeDirectory.isEmpty())
      throw Error("io_error", "The worker has no private runtime directory.");
    m_directory = std::make_unique<QTemporaryDir>(
        QDir(m_context.runtimeDirectory).filePath("media-check-XXXXXX"));
    if (!m_directory->isValid())
      throw Error("io_error", "Cannot create the private media directory.");
    return true;
  }

  // Copy at most one chunk per unit of work; a finished copy whose source
  // changed meanwhile is unreadable, not decoded.
  bool copyChunk() {
    constexpr qint64 chunk = 8 * 1024 * 1024;
    const QByteArray bytes = m_copy->source.read(chunk);
    if (bytes.size() > 0) {
      m_copy->target.write(bytes);
      m_bytes += bytes.size();
      m_cost.bytes += bytes.size();
      if (!m_copy->source.atEnd()) return true;
    }
    const std::size_t index = m_copy->index;
    const QFileInfo after(m_files[index].path);
    const bool changed = after.size() != m_files[index].size ||
                         after.lastModified() != m_copy->modified ||
                         m_copy->target.size() != m_files[index].size;
    const QString file = QFileInfo(m_copy->target).fileName();
    m_copy->target.close();
    m_copy.reset();
    if (changed) {
      QFile::remove(m_directory->filePath(file));
      skip(index, "unreadable", QString(), "changed_during_check");
      return true;
    }
    m_batch.push_back({index, file});
    if (int(m_batch.size()) >= MediaFilesPerHelper) return startHelper();
    return true;
  }

  bool startHelper() {
    m_running = std::move(m_batch);
    m_batch.clear();
    QJsonArray manifest;
    for (std::size_t slot = 0; slot < m_running.size(); ++slot)
      manifest.append(
          QJsonObject{{"index", int(slot)},
                      {"file", m_running[slot].file},
                      {"format", m_files[m_running[slot].index].format}});
    QFile file(m_directory->filePath("manifest.json"));
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(manifest).toJson()) < 0)
      throw Error("io_error", "Cannot write the media helper manifest.");
    file.close();
    m_answered.assign(m_running.size(), false);
    m_helper = std::make_unique<ParseHelperRun>(m_context, m_directory->path(),
                                                "media");
    ++m_cost.helpers;
    return true;
  }

  bool pollHelper() {
    const bool running = m_helper->poll();
    for (const QByteArray& line : m_helper->takeLines()) {
      const QJsonObject reply = QJsonDocument::fromJson(line).object();
      const int slot          = reply.value("index").toInt(-1);
      if (slot < 0 || slot >= int(m_running.size()) || m_answered[slot])
        continue;
      m_answered[slot] = true;
      ++m_cost.files;
      report(m_running[slot].index, reply);
    }
    if (running) return true;
    helperEnded(m_helper->cause());
    return true;
  }

  // The files a failed helper did not answer: each is decoded again in its
  // own helper, so the file that fails is the one named.
  void helperEnded(const QString& cause) {
    m_helper.reset();
    std::vector<Batched> unanswered;
    for (std::size_t slot = 0; slot < m_running.size(); ++slot)
      if (!m_answered[slot]) unanswered.push_back(m_running[slot]);
    if (!unanswered.empty() && cause.isEmpty()) {
      for (const Batched& file : unanswered)
        skip(file.index, "crashed", QString(), "The helper gave no answer.");
    } else if (unanswered.size() == 1) {
      ++m_cost.files;
      skip(unanswered.front().index, cause, QString(),
           "The decoder " + cause + " on this file.");
    } else {
      for (const Batched& file : unanswered) m_retry.push_back(file);
    }
    m_running.clear();
    if (m_retry.empty()) m_directory.reset();
  }

  bool startRetry() {
    m_batch.push_back(m_retry.front());
    m_retry.erase(m_retry.begin());
    return startHelper();
  }

  QString m_revision;
  int m_maxFiles;
  qint64 m_maxBytes;
  bool m_listing = true;
  int m_offset   = 0;
  std::vector<MediaFile> m_patterns;
  std::size_t m_expand = 0;
  std::vector<MediaFile> m_files;
  std::size_t m_next = 0;
  qint64 m_reported  = 0;
  int m_admitted     = 0;
  qint64 m_bytes     = 0;
  QMap<QString, int> m_results;
  std::unique_ptr<QTemporaryDir> m_directory;
  std::unique_ptr<Copy> m_copy;
  std::vector<Batched> m_batch;
  std::vector<Batched> m_running;
  std::vector<Batched> m_retry;
  std::vector<bool> m_answered;
  std::unique_ptr<ParseHelperRun> m_helper;
};

QSet<TXshLevel*> selectedLevels(Document& document, const QJsonObject& params) {
  QSet<TXshLevel*> selected;
  for (const QJsonValue& id : params.value("level_ids").toArray())
    selected.insert(document.level(id.toString()));
  return selected;
}

qint64 mediaBytes(const QJsonObject& params) {
  const double bytes =
      params.value("max_bytes").toDouble(double(DefaultMediaBytes));
  if (bytes < 1 || bytes > double(MaxMediaBytes) || bytes != std::floor(bytes))
    throw Error("invalid_params",
                "max_bytes must be an integer from 1 to 17179869184.");
  return qint64(bytes);
}

// The request as admitted: the caller's arguments with every default filled.
QJsonObject admittedRequest(const QJsonObject& params, int deadlineMs) {
  QJsonObject request = params;
  request.remove("operation_id");
  request.remove("expected_revision");
  request.insert("deadline_ms", deadlineMs);
  if (request.value("check") == "media_decode") {
    if (!request.contains("max_files"))
      request.insert("max_files", DefaultMediaFiles);
    if (!request.contains("max_bytes"))
      request.insert("max_bytes", double(DefaultMediaBytes));
  }
  return request;
}

QJsonValue submit(Context& context, const QJsonObject& params) {
  Document& document = context.document(requiredString(params, "document_id"));
  if (requiredString(params, "expected_revision") != document.revisionText())
    throw Error(
        "revision_conflict",
        "The document changed; inspect it before starting a check.",
        {{"document_id", document.id}, {"revision", document.revisionText()}});
  const QString check = requiredString(params, "check");
  if (params.contains("level_ids") && check != "unused_styles")
    throw Error("invalid_params", "level_ids applies to unused_styles only.");
  if ((params.contains("max_files") || params.contains("max_bytes")) &&
      check != "media_decode")
    throw Error("invalid_params",
                "max_files and max_bytes apply to media_decode only.");
  if (check == "media_decode" && !document.readOnly)
    throw Error("invalid_params",
                "media_decode checks the files of a read-only document; an "
                "editable document's drawings are already decoded and its "
                "files are written and verified by the worker.",
                {{"reason", "editable_document"}});
  const int deadlineMs =
      integerParam(params, "deadline_ms", 1000, 600000, DefaultDeadlineMs);
  requireAnalysisCapacity(context);
  std::shared_ptr<SceneCheckJob> job;
  if (check == "unused_styles")
    job = std::make_shared<UnusedStylesJob>(context, document, deadlineMs,
                                            selectedLevels(document, params));
  else if (check == "compatibility")
    job = std::make_shared<CompatibilityJob>(context, document, deadlineMs);
  else if (check == "media_decode")
    job = std::make_shared<MediaDecodeJob>(
        context, document, deadlineMs,
        integerParam(params, "max_files", 1, MaxMediaFiles, DefaultMediaFiles),
        mediaBytes(params));
  else
    job = std::make_shared<InventoryJob>(context, document, deadlineMs);
  job->common.request = admittedRequest(params, deadlineMs);
  context.markMutationStarted();
  jobRegistry(context).add(job);
  job->start();
  return job->record();
}

}  // namespace

QJsonArray sceneCheckOperations() {
  const QJsonObject handle{
      {"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
  QJsonObject descriptor = operation(
      "scene.check.submit",
      "Start one bounded, cancellable scene check as an analysis job: "
      "unused_styles (the palette styles no drawing of the used vector and "
      "Toonz raster levels uses; regions are built on detached clones) or "
      "inventory (every dependency row of scene.dependencies, each with the "
      "sheet_path of the first sheet that exposes it, without the read's "
      "record limit) or compatibility (what a legacy OpenToonz reader drops, "
      "may refuse or may not support, one row per finding with a stable "
      "code and certainty; it loads every vector drawing) or media_decode "
      "(read-only documents: every image, level and sound file the scene "
      "uses, frames of sequences included, copied privately and decoded in "
      "isolated helpers; one row per file with decoded, corrupt, "
      "unreadable, unsupported_format, too_large, missing, outside_roots, "
      "crashed, timeout, memory or skipped_budget). Poll job.status "
      "and read rows with job.result; a "
      "canceled or deadline-ended job keeps its rows, marked incomplete.",
      {{"document_id", handle},
       {"expected_revision",
        QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 80}}},
       {"check",
        QJsonObject{{"type", "string"},
                    {"enum", QJsonArray{"unused_styles", "inventory",
                                        "compatibility", "media_decode"}}}},
       {"level_ids", QJsonObject{{"type", "array"},
                                 {"items", handle},
                                 {"minItems", 1},
                                 {"maxItems", MaxLevelIds}}},
       {"deadline_ms",
        QJsonObject{
            {"type", "integer"}, {"minimum", 1000}, {"maximum", 600000}}},
       {"max_files",
        QJsonObject{
            {"type", "integer"}, {"minimum", 1}, {"maximum", MaxMediaFiles}}},
       {"max_bytes", QJsonObject{{"type", "integer"},
                                 {"minimum", 1},
                                 {"maximum", double(MaxMediaBytes)}}}},
      {"document_id", "expected_revision", "check"}, true);
  descriptor.insert("mutates_document", false);
  // A check only reads the document; starting one changes no state a retry
  // or an undo would need to know about.
  QJsonObject annotations = descriptor.value("annotations").toObject();
  annotations.insert("destructiveHint", false);
  descriptor.insert("annotations", annotations);
  return {descriptor};
}

#ifdef BUILD_UI_PORT_TESTS
SceneCheckSteps sceneCheckSteps(Context& context, const QString& jobId) {
  const auto job = std::dynamic_pointer_cast<SceneCheckJob>(
      jobRegistry(context).find(jobId));
  if (!job) return {};
  return {job->steps(), job->longestStepItems()};
}

void setSceneCheckItemDelayMs(int milliseconds) {
  SceneCheckJob::testItemDelayMs() = milliseconds;
}
#endif

QJsonValue executeSceneCheck(Context& context, const QString& method,
                             const QJsonObject& params) {
  if (method != "scene.check.submit")
    throw Error("unknown_operation", "Unknown scene check operation.");
  return submit(context, params);
}

}  // namespace GroundzeroAutomation
