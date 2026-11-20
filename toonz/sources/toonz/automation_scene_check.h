#pragma once

#include "automation.h"

namespace GroundzeroAutomation {

// The analysis work of a scene check runs on the application thread in steps
// that stop once this many milliseconds have passed: a step lasts at most this
// long plus the one drawing or cast level in progress.
constexpr int SceneCheckSliceMs = 20;

// scene.check.submit: start one bounded, cancellable scene check as an
// analysis job (unused_styles or inventory); rows come from job.result.
QJsonArray sceneCheckOperations();
QJsonValue executeSceneCheck(Context& context, const QString& method,
                             const QJsonObject& params);

#ifdef BUILD_UI_PORT_TESTS
// How a scene check job divided its work: application-thread steps run, and
// the most drawings or cast levels any one step took.
struct SceneCheckSteps final {
  int steps            = 0;
  int longestStepItems = 0;
};
SceneCheckSteps sceneCheckSteps(Context& context, const QString& jobId);
// Sleep this long after each unit of work, so a test can see the slicing.
void setSceneCheckItemDelayMs(int milliseconds);
void runAutomationSceneCheckTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
