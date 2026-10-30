#pragma once

#include "toonz/poselibrary.h"

class ToonzScene;

// Commits the complete library (including its base) as one native undo step.
// Returns false for an unchanged library or a scene which is no longer current.
bool replacePoseLibrary(ToonzScene *scene, const PoseLibrary &library,
                        const QString &label);
