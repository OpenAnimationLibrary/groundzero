#pragma once

class QMainWindow;
class QString;

// Only compiled into BUILD_UI_PORT_TESTS builds. Call preparation immediately
// after constructing QApplication, before initializing Toonz or Preferences.
bool prepareNativeUiIntegrationTest(QString *error);

// Discard only this prepared disposable runner's fixture resources, then use
// the actual New Scene path. Refuses unprepared/foreign storage or an existing
// modal dialog; checks dirty-resource inventory and the resulting empty scene.
bool resetNativeUiIntegrationScene(QString *error);

// Run once from the event loop after normal native application initialization.
// The caller exits the application with this status; no document is saved.
int runNativeUiIntegrationTest(QMainWindow *window);
