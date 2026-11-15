#pragma once
#include "tcommon.h"
class QString;
#if defined(BUILD_UI_PORT_TESTS) && defined(_WIN32)
#ifdef TSOUND_EXPORTS
DV_EXPORT_API bool runWindowsSoundOutputOwnershipChecks(QString& failure);
#else
DV_IMPORT_API bool runWindowsSoundOutputOwnershipChecks(QString& failure);
#endif
#endif
