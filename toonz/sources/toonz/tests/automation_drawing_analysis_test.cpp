#include "automation_drawing_analysis.h"

#include "automation_jobs.h"
#include "automation_vector_opacity.h"

#include "tpalette.h"
#include "traster.h"
#include "tpixel.h"
#include "tregion.h"
#include "tstroke.h"
#include "ttoonzimage.h"
#include "tvectorimage.h"
#include "toonz/levelproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/txshsimplelevel.h"

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

Error refused(const std::function<void()>& action) {
  try {
    action();
  } catch (const Error& error) {
    return error;
  }
  throw std::runtime_error("An analysis request was accepted");
}

QString createLevel(Context& context, Document& document, const char* format,
                    const char* name = "Analysis") {
  return executeScene(
             context, "level.create",
             {{"document_id", document.id}, {"name", name}, {"format", format}})
      .toObject()
      .value("level_id")
      .toString();
}

TVectorImageP square(double x, double y, int style, int strokes) {
  TVectorImageP image = new TVectorImage();
  for (int index = 0; index < strokes; ++index) {
    const double left = x + index * 25;
    std::vector<TThickPoint> points{
        {left, y, 1},           {left + 20, y, 1},      {left + 40, y, 1},
        {left + 40, y + 20, 1}, {left + 40, y + 40, 1}, {left + 20, y + 40, 1},
        {left, y + 40, 1},      {left, y + 20, 1},      {left, y, 1}};
    auto* stroke = new TStroke(points);
    stroke->setSelfLoop(true);
    stroke->setStyle(style + index % 2);
    image->addStroke(stroke);
  }
  return image;
}

QJsonObject params(Document& document, const QString& level,
                   const QString& check) {
  return {{"document_id", document.id},
          {"expected_revision", document.revisionText()},
          {"operation_id", "analysis"},
          {"level_id", level},
          {"check", check}};
}

QJsonObject drawingId(int number) {
  return {{"number", number}, {"suffix", ""}};
}

QString jobOf(const QJsonObject& record) {
  return record.value("job_id").toString();
}

QJsonObject statusOf(Context& context, const QString& job) {
  return executeJobs(context, "job.status", {{"job_id", job}}).toObject();
}

int drain(Context& context, const QString& job) {
  int steps = 0;
  while (stepDrawingAnalysis(context, job)) ++steps;
  return steps;
}

QJsonObject resultOf(Context& context, const QString& job, int offset = 0,
                     int limit = 256) {
  return executeJobs(context, "job.result",
                     {{"job_id", job}, {"offset", offset}, {"limit", limit}})
      .toObject();
}

struct Fixture final {
  Document* document;
  QString level;
  TXshSimpleLevel* simple;
};

Fixture vectorLevel(Context& context, int drawings, int strokes) {
  auto& document      = context.addDocument(std::make_unique<ToonzScene>());
  const QString level = createLevel(context, document, "vector");
  auto* simple        = document.level(level)->getSimpleLevel();
  for (int number = 1; number <= drawings; ++number) {
    const TVectorImageP image = square(0, 0, 1, strokes + number - 1);
    image->setPalette(simple->getPalette());
    simple->setFrame(TFrameId(number), TImageP(image.getPointer()));
  }
  return {&document, level, simple};
}

