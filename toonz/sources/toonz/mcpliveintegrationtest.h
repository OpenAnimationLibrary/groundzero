#pragma once

#include <functional>

class QMainWindow;

// Runs only in the prepared disposable native UI process. Completion is
// asynchronous so the production bridge serves the outer application loop.
void startMcpLiveIntegrationTest(QMainWindow* window, std::function<void(int)> completed);

// Reaps the fixed SDK child after QApplication exits; never services GUI events.
int finishMcpLiveIntegrationTest();
