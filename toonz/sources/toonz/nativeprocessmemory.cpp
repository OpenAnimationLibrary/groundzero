#include "nativeprocessmemory.h"

#include <QFile>
#include <stdexcept>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <sys/resource.h>
#endif

qint64 nativeProcessPeakResidentBytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    return qint64(counters.PeakWorkingSetSize);
#elif defined(__APPLE__)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) return qint64(usage.ru_maxrss);
#else
  QFile status("/proc/self/status");
  if (status.open(QIODevice::ReadOnly)) {
    for (const QByteArray &raw : status.readAll().split('\n')) {
      const QByteArray line = raw.simplified();
      if (line.startsWith("VmHWM:")) {
        bool valid = false;
        const qint64 kibibytes = line.split(' ').value(1).toLongLong(&valid);
        if (valid && kibibytes > 0) return kibibytes * 1024;
      }
    }
  }
#endif
  throw std::runtime_error("Cannot measure process peak resident memory");
}

const char *nativeProcessPeakResidentSource() {
#if defined(_WIN32)
  return "GetProcessMemoryInfo.PeakWorkingSetSize";
#elif defined(__APPLE__)
  return "getrusage(RUSAGE_SELF).ru_maxrss (bytes on macOS)";
#else
  return "/proc/self/status VmHWM (KiB converted to bytes)";
#endif
}
