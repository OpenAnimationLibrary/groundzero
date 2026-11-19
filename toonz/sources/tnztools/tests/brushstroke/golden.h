#pragma once

#ifndef BRUSHSTROKE_GOLDEN_H
#define BRUSHSTROKE_GOLDEN_H

#include "harness.h"

namespace brushstroke {

struct Comparison {
  std::vector<std::string> problems;  // empty when the record matches
  // The numbers that are not equal, whether or not the tolerance covers them.
  int numbersOff = 0;
  double largest = 0;  // the largest difference between two of them
};

// "<os>-<cpu>-qt<major>": the golden data of a platform lives in a folder of
// this name.
QString platformKey();

bool goldenExists(const QString& dir, const std::string& name);
// The images are loaded where they are stored: only the reference platform
// stores them.
Record loadGolden(const QString& dir, const std::string& name);
void saveGolden(const QString& dir, const std::string& name,
                const Record& record, bool withImages);
void removeGolden(const QString& dir, const std::string& name);

// Compares a result with golden data. Counts, styles, flags, order, saveboxes
// and pixels (through their digest) must be equal. A coordinate, a thickness
// or a region box may differ by `tolerance`; 0 allows no difference.
Comparison compare(const Record& expected, const Record& actual,
                   double tolerance);

}  // namespace brushstroke

#endif
