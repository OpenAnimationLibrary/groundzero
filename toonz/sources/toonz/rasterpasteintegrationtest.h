#pragma once

class QString;

// Exercise real Xsheet paste commands in the prepared disposable native runner.
// Resets the fixture scene and restores clipboard, tool, and preferences.
bool runRasterPasteIntegrationTest(QString* error);
