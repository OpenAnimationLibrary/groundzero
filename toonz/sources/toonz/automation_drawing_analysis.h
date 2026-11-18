#pragma once

#include "automation.h"

namespace GroundzeroAutomation {

// Limits of one drawing analysis job. A budget hit during the job ends it
// completed with analysis.complete false; an estimate over a budget refuses at
// submit.
struct AnalysisBudget final {
  int drawings       = 4096;
  qint64 strokes     = 16777216;
  qint64 pixels      = 4294967296;
  int deadlineMs     = 120000;
  int rows           = 65536;
  int rowsForTotals  = 4096;
  int pixelsPerSlice = 2097152;
};

// drawing.analysis.submit: starts the job and returns its record.
QJsonObject submitDrawingAnalysis(Context& context, Document& document,
                                  const QJsonObject& params);
// The tool's catalog entry.
QJsonObject drawingAnalysisOperation();

#ifdef BUILD_UI_PORT_TESTS
// Test hooks: start a job with its own budget and no scheduled slices, then
// run its slices by hand. stepDrawingAnalysis returns whether slices remain.
QJsonObject startDrawingAnalysisUnscheduled(Context& context,
                                            Document& document,
                                            const QJsonObject& params,
                                            const AnalysisBudget& budget);
bool stepDrawingAnalysis(Context& context, const QString& jobId);
void runAutomationDrawingAnalysisTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
