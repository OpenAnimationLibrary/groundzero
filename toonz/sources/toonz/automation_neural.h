#pragma once

#include "automation.h"
#include "toonz/neuralstyle.h"
#include "toonz/neuralstyleexecution.h"

namespace GroundzeroAutomation {

struct NeuralPreview final {
  NeuralStyle::Profile profile;
  std::shared_ptr<const NeuralStyle::WorkerExecution> execution;
  QJsonObject metadata;
  int frame       = 0;
  double strength = 35;
};

QJsonObject neuralPreviewOperation();
NeuralPreview prepareNeuralPreview(
    Context& context, Document& document, const QJsonObject& params,
    NeuralStyle::WorkerExecution::Clock::time_point deadline);
void checkNeuralDeadline(
    NeuralStyle::WorkerExecution::Clock::time_point deadline);

}  // namespace GroundzeroAutomation
