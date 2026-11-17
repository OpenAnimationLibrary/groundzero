#pragma once

#include "tcommon.h"

#ifdef TOONZLIB_EXPORTS
#define STAGE_UNITS_VAR DV_EXPORT_VAR
#else
#define STAGE_UNITS_VAR DV_IMPORT_VAR
#endif

namespace Stage {

STAGE_UNITS_VAR extern const double inch;
STAGE_UNITS_VAR extern const double standardDpi;

}  // namespace Stage

#undef STAGE_UNITS_VAR