void slicesAndRows(Context& context) {
  const auto fixture         = vectorLevel(context, 3, 6);
  Document& document         = *fixture.document;
  const TVectorImageP source = fixture.simple->getFrame(TFrameId(1), false);
  const auto digest          = vectorLevelOpacityDigest(*fixture.simple);
  const auto record          = startDrawingAnalysisUnscheduled(
      context, document, params(document, fixture.level, "style_usage"),
      AnalysisBudget());
  const QString job   = jobOf(record);
  const auto analysis = record.value("analysis").toObject();
  require(record.value("kind") == "drawing_analysis" &&
              record.value("lane") == "analysis" &&
              record.value("status") == "preparing",
          "A new analysis job has the wrong identity or state");
  const auto estimate = analysis.value("estimate").toObject();
  require(estimate.value("drawings").toInt() == 3 &&
              estimate.value("strokes").toInt() == 6 + 7 + 8 &&
              estimate.value("control_points").toInt() == (6 + 7 + 8) * 9 &&
              estimate.value("pixels").toInt() == 0,
          "The estimate does not match the drawings");
  require(analysis.value("complete").isNull(),
          "A new job reports completeness");
  require(stepDrawingAnalysis(context, job), "The first slice ended the job");
  auto running = statusOf(context, job);
  require(running.value("progress").toObject().value("done").toInt() == 1,
          "One slice did not analyse exactly one drawing");
  require(running.value("status") == "running", "The job is not running");
  require(!executeJobs(context, "job.list", {}).toObject().isEmpty(),
          "job.list failed");
  const auto early = refused([&]() { resultOf(context, job); });
  require(early.code == "result_unavailable", "A running job returned rows");
  const int more = drain(context, job);
  require(more == 2, "The job did not take one slice per drawing");
  const auto done = statusOf(context, job);
  require(
      done.value("status") == "completed" && done.value("terminal").toBool(),
      "The job did not complete");
  require(done.value("analysis").toObject().value("complete") == true &&
              done.value("analysis")
                  .toObject()
                  .value("reasons")
                  .toArray()
                  .isEmpty(),
          "A complete job reports reasons");
  const auto cost = done.value("analysis").toObject().value("cost").toObject();
  require(cost.value("drawings_done").toInt() == 3 &&
              cost.value("strokes_scanned").toInt() == 21 &&
              cost.value("regions_built").toInt() > 0,
          "The cost does not match the work");
  const auto page = resultOf(context, job, 0, 5);
  require(page.value("rows").toArray().size() == 5 &&
              page.value("next_offset").toInt() == 5,
          "The first row page is wrong");
  const auto last =
      resultOf(context, job, page.value("total_rows").toInt() - 1, 5);
  require(last.value("next_offset").isNull() &&
              last.value("rows").toArray().size() == 1,
          "The last row page has a cursor");
  const auto rows = resultOf(context, job).value("rows").toArray();
  int strokes     = 0;
  for (const auto& entry : rows) {
    const auto row = entry.toObject();
    if (row.value("row_kind") == "style_total")
      strokes += row.value("strokes").toInt();
  }
  require(strokes == 21, "The style totals miss strokes");
  require(source->getRegionCount() == 0,
          "The job built regions on the document");
  require(vectorLevelOpacityDigest(*fixture.simple) == digest,
          "The job changed the artwork");
  context.removeDocument(document.id);
}

void admissionAndRetention(Context& context) {
  const auto fixture = vectorLevel(context, 2, 3);
  Document& document = *fixture.document;
  const auto first   = startDrawingAnalysisUnscheduled(
      context, document, params(document, fixture.level, "roles"),
      AnalysisBudget());
  const auto busy = refused([&]() {
    startDrawingAnalysisUnscheduled(context, document,
                                    params(document, fixture.level, "roles"),
                                    AnalysisBudget());
  });
  require(busy.code == "analysis_busy" &&
              busy.details.value("active_job_id") == jobOf(first),
          "A second job was admitted while one runs");
  drain(context, jobOf(first));
  while (jobRegistry(context).retained(JobLane::Analysis) < JobsPerLane) {
    const auto next = startDrawingAnalysisUnscheduled(
        context, document, params(document, fixture.level, "roles"),
        AnalysisBudget());
    drain(context, jobOf(next));
  }
  const auto full = refused([&]() {
    startDrawingAnalysisUnscheduled(context, document,
                                    params(document, fixture.level, "roles"),
                                    AnalysisBudget());
  });
  require(full.code == "resource_limit" &&
              full.details.value("limit") == "retained_analysis_jobs",
          "The retained job limit was not enforced");
  context.removeDocument(document.id);
}

