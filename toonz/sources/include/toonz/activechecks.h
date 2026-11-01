#pragma once

#ifndef TOONZ_ACTIVECHECKS_H
#define TOONZ_ACTIVECHECKS_H

#include "tcommon.h"
#include "tcheckrenderdata.h"

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

class ToonzScene;

// Resolve the saved column path against this scene. The FX identifier is valid
// only for this session and is preserved by the renderer's FX clones.
DVAPI void resolveActiveRenderChecks(ToonzScene *scene,
                                     TRenderCheckSettings &checks);

#endif
