#pragma once

#ifndef ACTIVECHECKS_H
#define ACTIVECHECKS_H

class ToonzScene;
class TOutputProperties;

// Call on the GUI thread before saving or starting a job. This updates only
// the supplied properties; it never emits notifications or changes View state.
void captureActiveRenderChecks(ToonzScene *scene,
                               TOutputProperties *properties);

#endif
