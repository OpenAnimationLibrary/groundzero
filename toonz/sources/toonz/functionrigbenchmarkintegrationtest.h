#pragma once

class FunctionViewer;

// Runs in the disposable real application scene after Function Editor checks.
// Throws on correctness failures; timings are descriptive, never a speed gate.
void runFunctionRigBenchmarkIntegrationTest(FunctionViewer *viewer);
