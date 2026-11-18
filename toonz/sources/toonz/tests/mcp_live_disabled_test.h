#pragma once

#include <functional>

class QMainWindow;

/// Record a normal launch's state with live automation disabled, and with
/// `control` in the fixture configuration, the same state while a live
/// session runs and after it stops.
/// Args:
///   window: The editor window of this launch.
///   completed: Receives 0 when every observation passes, else 1.
void startMcpLiveDisabledTest(QMainWindow* window,
                              std::function<void(int)> completed);
