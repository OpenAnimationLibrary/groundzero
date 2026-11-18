#pragma once

#include <QString>

class QMainWindow;

/// Run the live-bridge lifecycle checks inside the native live fixture.
/// Args:
///   window: The editor window that owns each temporary live session's UI.
///   directory: The fixture's empty private grant directory; left empty.
void runNativeLiveLifecycleTests(QMainWindow* window, const QString& directory);
