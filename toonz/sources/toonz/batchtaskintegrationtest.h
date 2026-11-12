#pragma once

#include <QString>

// Compiled only with BUILD_UI_PORT_TESTS.
void exerciseBatchTaskLifetime(const QString& testRoot);
int runBatchTaskFixtureChild();
