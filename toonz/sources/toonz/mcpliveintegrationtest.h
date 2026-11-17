#pragma once

#include <functional>

class QMainWindow;

/// Start the fixed disposable GUI/SDK journey on the outer application loop.
/// Args:
///   window: The native editor window that owns the fixture and production LiveUi.
///   completed: Reports a journey that finishes before application shutdown.
void startMcpLiveIntegrationTest(QMainWindow* window, std::function<void(int)> completed);

/// Reap the fixed SDK child after QApplication exits without servicing GUI events.
/// Returns:
///   Zero only when the SDK child and independent native observations both pass.
int finishMcpLiveIntegrationTest();
