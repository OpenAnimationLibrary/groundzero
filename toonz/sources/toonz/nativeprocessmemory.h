#pragma once

#include <QtGlobal>

// Cumulative process high-water mark. It cannot attribute an earlier allocation
// to the workload being measured, and it is not simultaneous process-tree RSS.
qint64 nativeProcessPeakResidentBytes();
const char *nativeProcessPeakResidentSource();
