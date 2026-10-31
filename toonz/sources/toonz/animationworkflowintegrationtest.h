#pragma once
#include <QString>
class QMainWindow;
// Uses the already isolated native-app fixture. Requires and leaves a fresh scene.
bool runAnimationWorkflowIntegrationTest(QMainWindow *window, QString *error);
