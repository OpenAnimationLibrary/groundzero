#pragma once

class QMainWindow;
class QString;

// Only compiled into BUILD_UI_PORT_TESTS builds. Call preparation immediately
// after constructing QApplication, before initializing Toonz or Preferences.
bool prepareNativeUiIntegrationTest(QString *error);

// Run once from the event loop after normal native application initialization.
// The caller exits the application with this status; no document is saved.
int runNativeUiIntegrationTest(QMainWindow *window);