void refusalsAtSubmit(Context& context) {
  const auto fixture = vectorLevel(context, 3, 2);
  Document& document = *fixture.document;
  auto start         = [&](QJsonObject request, AnalysisBudget budget = {}) {
    startDrawingAnalysisUnscheduled(context, document, request, budget);
  };
  const int listed =
      executeJobs(context, "job.list", {}).toObject().value("total").toInt();
  auto base    = params(document, fixture.level, "regions");
  auto limited = base;
  limited.insert("max_drawings", 2);
  const auto over = refused([&]() { start(limited); });
  require(over.code == "resource_limit" &&
              over.details.value("estimate").toObject().value("drawings") == 3,
          "An over-budget estimate was admitted");
  AnalysisBudget tight;
  tight.strokes = 5;
  require(refused([&]() { start(base, tight); }).code == "resource_limit",
          "Strokes over budget were admitted");
  auto styles = base;
  styles.insert("style_ids", QJsonArray{1});
  require(refused([&]() { start(styles); }).code == "invalid_params",
          "style_ids was accepted for regions");
  auto missing = base;
  missing.insert("drawing_ids", QJsonArray{drawingId(9)});
  require(refused([&]() { start(missing); }).code == "not_found",
          "A missing drawing was accepted");
  auto duplicate = base;
  duplicate.insert("drawing_ids", QJsonArray{drawingId(1), drawingId(1)});
  require(refused([&]() { start(duplicate); }).code == "invalid_params",
          "Duplicate drawings were accepted");
  auto unknown = base;
  unknown.insert("check", "unused_styles");
  require(refused([&]() { start(unknown); }).code == "invalid_params",
          "An unknown check was accepted");
  const QString raster =
      createLevel(context, document, "toonzRaster", "Raster");
  require(refused([&]() { start(params(document, raster, "roles")); }).code ==
              "unsupported_type",
          "Roles were admitted on a raster level");
  require(
      executeJobs(context, "job.list", {}).toObject().value("total").toInt() ==
          listed,
      "A refused request created a job");
  context.removeDocument(document.id);
}

void revisionBinding(Context& context) {
  const auto fixture = vectorLevel(context, 3, 3);
  Document& document = *fixture.document;
  const auto job     = jobOf(startDrawingAnalysisUnscheduled(
      context, document, params(document, fixture.level, "regions"),
      AnalysisBudget()));
  require(stepDrawingAnalysis(context, job), "The first slice ended the job");
  ++document.revision;
  require(!stepDrawingAnalysis(context, job), "The job outlived a write");
  const auto ended   = statusOf(context, job);
  const auto failure = ended.value("failure").toObject();
  require(ended.value("status") == "failed" &&
              failure.value("code") == "revision_changed" &&
              failure.value("phase") == "analyze",
          "A write did not fail the job");
  require(ended.value("outputs").toObject().value("result_rows").toInt() == 0,
          "A failed job kept rows");
  require(
      refused([&]() { resultOf(context, job); }).code == "result_unavailable",
      "A failed job returned rows");
  context.removeDocument(document.id);
}

void cancelAndDeadline(Context& context) {
  const auto fixture = vectorLevel(context, 4, 3);
  Document& document = *fixture.document;
  const auto job     = jobOf(startDrawingAnalysisUnscheduled(
      context, document, params(document, fixture.level, "regions"),
      AnalysisBudget()));
  stepDrawingAnalysis(context, job);
  stepDrawingAnalysis(context, job);
  const auto asked =
      executeJobs(context, "job.cancel", {{"job_id", job}}).toObject();
  require(asked.value("status") == "cancellation_requested",
          "A cancelled running job is not canceling");
  require(!stepDrawingAnalysis(context, job), "A cancelled job kept running");
  const auto ended    = statusOf(context, job);
  const auto analysis = ended.value("analysis").toObject();
  require(ended.value("status") == "canceled" &&
              analysis.value("complete") == false &&
              analysis.value("reasons") == QJsonArray{"canceled"} &&
              ended.value("cancellation").toObject().value("effective") == true,
          "Cancellation was not recorded");
  require(resultOf(context, job).value("rows").toArray().size() == 2,
          "A cancelled job lost its rows");
  AnalysisBudget late;
  late.deadlineMs = -1;
  const auto slow = jobOf(startDrawingAnalysisUnscheduled(
      context, document, params(document, fixture.level, "regions"), late));
  require(!stepDrawingAnalysis(context, slow), "The deadline was ignored");
  const auto expired = statusOf(context, slow);
  require(expired.value("status") == "deadline_exceeded" &&
              expired.value("analysis").toObject().value("reasons") ==
                  QJsonArray{"deadline"},
          "The deadline was not recorded");
  context.removeDocument(document.id);
}

