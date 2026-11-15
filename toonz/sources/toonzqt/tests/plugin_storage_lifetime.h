#pragma once

#include "tcommon.h"

#include <QString>

void checkpointPluginStorageCopyForTest();

#ifdef TOONZQT_EXPORTS
DV_EXPORT_API
#else
DV_IMPORT_API
#endif
bool verifyPluginStorageLifetime(QString& failure);
