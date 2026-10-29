#include "levelselection.h"

#include "toonz/txshsimplelevel.h"

#include <QCoreApplication>
#include <QPointer>

#include <cstdlib>
#include <iostream>
#include <set>

namespace {

void require(bool condition, const char *message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(1);
}

}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);

  QSet<TXshLevelP> levels;
  std::set<size_t> hashes;
  const int levelCount = 256;
  for (int i = 0; i < levelCount; ++i) {
    // Identical names must not collapse distinct level identities.
    TXshLevelP level = new TXshSimpleLevel(L"drawing");
    levels.insert(level);
    levels.insert(TXshLevelP(level.getPointer()));
    hashes.insert(qHash(level));
  }
  require(levels.size() == levelCount,
          "distinct levels were lost or aliases were duplicated");
  require(hashes.size() > 1,
          "all live levels hash alike: selection falls back to a bool hash");

  levels.insert(TXshLevelP());
  levels.insert(TXshLevelP());
  require(levels.size() == levelCount + 1 && levels.contains(TXshLevelP()),
          "null levels must remain safe to insert and remove");
  levels.remove(TXshLevelP());

  // The popup's selection remains an owner, including after implicit sharing
  // detaches. A raw-pointer set would fail these lifetime checks.
  QPointer<TXshLevel> retained;
  {
    TXshLevelP level = new TXshSimpleLevel(L"retained");
    retained        = level.getPointer();
    levels.insert(level);
  }
  require(!retained.isNull(), "selection released its owned level");
  QSet<TXshLevelP> copy = levels;
  levels.remove(TXshLevelP(retained.data()));
  require(!retained.isNull() && copy.contains(TXshLevelP(retained.data())),
          "detaching one selection invalidated the other selection");
  copy.clear();
  require(retained.isNull(), "removed selection retained an unused level");

  std::cout << "level selection identity and lifetime checks passed\n";
  return 0;
}
