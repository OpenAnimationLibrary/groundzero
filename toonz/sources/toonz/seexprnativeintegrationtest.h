#pragma once

#include <QString>

class QMainWindow;

// Compiled only with BUILD_SEEXPR_TESTS. The caller must prepare an isolated
// copied portable profile before normal application environment initialization.
bool prepareSeExprNativeIntegrationTest(QString *error);
int runSeExprNativeIntegrationTest(QMainWindow *window);
