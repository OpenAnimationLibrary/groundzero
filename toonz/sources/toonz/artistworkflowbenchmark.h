#pragma once
class QMainWindow;
// Only compiled into CI builds. Requires the isolated native UI fixture.
int runArtistWorkflowBenchmark(QMainWindow *window);
