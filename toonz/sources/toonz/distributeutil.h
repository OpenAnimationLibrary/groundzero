#pragma once

#ifndef DISTRIBUTEUTIL_H
#define DISTRIBUTEUTIL_H

#include <algorithm>
#include <set>
#include <string>
#include <vector>

//=============================================================================
// DistributeUtil
//-----------------------------------------------------------------------------
// Pure helpers of the "Distribute to Layers", "Distribute to Frames" and
// "Convert Selection to Symbol" commands (distributecommand.cpp), kept free of
// OpenToonz and Qt dependencies so that they can be checked standalone (see
// tests/distributeutil_test.cpp).
//-----------------------------------------------------------------------------

namespace DistributeUtil {

//! The indices, in increasing order, of the strokes that go together to the
//! same new layer or frame.
typedef std::vector<int> Part;

//! Splits the strokes with the given indices into the parts to distribute.
//!
//! The strokes of a group stay together in one part, and every stroke that is
//! not in a group is a part of its own. The strokes in the result are in the
//! stacking order of the image (index 0 is at the bottom): the parts are sorted
//! by their lowest stroke index and the indices of a part are increasing. The
//! order of \e indices and repeated indices do not matter.
//!
//! \e isGrouped(index) tells if the stroke is in a group, and
//! \e sameGroup(a, b) if two grouped strokes are in the same group; it is not
//! called for strokes that are not grouped. Only the given strokes are
//! distributed, so a group that is partly selected gives a part with the
//! selected strokes only.
template <class IsGrouped, class SameGroup>
std::vector<Part> partitionStrokes(std::vector<int> indices,
                                   IsGrouped isGrouped, SameGroup sameGroup) {
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

  std::vector<Part> parts;
  std::vector<bool> isGroupPart;  // parallel to parts
  for (int index : indices) {
    if (index < 0) continue;

    bool grouped = isGrouped(index);
    int target   = -1;
    if (grouped)
      for (int p = 0; p < (int)parts.size(); ++p)
        if (isGroupPart[p] && sameGroup(parts[p].front(), index)) {
          target = p;
          break;
        }
    if (target < 0) {
      parts.push_back(Part());
      isGroupPart.push_back(grouped);
      target = (int)parts.size() - 1;
    }
    parts[target].push_back(index);
  }
  return parts;
}

//-----------------------------------------------------------------------------

//! Returns \e baseName followed by "_" and the lowest number from 1 up for
//! which \e isUsed(name) is false: the name of a new level made from the
//! level \e baseName.
template <class IsUsed>
std::wstring numberedName(const std::wstring &baseName, IsUsed isUsed) {
  const int maxTries = 1000000;
  std::wstring name;
  for (int n = 1; n <= maxTries; ++n) {
    name = baseName + L"_" + std::to_wstring(n);
    if (!isUsed(name)) break;
  }
  return name;
}

//-----------------------------------------------------------------------------

//! Returns the first \e count frame numbers after \e after that are not in
//! \e used: the numbers of the new drawings of a level, that follow the
//! drawing they come from.
inline std::vector<int> freeFrameNumbers(const std::set<int> &used, int after,
                                         int count) {
  std::vector<int> numbers;
  for (int n = std::max(after, 0) + 1; (int)numbers.size() < count; ++n)
    if (!used.count(n)) numbers.push_back(n);
  return numbers;
}

}  // namespace DistributeUtil

#endif  // DISTRIBUTEUTIL_H
