#pragma once

#include "tcommon.h"

#include <QString>

class Loader;
class PluginLoadController;
class QThread;

void observePluginControllerForTest(PluginLoadController* controller,
                                    Loader* loader, QThread* thread);

#ifdef TOONZQT_EXPORTS
DV_EXPORT_API
#else
DV_IMPORT_API
#endif
bool verifyPluginControllerLifetime(const QString& directory, bool hasPlugin,
                                    QString& failure);
