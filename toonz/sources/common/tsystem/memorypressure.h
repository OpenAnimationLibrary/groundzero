// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdio>

namespace TSystem::detail {

inline bool memoryShortageFromMemInfo(std::FILE* input) {
  unsigned long long memTotal = 0, memAvailable = 0, memFree = 0, value = 0;
  bool hasAvailable = false;
  char line[256];
  while (std::fgets(line, sizeof(line), input)) {
    if (std::sscanf(line, "MemTotal: %llu kB", &value) == 1)
      memTotal = value;
    else if (std::sscanf(line, "MemFree: %llu kB", &value) == 1)
      memFree = value;
    else if (std::sscanf(line, "MemAvailable: %llu kB", &value) == 1) {
      memAvailable = value;
      hasAvailable = true;
      break;  // The kernel reports MemTotal and MemFree before MemAvailable.
    }
  }
  if (!memTotal) return false;
  return (hasAvailable ? memAvailable : memFree) < memTotal / 7;
}

}  // namespace TSystem::detail