void rowLimit(Context& context) {
  const auto fixture = vectorLevel(context, 3, 4);
  Document& document = *fixture.document;
  AnalysisBudget small;
  small.rowsForTotals = 2;
  small.rows          = 2 + 4;
  const auto job      = jobOf(startDrawingAnalysisUnscheduled(
      context, document, params(document, fixture.level, "style_usage"),
      small));
  drain(context, job);
  const auto ended    = statusOf(context, job);
  const auto analysis = ended.value("analysis").toObject();
  require(ended.value("status") == "completed" &&
              analysis.value("complete") == false &&
              analysis.value("reasons") == QJsonArray{"row_limit"},
          "The row limit did not end the job");
  require(resultOf(context, job).value("rows").toArray().size() > 0,
          "The row limit dropped every row");
  context.removeDocument(document.id);
}

void rasterBatchesAndPixelBudget(Context& context) {
  auto& document      = context.addDocument(std::make_unique<ToonzScene>());
  const QString level = createLevel(context, document, "toonzRaster");
  auto* simple        = document.level(level)->getSimpleLevel();
  simple->getProperties()->setImageRes(TDimension(10, 10));
  TRasterCM32P raster(40, 25);
  raster->lock();
  for (int y = 0; y < 25; ++y)
    for (int x = 0; x < 40; ++x)
      raster->pixels(y)[x] = TPixelCM32(1 + x % 2, 2 + y % 3, 255);
  raster->unlock();
  TToonzImageP image(raster, TRect(0, 0, 39, 24));
  image->setPalette(simple->getPalette());
  simple->setFrame(TFrameId(1), image);
  AnalysisBudget batches;
  batches.pixelsPerSlice = 400;
  const auto job         = jobOf(startDrawingAnalysisUnscheduled(
      context, document, params(document, level, "style_usage"), batches));
  require(drain(context, job) == 3, "Raster rows were not scanned in batches");
  const auto rows = resultOf(context, job).value("rows").toArray();
  qint64 ink = 0, paint = 0;
  for (const auto& entry : rows) {
    const auto row = entry.toObject();
    if (row.value("row_kind") != "style_total") continue;
    ink += row.value("ink_pixels").toInt();
    paint += row.value("paint_pixels").toInt();
  }
  require(ink == 1000 && paint == 1000, "Raster style totals miss pixels");
  AnalysisBudget tight;
  tight.pixels       = 500;
  const auto limited = jobOf(startDrawingAnalysisUnscheduled(
      context, document, params(document, level, "style_usage"), tight));
  drain(context, limited);
  const auto ended = statusOf(context, limited);
  require(ended.value("analysis").toObject().value("reasons") ==
                  QJsonArray{"pixel_budget"} &&
              ended.value("status") == "completed",
          "The pixel budget was not enforced");
  context.removeDocument(document.id);
}

}  // namespace

void runAutomationDrawingAnalysisTests(Context& context) {
  const std::pair<const char*, std::function<void()>> cases[]{
      {"slices and rows", [&context]() { slicesAndRows(context); }},
      {"refusals at submit", [&context]() { refusalsAtSubmit(context); }},
      {"revision binding", [&context]() { revisionBinding(context); }},
      {"cancel and deadline", [&context]() { cancelAndDeadline(context); }},
      {"row limit", [&context]() { rowLimit(context); }},
      {"raster batches and pixel budget",
       [&context]() { rasterBatchesAndPixelBudget(context); }},
      {"admission and retention",
       [&context]() { admissionAndRetention(context); }}};
  for (const auto& test : cases) {
    test.second();
    std::cout << "automation-drawing-analysis-test: " << test.first
              << " PASS\n";
  }
}

}  // namespace GroundzeroAutomation
