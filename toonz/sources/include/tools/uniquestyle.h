#pragma once

#ifndef UNIQUE_STYLE_INCLUDED
#define UNIQUE_STYLE_INCLUDED

// Pure decisions behind "Make Unique Style" (Gradient Transform tool), which
// gives one region its own copy of a palette style:
//  - the name of the copy: "<name> (unique)", then "<name> (unique 2)",
//    "<name> (unique 3)" ... A copy of a copy does not pile up suffixes: the
//    suffix of the original is removed first, and the smallest number that no
//    other style uses with the same name is taken (" (unique)" counts as 1).
//  - whether a style is shared, from the style ids of the regions of a
//    drawing.
//
// No GUI, no application objects: only the standard library.

#include <algorithm>
#include <string>
#include <vector>

namespace UniqueStyle {

namespace detail {

// "(unique" without the closing parenthesis.
inline const std::wstring &suffixStart() {
  static const std::wstring start(L"(unique");
  return start;
}

// If \p name ends with "(unique)" or "(unique N)" (N a positive integer
// without leading zeros and at most 9 digits), returns the position of the
// opening parenthesis and sets \p number (1 for "(unique)"). Otherwise
// returns std::wstring::npos.
inline std::wstring::size_type findSuffix(const std::wstring &name,
                                          int &number) {
  const std::wstring::size_type npos = std::wstring::npos;
  number                             = 0;
  if (name.empty() || name[name.size() - 1] != L')') return npos;
  std::wstring::size_type open = name.rfind(suffixStart());
  if (open == npos) return npos;
  std::wstring::size_type pos   = open + suffixStart().size();
  std::wstring::size_type close = name.size() - 1;
  if (pos == close) {  // "(unique)"
    number = 1;
    return open;
  }
  if (pos > close || name[pos] != L' ') return npos;
  ++pos;
  if (pos >= close || close - pos > 9) return npos;
  if (name[pos] < L'1' || name[pos] > L'9') return npos;
  int n = 0;
  for (; pos < close; ++pos) {
    wchar_t c = name[pos];
    if (c < L'0' || c > L'9') return npos;
    n = n * 10 + int(c - L'0');
  }
  number = n;
  return open;
}

}  // namespace detail

//! \p name without its " (unique)" or " (unique N)" suffix (one only). A
//! name made only of the suffix gives an empty name. Other names are
//! returned as they are.
inline std::wstring baseName(const std::wstring &name) {
  int number                   = 0;
  std::wstring::size_type open = detail::findSuffix(name, number);
  if (open == std::wstring::npos) return name;
  if (open == 0) return std::wstring();
  if (name[open - 1] != L' ') return name;  // "Sky(unique)": not ours
  return name.substr(0, open - 1);
}

//! The number of the suffix when \p name is \p base followed by a unique
//! suffix (1 for " (unique)"), or 0 when it is any other name.
inline int suffixNumber(const std::wstring &name, const std::wstring &base) {
  int number                   = 0;
  std::wstring::size_type open = detail::findSuffix(name, number);
  if (open == std::wstring::npos) return 0;
  if (base.empty()) return open == 0 ? number : 0;
  if (open != base.size() + 1 || name[base.size()] != L' ') return 0;
  if (name.compare(0, base.size(), base) != 0) return 0;
  return number;
}

//! \p base with the suffix of copy number \p number (" (unique)" for 1).
inline std::wstring nameWithNumber(const std::wstring &base, int number) {
  std::wstring suffix = detail::suffixStart();
  if (number > 1) suffix += L" " + std::to_wstring(number);
  suffix += L")";
  return base.empty() ? suffix : base + L" " + suffix;
}

//! The name for a new copy of the style called \p name, given the names of
//! the styles already in the palette (the original included).
inline std::wstring uniqueName(const std::wstring &name,
                               const std::vector<std::wstring> &existingNames) {
  std::wstring base = baseName(name);
  std::vector<int> used;
  for (const std::wstring &existing : existingNames) {
    int number = suffixNumber(existing, base);
    if (number > 0) used.push_back(number);
  }
  std::sort(used.begin(), used.end());
  int candidate = 1;
  for (int number : used) {
    if (number == candidate)
      ++candidate;
    else if (number > candidate)
      break;  // a gap: candidate is free
  }
  return nameWithNumber(base, candidate);
}

//! How many regions use \p styleId, from the style ids of all the regions.
inline int countUses(const std::vector<int> &regionStyleIds, int styleId) {
  return (int)std::count(regionStyleIds.begin(), regionStyleIds.end(), styleId);
}

//! Whether more than one region uses \p styleId: only then does giving one
//! region its own copy change anything.
inline bool isShared(const std::vector<int> &regionStyleIds, int styleId) {
  return countUses(regionStyleIds, styleId) > 1;
}

}  // namespace UniqueStyle

#endif  // UNIQUE_STYLE_INCLUDED
