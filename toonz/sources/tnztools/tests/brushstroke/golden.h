#pragma once

#ifndef BRUSHSTROKE_GOLDEN_H
#define BRUSHSTROKE_GOLDEN_H

#include "harness.h"

namespace brushstroke {

// Exact: every line and every pixel equal. Tolerant: the comparison used on
// a platform that has no golden data of its own; see compare().
enum class Mode { Exact, Tolerant };

struct Comparison {
  std::vector<std::string> problems;  // empty when the record matches
  std::string note;                   // what a tolerant match let through
};

// "<os>-<cpu>-qt<major>": the golden data of a platform lives in a folder of
// this name.
QString platformKey();

bool goldenExists(const QString& dir, const std::string& name);
Record loadGolden(const QString& dir, const std::string& name);
void saveGolden(const QString& dir, const std::string& name,
                const Record& record);

// Tolerant mode exists because the golden data was recorded on one platform
// and the fit, the anti-aliasing and libmypaint do not round the same way on
// every compiler and processor. Counts, styles, flags and order still have to
// be equal; coordinates may differ by 1e-6, a savebox by one pixel, and a
// pixel by 4 levels, with at most 1% of the pixels the gesture changed (2 at
// least) further off.
Comparison compare(const Record& expected, const Record& actual, Mode mode);

}  // namespace brushstroke

#endif
