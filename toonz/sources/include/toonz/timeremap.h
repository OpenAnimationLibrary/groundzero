#pragma once

#ifndef TIMEREMAP_H
#define TIMEREMAP_H

#include <cmath>
#include <utility>
#include <vector>

//=============================================================================
// Pure helpers of the per-column time remap (master plan B7).
//
// A level column can carry a remap curve (see TLevelColumnFx): the value of
// the curve at the row R of the output is the number of the xsheet frame (the
// 1-based number shown in the xsheet ruler) whose cell is shown at R. So the
// identity is the curve with the value R + 1 at the row R.
//
// The curve is applied inside the span of its keys only: the rows before the
// first key and, unless the curve is cycled, the rows after the last key are
// not remapped. A cycled curve repeats its span indefinitely.
//
// Everything here is header only and does not depend on the rest of OpenToonz,
// so it can be unit tested alone (toonzlib/tests/timeremap_test.cpp).
//=============================================================================

namespace TimeRemap {

//! A key of a remap curve: frame is a 0-based row, value a 1-based frame
//! number.
typedef std::pair<int, double> Key;

//! Converts a value of the remap curve to the 0-based row it designates.
inline int valueToRow(double value) { return (int)std::floor(value + 0.5) - 1; }

//! Converts a 0-based row to the value of the remap curve that designates it.
inline double rowToValue(int row) { return row + 1.0; }

//! Row shown at \b row when the curve has keys at \b keyCount rows from
//! \b firstKeyRow to \b lastKeyRow, and \b curveValue is its value at \b row.
inline int remapRow(int row, int keyCount, double firstKeyRow,
                    double lastKeyRow, bool cycled, double curveValue) {
  if (keyCount <= 0 || row < firstKeyRow) return row;
  if (row > lastKeyRow && !cycled) return row;
  return valueToRow(curveValue);
}

//-----------------------------------------------------------------------------
// Loop / Hold / Reverse
//-----------------------------------------------------------------------------

//! Row shown at \b row by the loop of the rows [first, last], that starts at
//! \b first and goes on forever. The rows before \b first are not remapped.
inline int loopRow(int row, int first, int last) {
  if (last < first) last = first;
  if (row < first) return row;
  return first + (row - first) % (last - first + 1);
}

//! Row shown at \b row by the hold of the row \b heldRow along the rows
//! [from, to]. The rows outside the hold are not remapped.
inline int holdRow(int row, int from, int to, int heldRow) {
  return (row >= from && row <= to) ? heldRow : row;
}

//! Row shown at \b row by the reverse of the rows [first, last]. The rows
//! outside the range are not remapped.
inline int reverseRow(int row, int first, int last) {
  return (row >= first && row <= last) ? first + last - row : row;
}

//-----------------------------------------------------------------------------
// Keys of the presets (linear keys). Loop needs the curve to be cycled.
//-----------------------------------------------------------------------------

//! Keys that show \b heldRow along the rows [from, to].
inline std::vector<Key> holdKeys(int from, int to, int heldRow) {
  std::vector<Key> keys;
  keys.push_back(Key(from, rowToValue(heldRow)));
  if (to > from) keys.push_back(Key(to, rowToValue(heldRow)));
  return keys;
}

//! Keys that loop the rows [first, last] from \b first on; the curve has to be
//! cycled. The last key is the first row again, one row after \b last, so the
//! cycle has the length of the range.
inline std::vector<Key> loopKeys(int first, int last) {
  std::vector<Key> keys;
  if (last < first) last = first;
  keys.push_back(Key(first, rowToValue(first)));
  if (last > first) keys.push_back(Key(last, rowToValue(last)));
  keys.push_back(Key(last + 1, rowToValue(first)));
  return keys;
}

//! Keys that play the rows [first, last] backwards.
inline std::vector<Key> reverseKeys(int first, int last) {
  std::vector<Key> keys;
  keys.push_back(Key(first, rowToValue(last)));
  if (last > first) keys.push_back(Key(last, rowToValue(first)));
  return keys;
}

}  // namespace TimeRemap

#endif
