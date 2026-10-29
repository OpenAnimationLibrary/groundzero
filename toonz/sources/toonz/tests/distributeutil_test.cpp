// Standalone checks of the helpers of the Distribute to Layers / Distribute to
// Frames / Convert Selection to Symbol commands; no GUI or application
// initialization.
// g++ -std=c++17 -I.. distributeutil_test.cpp -o /tmp/distributeutil_test
#include "distributeutil.h"
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace DistributeUtil;

namespace {

void fail(const char *what) {
  std::cerr << what << '\n';
  std::exit(1);
}

void expect(bool condition, const char *what) {
  if (!condition) fail(what);
}

// A fake image: groupOf[i] is the group of the stroke i, 0 if it is not in a
// group.
std::vector<Part> partition(const std::vector<int> &groupOf,
                            const std::vector<int> &selected) {
  return partitionStrokes(
      selected, [&](int i) { return groupOf[i] != 0; },
      [&](int a, int b) { return groupOf[a] == groupOf[b]; });
}

bool same(const std::vector<Part> &parts, const std::vector<Part> &expected) {
  return parts == expected;
}

}  // namespace

int main() {
  // Nothing selected, nothing to distribute
  {
    std::vector<int> groupOf = {0, 1, 1};
    expect(partition(groupOf, {}).empty(), "empty input gives no parts");
  }

  // Strokes that are not grouped get a part each, in stacking order
  {
    std::vector<int> groupOf = {0, 0, 0, 0};
    expect(same(partition(groupOf, {0, 1, 2, 3}), {{0}, {1}, {2}, {3}}),
           "ungrouped strokes are parts of their own");
    expect(same(partition(groupOf, {3, 1}), {{1}, {3}}),
           "ungrouped strokes in stacking order, unselected ones left out");
  }

  // The strokes of a group stay together
  {
    std::vector<int> groupOf = {0, 7, 7, 7, 0, 9, 9};
    expect(same(partition(groupOf, {0, 1, 2, 3, 4, 5, 6}),
                {{0}, {1, 2, 3}, {4}, {5, 6}}),
           "groups give one part each, ungrouped strokes in between");
  }

  // Order of the input and repeated indices do not matter
  {
    std::vector<int> groupOf = {0, 7, 7, 0};
    expect(same(partition(groupOf, {3, 2, 2, 1, 0, 3}), {{0}, {1, 2}, {3}}),
           "unsorted input with repeated indices");
  }

  // Parts are sorted by their lowest stroke index even if a group is not
  // contiguous in the selection (a group inside another one, for instance)
  {
    std::vector<int> groupOf = {5, 6, 6, 0, 5};
    expect(same(partition(groupOf, {0, 1, 2, 3, 4}), {{0, 4}, {1, 2}, {3}}),
           "parts sorted by their lowest stroke");
  }

  // A group that is only partly selected gives a part with the selected
  // strokes only
  {
    std::vector<int> groupOf = {4, 4, 4, 0};
    expect(same(partition(groupOf, {1, 2, 3}), {{1, 2}, {3}}),
           "partly selected group");
  }

  // A group is never merged with the ungrouped strokes, whatever their order
  {
    std::vector<int> groupOf = {0, 2, 0, 2};
    expect(same(partition(groupOf, {0, 1, 2, 3}), {{0}, {1, 3}, {2}}),
           "ungrouped stroke between the members of a group");
  }

  // The predicates are not asked about strokes that are not grouped
  {
    std::vector<int> groupOf = {0, 0, 3, 3};
    bool asked               = false;
    std::vector<Part> parts  = partitionStrokes(
        std::vector<int>{0, 1, 2, 3}, [&](int i) { return groupOf[i] != 0; },
        [&](int a, int b) {
          if (groupOf[a] == 0 || groupOf[b] == 0) asked = true;
          return groupOf[a] == groupOf[b];
        });
    expect(!asked, "sameGroup only called for grouped strokes");
    expect(same(parts, {{0}, {1}, {2, 3}}), "mixed parts");
  }

  // Negative indices are ignored
  {
    std::vector<int> groupOf = {0, 0};
    expect(same(partition(groupOf, {-1, 0}), {{0}}), "negative index ignored");
  }

  // Level names: the next free suffix
  {
    std::set<std::wstring> used;
    auto isUsed = [&](const std::wstring &n) { return used.count(n) > 0; };
    expect(numberedName(L"Hero", isUsed) == L"Hero_1", "first suffix is 1");
    used.insert(L"Hero");
    expect(numberedName(L"Hero", isUsed) == L"Hero_1",
           "the base name itself does not matter");
    used.insert(L"Hero_1");
    used.insert(L"Hero_2");
    expect(numberedName(L"Hero", isUsed) == L"Hero_3", "next free suffix");
    used.erase(L"Hero_1");
    expect(numberedName(L"Hero", isUsed) == L"Hero_1", "lowest free suffix");
    used.insert(L"Hero_1");
    used.insert(L"Hero_3");
    expect(numberedName(L"Hero", isUsed) == L"Hero_4", "skips used suffixes");
    expect(numberedName(L"Hero_2", isUsed) == L"Hero_2_1",
           "a suffixed name is a base name too");
    expect(numberedName(L"", isUsed) == L"_1", "empty base name");
  }

  // Names are picked one after the other, each new level being used
  {
    std::set<std::wstring> used = {L"Bg"};
    std::vector<std::wstring> names;
    for (int i = 0; i < 3; ++i) {
      std::wstring name = numberedName(
          L"Bg", [&](const std::wstring &n) { return used.count(n) > 0; });
      used.insert(name);
      names.push_back(name);
    }
    expect(names[0] == L"Bg_1" && names[1] == L"Bg_2" && names[2] == L"Bg_3",
           "consecutive names");
  }

  // Frame numbers of the new drawings
  {
    std::set<int> used = {1, 2, 3, 5};
    expect(freeFrameNumbers(used, 2, 3) == std::vector<int>({4, 6, 7}),
           "free numbers skip the used ones");
    expect(freeFrameNumbers(used, 5, 2) == std::vector<int>({6, 7}),
           "free numbers after the last one");
    expect(freeFrameNumbers(used, 0, 2) == std::vector<int>({4, 6}),
           "free numbers from the start");
    expect(freeFrameNumbers(used, -1, 1) == std::vector<int>({4}),
           "no frame number counts as 0");
    expect(freeFrameNumbers(used, 2, 0).empty(), "no new drawing");
    expect(freeFrameNumbers(std::set<int>(), 1, 2) == std::vector<int>({2, 3}),
           "empty level");
  }

  std::cout << "distributeutil_test passed\n";
  return 0;
}
