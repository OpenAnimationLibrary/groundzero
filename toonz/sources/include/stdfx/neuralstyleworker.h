#pragma once

#include "toonz/neuralstyle.h"
#include "toonz/neuralstyleexecution.h"

class TFx;

namespace NeuralStyle {
#ifdef TNZSTDFX_EXPORTS
#define NEURAL_WORKER_FX_API DV_EXPORT_API
#else
#define NEURAL_WORKER_FX_API DV_IMPORT_API
#endif

// Create a transient one-frame native FX; the caller owns the returned
// instance.
NEURAL_WORKER_FX_API TFx* createWorkerPreviewFx(
    const Profile& profile, double strength, int frame,
    std::shared_ptr<const WorkerExecution> execution);

#undef NEURAL_WORKER_FX_API
}  // namespace NeuralStyle
