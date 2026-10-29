#pragma once

#ifndef LIBRARY_ITEMS_INCLUDED
#define LIBRARY_ITEMS_INCLUDED

//  Pure helpers of the shared Library (see toonz/librarycommand.h): the file
//  names of the library items, and the merge of a pose set into the poses of
//  a scene. They use neither the GUI nor the file system, so they are tested
//  alone (toonzlib/tests/libraryitems_test.cpp).

#include <cwctype>
#include <map>
#include <string>
#include <vector>

namespace LibraryItems {

//! The longest item name, in characters: there is room left for a "_N"
//! suffix and the extensions of the files of the item.
const std::wstring::size_type MaxNameLength = 120;

//-----------------------------------------------------------------------------

//! Returns the text without the white space at both ends.
inline std::wstring trimmed(const std::wstring &text) {
  std::wstring::size_type begin = 0, end = text.size();
  while (begin < end && std::iswspace(text[begin])) ++begin;
  while (end > begin && std::iswspace(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

//-----------------------------------------------------------------------------

//! Returns a copy of the name in lower case, to compare file names as the
//! case-insensitive file systems do.
inline std::wstring foldCase(const std::wstring &name) {
  std::wstring result(name);
  for (wchar_t &c : result) c = (wchar_t)std::towlower(c);
  return result;
}

//-----------------------------------------------------------------------------

//! Whether the character cannot be used in a file name on some system: the
//! path separators, the characters Windows forbids and the control ones.
inline bool isForbiddenChar(wchar_t c) {
  return c < 0x20 || c == 0x7f || c == L'/' || c == L'\\' || c == L':' ||
         c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' ||
         c == L'|';
}

//-----------------------------------------------------------------------------

//! Whether the name, up to its first dot, is a device name reserved by
//! Windows (CON, PRN, AUX, NUL, COM0-COM9, LPT0-LPT9), in any case. The
//! check is made on every system, since a Library can be shared.
inline bool isReservedName(const std::wstring &name) {
  std::wstring base = name.substr(0, name.find(L'.'));
  while (!base.empty() && std::iswspace(base.back())) base.pop_back();
  std::wstring upper;
  for (wchar_t c : base) upper += (wchar_t)std::towupper(c);

  if (upper == L"CON" || upper == L"PRN" || upper == L"AUX" || upper == L"NUL")
    return true;
  return upper.size() == 4 &&
         (upper.compare(0, 3, L"COM") == 0 ||
          upper.compare(0, 3, L"LPT") == 0) &&
         upper[3] >= L'0' && upper[3] <= L'9';
}

//-----------------------------------------------------------------------------

//! Returns the name made safe as the file name of a library item on every
//! system: trimmed, with "_" in place of the path separators and of the
//! characters that cannot be used in file names, without the dots at its
//! start (hidden files) and the dots and spaces at its end (dropped by
//! Windows), at most MaxNameLength characters long, and with "_" added
//! after a reserved device name ("CON.x" becomes "CON_.x"). Returns an empty
//! string if nothing is left, as for "." or "..".
inline std::wstring sanitizeFileName(const std::wstring &name) {
  std::wstring result;
  for (wchar_t c : trimmed(name)) result += isForbiddenChar(c) ? L'_' : c;

  std::wstring::size_type first = 0;
  while (first < result.size() &&
         (result[first] == L'.' || std::iswspace(result[first])))
    ++first;
  result = result.substr(first);
  if (result.size() > MaxNameLength) result.resize(MaxNameLength);
  while (!result.empty() &&
         (result.back() == L'.' || std::iswspace(result.back())))
    result.pop_back();
  if (result.empty()) return result;

  if (isReservedName(result)) {
    std::wstring::size_type dot = result.find(L'.');
    if (dot == std::wstring::npos) dot = result.size();
    while (dot > 0 && std::iswspace(result[dot - 1])) --dot;
    result.insert(dot, L"_");
  }
  return result;
}

//-----------------------------------------------------------------------------

//! Whether the name can be used as it is as the name of a library item.
inline bool isSafeFileName(const std::wstring &name) {
  return !name.empty() && sanitizeFileName(name) == name;
}

//-----------------------------------------------------------------------------

//! Returns \b base if \b exists(base) is false, otherwise the first of
//! base_2, base_3... for which it is false (or an empty string if there is
//! none in a reasonable range).
template <class Exists>
std::wstring uniqueName(const std::wstring &base, Exists exists) {
  if (!exists(base)) return base;
  for (int i = 2; i < 100000; ++i) {
    std::wstring name = base + L"_" + std::to_wstring(i);
    if (!exists(name)) return name;
  }
  return std::wstring();
}

//=============================================================================
//  Pose sets
//-----------------------------------------------------------------------------

//! Pairs the items of a pose set with the poses of a scene by name: the k-th
//! item named N of the set goes with the k-th pose named N of the scene, so
//! a set saved from a scene pairs with all its poses, duplicated names
//! included. Returns, for each incoming item, the index of its existing pose
//! or -1 if it has none. Names are compared exactly.
inline std::vector<int> pairByName(const std::vector<std::wstring> &existing,
                                   const std::vector<std::wstring> &incoming) {
  std::map<std::wstring, std::vector<int>> indicesByName;
  for (int i = 0; i < (int)existing.size(); ++i)
    indicesByName[existing[i]].push_back(i);

  std::map<std::wstring, int> usedCount;
  std::vector<int> pairs;
  pairs.reserve(incoming.size());
  for (const std::wstring &name : incoming) {
    auto it   = indicesByName.find(name);
    int &used = usedCount[name];
    if (it != indicesByName.end() && used < (int)it->second.size())
      pairs.push_back(it->second[used++]);
    else
      pairs.push_back(-1);
  }
  return pairs;
}

//-----------------------------------------------------------------------------

//! Returns how many incoming items are paired with an existing one.
inline int countPaired(const std::vector<int> &pairs) {
  int count = 0;
  for (int index : pairs)
    if (index >= 0) ++count;
  return count;
}

//-----------------------------------------------------------------------------

//! Merges the incoming items into the existing ones, with the pairs returned
//! by pairByName(): a paired item replaces its existing one in place if
//! \b replace is true, and is dropped otherwise; the other items are added
//! at the end, in their order.
template <class T>
std::vector<T> mergeByName(const std::vector<T> &existing,
                           const std::vector<T> &incoming,
                           const std::vector<int> &pairs, bool replace) {
  std::vector<T> result(existing);
  for (std::size_t i = 0; i < incoming.size(); ++i) {
    int target = i < pairs.size() ? pairs[i] : -1;
    if (target >= 0 && target < (int)existing.size()) {
      if (replace) result[target] = incoming[i];
    } else
      result.push_back(incoming[i]);
  }
  return result;
}

}  // namespace LibraryItems

#endif  // LIBRARY_ITEMS_INCLUDED
