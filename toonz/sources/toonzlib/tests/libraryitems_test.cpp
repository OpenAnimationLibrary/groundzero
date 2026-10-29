// Regression test of the pure helpers of the shared Library
// (toonz/libraryitems.h): the file names of the library items (sanitised,
// reserved names, unique "name_N" choice) and the merge of a pose set into
// the poses of a scene.

#include "toonz/libraryitems.h"

#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

//------------------------------------------------------------------

void testTrimmed() {
  using LibraryItems::trimmed;
  require(trimmed(L"  Bob \t") == L"Bob", "trimmed: both ends");
  require(trimmed(L"Walk Cycle") == L"Walk Cycle", "trimmed: inner space");
  require(trimmed(L"").empty(), "trimmed: empty");
  require(trimmed(L"   ").empty(), "trimmed: only spaces");
}

//------------------------------------------------------------------

void testSanitize() {
  using LibraryItems::sanitizeFileName;
  require(sanitizeFileName(L"Bob") == L"Bob", "sanitize: plain name");
  require(sanitizeFileName(L" Walk/Cycle ") == L"Walk_Cycle",
          "sanitize: trimmed, slash replaced");
  require(sanitizeFileName(L"a\\b:c*d?e\"f<g>h|i") == L"a_b_c_d_e_f_g_h_i",
          "sanitize: forbidden characters");
  require(sanitizeFileName(L"ab\ncd") == L"ab_cd", "sanitize: control char");
  require(sanitizeFileName(L"../up") == L"_up", "sanitize: parent path");
  require(sanitizeFileName(L"").empty(), "sanitize: empty");
  require(sanitizeFileName(L"   ").empty(), "sanitize: spaces");
  require(sanitizeFileName(L".").empty(), "sanitize: dot");
  require(sanitizeFileName(L"...").empty(), "sanitize: dots");
  require(sanitizeFileName(L".hidden") == L"hidden", "sanitize: leading dot");
  require(sanitizeFileName(L". . x") == L"x", "sanitize: leading dots");
  require(sanitizeFileName(L"name. . ") == L"name",
          "sanitize: trailing dots and spaces");
  require(sanitizeFileName(L"v1.2") == L"v1.2", "sanitize: inner dot kept");
  require(sanitizeFileName(L"Pose été") == L"Pose été",
          "sanitize: accents kept");

  // Reserved device names
  require(sanitizeFileName(L"CON") == L"CON_", "sanitize: CON");
  require(sanitizeFileName(L"con.txt") == L"con_.txt", "sanitize: con.txt");
  require(sanitizeFileName(L"Com1") == L"Com1_", "sanitize: Com1");
  require(sanitizeFileName(L"LPT0") == L"LPT0_", "sanitize: LPT0");
  require(sanitizeFileName(L"COM10") == L"COM10", "sanitize: COM10");
  require(sanitizeFileName(L"Console") == L"Console", "sanitize: Console");

  // Length
  require(sanitizeFileName(std::wstring(200, L'a')).size() ==
              LibraryItems::MaxNameLength,
          "sanitize: long name cut");
  require(sanitizeFileName(std::wstring(119, L'a') + L" b...") ==
              std::wstring(119, L'a'),
          "sanitize: cut name without trailing space");
}

//------------------------------------------------------------------

void testNames() {
  using LibraryItems::isReservedName;
  require(isReservedName(L"nul"), "reserved: nul");
  require(isReservedName(L"LPT9"), "reserved: LPT9");
  require(isReservedName(L"lpt0.foo"), "reserved: with extension");
  require(isReservedName(L"aux "), "reserved: trailing space");
  require(!isReservedName(L"NULL"), "reserved: NULL");
  require(!isReservedName(L"LPT"), "reserved: LPT");
  require(!isReservedName(L"COMX"), "reserved: COMX");

  using LibraryItems::isSafeFileName;
  require(isSafeFileName(L"Bob"), "safe: Bob");
  require(!isSafeFileName(L""), "safe: empty");
  require(!isSafeFileName(L"Bob/"), "safe: slash");
  require(!isSafeFileName(L" Bob"), "safe: leading space");
  require(!isSafeFileName(L"CON"), "safe: CON");
  require(!isSafeFileName(L".."), "safe: parent folder");

  require(LibraryItems::foldCase(L"BoB_2") == L"bob_2", "foldCase");
}

//------------------------------------------------------------------

void testUniqueName() {
  using LibraryItems::uniqueName;
  std::set<std::wstring> taken = {L"Bob", L"Bob_2"};
  auto exists                  = [&taken](const std::wstring &name) {
    return taken.count(name) > 0;
  };
  require(uniqueName(L"Ann", exists) == L"Ann", "unique: free name");
  require(uniqueName(L"Bob", exists) == L"Bob_3", "unique: next free");
  taken.insert(L"Bob_3");
  require(uniqueName(L"Bob", exists) == L"Bob_4", "unique: after more");
  require(uniqueName(L"X", [](const std::wstring &) { return true; }).empty(),
          "unique: none free");
}

//------------------------------------------------------------------

struct Item {
  std::wstring m_name;
  int m_value;
};

std::vector<std::wstring> names(const std::vector<Item> &items) {
  std::vector<std::wstring> result;
  for (const Item &item : items) result.push_back(item.m_name);
  return result;
}

std::vector<int> values(const std::vector<Item> &items) {
  std::vector<int> result;
  for (const Item &item : items) result.push_back(item.m_value);
  return result;
}

void testPoseSetMerge() {
  using namespace LibraryItems;

  // The k-th incoming "A" goes with the k-th existing "A"
  std::vector<Item> existing = {{L"A", 1}, {L"B", 2}, {L"A", 3}};
  std::vector<Item> incoming = {{L"A", 10}, {L"C", 20}, {L"A", 30}, {L"A", 40}};
  std::vector<int> pairs     = pairByName(names(existing), names(incoming));
  require(pairs == std::vector<int>({0, -1, 2, -1}), "pair: by occurrence");
  require(countPaired(pairs) == 2, "pair: count");

  require(values(mergeByName(existing, incoming, pairs, true)) ==
              std::vector<int>({10, 2, 30, 20, 40}),
          "merge: replace in place, add the others");
  require(values(mergeByName(existing, incoming, pairs, false)) ==
              std::vector<int>({1, 2, 3, 20, 40}),
          "merge: keep the scene poses");

  // Names are compared exactly
  std::vector<Item> lower    = {{L"a", 5}};
  std::vector<int> casePairs = pairByName(names(existing), names(lower));
  require(casePairs == std::vector<int>({-1}), "pair: case sensitive");

  // Into a scene without poses
  std::vector<Item> none;
  std::vector<int> nonePairs = pairByName(names(none), names(incoming));
  require(countPaired(nonePairs) == 0, "pair: empty scene");
  require(values(mergeByName(none, incoming, nonePairs, true)) ==
              std::vector<int>({10, 20, 30, 40}),
          "merge: into an empty scene, duplicates kept");

  // A set saved from the scene pairs with all its poses
  std::vector<int> selfPairs = pairByName(names(existing), names(existing));
  require(selfPairs == std::vector<int>({0, 1, 2}), "pair: same set");
  require(values(mergeByName(existing, existing, selfPairs, false)) ==
              values(existing),
          "merge: same set adds nothing");
}

}  // namespace

int main() {
  try {
    testTrimmed();
    testSanitize();
    testNames();
    testUniqueName();
    testPoseSetMerge();
  } catch (const std::exception &e) {
    std::cerr << "libraryitems_test failed: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "libraryitems_test passed" << std::endl;
  return 0;
}
