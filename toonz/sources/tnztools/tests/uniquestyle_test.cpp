// Standalone checks of the pure decisions behind "Make Unique Style" (the
// name of the copied style, whether a style is shared); no GUI or
// application initialization.
// Build by hand:
//   g++ -std=c++17 -I../../include uniquestyle_test.cpp
#include "tools/uniquestyle.h"

#include <iostream>
#include <string>
#include <vector>

using namespace UniqueStyle;

namespace {

int failures = 0;

void check(bool condition, const char *what) {
  if (condition) return;
  std::cerr << "Check failed: " << what << '\n';
  failures++;
}

typedef std::vector<std::wstring> Names;

void testNoSuffix() {
  check(uniqueName(L"Sky", Names{L"Sky"}) == L"Sky (unique)",
        "first copy of a plain name gets ' (unique)'");
  check(uniqueName(L"Sky", Names{L"Sky", L"Sky (unique)"}) == L"Sky (unique 2)",
        "second copy gets number 2");
  check(uniqueName(L"Sky", Names()) == L"Sky (unique)",
        "no existing names: ' (unique)'");
  check(uniqueName(L"color_5", Names{L"color_5", L"color_6"}) ==
            L"color_5 (unique)",
        "default style names work like the others");
  // Other names that only look alike do not take numbers.
  check(uniqueName(L"Sky", Names{L"Sky", L"Sky blue (unique)", L"Sea (unique)",
                                 L"Sky(unique)", L"Sky (Unique)"}) ==
            L"Sky (unique)",
        "suffixes of other names are ignored");
  check(uniqueName(L"Cielo é", Names{L"Cielo é (unique)"}) ==
            L"Cielo é (unique 2)",
        "non-ASCII names");
}

void testExistingSuffix() {
  check(baseName(L"Sky (unique)") == L"Sky", "strip ' (unique)'");
  check(baseName(L"Sky (unique 2)") == L"Sky", "strip ' (unique 2)'");
  check(baseName(L"Sky (unique 123)") == L"Sky", "strip a long number");
  check(baseName(L"Sky (unique) (unique 4)") == L"Sky (unique)",
        "only one suffix is stripped");
  check(baseName(L"Sky") == L"Sky", "plain name unchanged");
  check(baseName(L"Sky (unique 0)") == L"Sky (unique 0)", "0 is not ours");
  check(baseName(L"Sky (unique 02)") == L"Sky (unique 02)",
        "leading zero is not ours");
  check(baseName(L"Sky (unique x)") == L"Sky (unique x)",
        "not a number: unchanged");
  check(baseName(L"Sky (unique )") == L"Sky (unique )", "no number: unchanged");
  check(baseName(L"Sky (uniquely)") == L"Sky (uniquely)",
        "other word: unchanged");
  check(baseName(L"Sky(unique)") == L"Sky(unique)", "no space: unchanged");
  check(baseName(L"Sky (unique 1234567890)") == L"Sky (unique 1234567890)",
        "too many digits: unchanged");

  // A copy of a copy: the suffix does not pile up.
  check(uniqueName(L"Sky (unique)", Names{L"Sky", L"Sky (unique)"}) ==
            L"Sky (unique 2)",
        "copy of the first copy");
  check(uniqueName(L"Sky (unique 2)",
                   Names{L"Sky", L"Sky (unique)", L"Sky (unique 2)"}) ==
            L"Sky (unique 3)",
        "copy of the second copy");
  check(uniqueName(L"Sky (unique 2)", Names{L"Sky (unique 2)"}) ==
            L"Sky (unique)",
        "number 1 is free: ' (unique)'");
  // "(unique 1)" written by hand counts as number 1.
  check(uniqueName(L"Sky", Names{L"Sky (unique 1)"}) == L"Sky (unique 2)",
        "' (unique 1)' takes number 1");

  check(suffixNumber(L"Sky (unique)", L"Sky") == 1, "suffix number 1");
  check(suffixNumber(L"Sky (unique 7)", L"Sky") == 7, "suffix number 7");
  check(suffixNumber(L"Sky (unique 7)", L"Sk") == 0, "other base");
  check(suffixNumber(L"Sky", L"Sky") == 0, "no suffix");
  check(nameWithNumber(L"Sky", 1) == L"Sky (unique)", "name of copy 1");
  check(nameWithNumber(L"Sky", 12) == L"Sky (unique 12)", "name of copy 12");
}

void testGaps() {
  check(uniqueName(L"Sky", Names{L"Sky", L"Sky (unique)", L"Sky (unique 3)"}) ==
            L"Sky (unique 2)",
        "the gap at 2 is used");
  check(uniqueName(L"Sky", Names{L"Sky", L"Sky (unique 2)",
                                 L"Sky (unique 3)"}) == L"Sky (unique)",
        "the gap at 1 is used");
  check(uniqueName(L"Sky", Names{L"Sky (unique 3)", L"Sky (unique)",
                                 L"Sky (unique 2)", L"Sky (unique 5)"}) ==
            L"Sky (unique 4)",
        "unordered names, gap at 4");
  check(uniqueName(L"Sky", Names{L"Sky (unique)", L"Sky (unique)",
                                 L"Sky (unique 2)"}) == L"Sky (unique 3)",
        "duplicate names count once");
}

void testEmptyName() {
  check(baseName(L"") == L"", "empty base of empty name");
  check(baseName(L"(unique)") == L"", "suffix only gives an empty base");
  check(baseName(L"(unique 3)") == L"", "numbered suffix only");
  check(uniqueName(L"", Names()) == L"(unique)",
        "empty name: no leading space");
  check(uniqueName(L"", Names{L"", L"(unique)"}) == L"(unique 2)",
        "empty name, second copy");
  check(uniqueName(L"(unique)", Names{L"(unique)"}) == L"(unique 2)",
        "copy of an empty-named copy");
  check(uniqueName(L"", Names{L"Sky (unique)"}) == L"(unique)",
        "named copies do not count for the empty name");
  check(suffixNumber(L" (unique)", L"") == 0,
        "a leading space is not the empty name's suffix");
}

void testShared() {
  std::vector<int> none;
  check(countUses(none, 5) == 0, "no regions: 0 uses");
  check(!isShared(none, 5), "no regions: not shared");

  std::vector<int> other{3, 4, 3};
  check(countUses(other, 5) == 0, "style not used: 0 uses");
  check(!isShared(other, 5), "style not used: not shared");

  std::vector<int> one{3, 5, 4};
  check(countUses(one, 5) == 1, "one region: 1 use");
  check(!isShared(one, 5), "one region: not shared");

  std::vector<int> two{5, 3, 5};
  check(countUses(two, 5) == 2, "two regions: 2 uses");
  check(isShared(two, 5), "two regions: shared");

  std::vector<int> many{5, 5, 0, 5, 7, 5};
  check(countUses(many, 5) == 4, "many regions: 4 uses");
  check(isShared(many, 5), "many regions: shared");
  check(!isShared(many, 7), "the other style of the list is not shared");
  check(isShared(std::vector<int>{0, 0}, 0) && countUses(many, 0) == 1,
        "unfilled regions (style 0) are counted like any id");
}

}  // namespace

int main() {
  testNoSuffix();
  testExistingSuffix();
  testGaps();
  testEmptyName();
  testShared();

  if (failures) {
    std::cerr << "uniquestyle_test: " << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "uniquestyle_test: all checks passed\n";
  return 0;
}
