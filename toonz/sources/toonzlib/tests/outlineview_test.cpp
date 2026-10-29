// Regression test of the Outline View of the columns
// (doc/animate_outline_view.md): the palette that colours the outlined columns
// (toonz/outlinecolors.h), and the column status bit that switches a column to
// it, which must keep the other bits of the status word.

#include "toonz/outlinecolors.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshlevelcolumn.h"

#include <QCoreApplication>

#include <climits>
#include <iostream>
#include <set>
#include <stdexcept>
#include <tuple>

// Reads and writes the status word of a column, which is protected: the word
// is what the scene file keeps, and what clone() copies. This struct is a
// friend of TXshColumn.
struct TXshColumnStatusAccess {
  static int get(const TXshColumn &column) { return column.getStatusWord(); }
  static void set(TXshColumn &column, int status) {
    column.setStatusWord(status);
  }
};

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

typedef std::tuple<int, int, int> RgbKey;

RgbKey rgbOf(int index) {
  const OutlineColors::Rgb c = OutlineColors::colorForIndex(index);
  return RgbKey(c.r, c.g, c.b);
}

//------------------------------------------------------------------

void testColors() {
  require(OutlineColors::count == 8, "colors: the palette has 8 colours");

  // Stable: the same index gives the same colour
  for (int i = -20; i < 40; i++)
    require(rgbOf(i) == rgbOf(i), "colors: not stable");

  // The first 8 indices are all different, and far enough apart to be told
  // apart on screen
  std::set<RgbKey> distinct;
  for (int i = 0; i < OutlineColors::count; i++) {
    distinct.insert(rgbOf(i));
    const OutlineColors::Rgb a = OutlineColors::colorForIndex(i);
    // Neither too light for a light background nor too dark for a dark one
    const double luma = 0.299 * a.r + 0.587 * a.g + 0.114 * a.b;
    require(luma >= 60.0 && luma <= 190.0, "colors: too light or too dark");
    for (int j = i + 1; j < OutlineColors::count; j++) {
      const OutlineColors::Rgb b = OutlineColors::colorForIndex(j);
      const int dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
      require(dr * dr + dg * dg + db * db >= 60 * 60,
              "colors: two colours are too close");
    }
  }
  require(int(distinct.size()) == OutlineColors::count,
          "colors: the first 8 indices are not distinct");

  // The slot is always in range, and the palette repeats with its size
  for (int i = -100; i <= 100; i++) {
    const int s = OutlineColors::slot(i);
    require(s >= 0 && s < OutlineColors::count, "colors: slot out of range");
    require(rgbOf(i) == rgbOf(i + OutlineColors::count),
            "colors: the palette does not repeat");
  }

  // Negative indices (such as the camera column) and huge ones
  require(OutlineColors::slot(-1) == OutlineColors::count - 1,
          "colors: slot of -1");
  require(OutlineColors::slot(-OutlineColors::count) == 0,
          "colors: slot of minus the size");
  require(rgbOf(-1) == rgbOf(OutlineColors::count - 1), "colors: colour of -1");
  require(OutlineColors::slot(INT_MAX) == INT_MAX % OutlineColors::count,
          "colors: slot of INT_MAX");
  const int minSlot = OutlineColors::slot(INT_MIN);
  require(minSlot >= 0 && minSlot < OutlineColors::count,
          "colors: slot of INT_MIN");
  (void)OutlineColors::colorForIndex(INT_MIN);
  (void)OutlineColors::colorForIndex(INT_MAX);
}

//------------------------------------------------------------------

void testStatusBit() {
  TXshColumnP column(new TXshLevelColumn);

  // A new column is not an outline, and its status word is untouched
  require(!column->isOutlineView(), "status: a new column is an outline");
  require(TXshColumnStatusAccess::get(*column) == 0,
          "status: a new column has a status word");

  // Switch on every other bit that the status word keeps (locked, selection
  // locked, light table excluded, masked, camstand and preview hidden)
  column->lock(true);
  column->lockSelection(true);
  column->setLightTableExcluded(true);
  column->setIsMask(true);
  column->setCamstandVisible(false);
  column->setPreviewVisible(false);
  const int others = TXshColumnStatusAccess::get(*column);
  require(others != 0 && (others & 0x100) == 0,
          "status: the other bits are not set, or the new one is");

  // On: the bit 0x100 is set and nothing else changed
  column->setOutlineView(true);
  require(column->isOutlineView(), "status: not an outline after on");
  require(TXshColumnStatusAccess::get(*column) == (others | 0x100),
          "status: on must set the bit 0x100 only");
  require(column->isLocked() && column->isSelectionLocked() &&
              column->isLightTableExcluded() && column->isMask() &&
              !column->isCamstandVisible() && !column->isPreviewVisible(),
          "status: on changed another bit");

  // The setters of the other bits keep the outline bit
  column->lock(false);
  column->setCamstandVisible(true);
  require(column->isOutlineView(), "status: another setter cleared the bit");
  column->lock(true);
  column->setCamstandVisible(false);

  // On again changes nothing
  column->setOutlineView(true);
  require(TXshColumnStatusAccess::get(*column) == (others | 0x100),
          "status: on twice changed the word");

  // Off clears the bit only
  column->setOutlineView(false);
  require(!column->isOutlineView(), "status: still an outline after off");
  require(TXshColumnStatusAccess::get(*column) == others,
          "status: off must clear the bit 0x100 only");
  column->setOutlineView(false);
  require(TXshColumnStatusAccess::get(*column) == others,
          "status: off twice changed the word");
}

//------------------------------------------------------------------

// The status word is what the scene file saves, loads and clones; the column
// kinds that keep it copy it whole, so the bit goes through them.
void testStatusRoundTrip() {
  // A word with the bit, as loaded from a scene: 0x100 and the lock bit
  TXshColumnP loaded(new TXshLevelColumn);
  TXshColumnStatusAccess::set(*loaded, 0x100 | 0x8);
  require(loaded->isOutlineView() && loaded->isLocked(),
          "round trip: the loaded word is not read");
  require(!loaded->isLightTableExcluded() && loaded->isCamstandVisible() &&
              loaded->isPreviewVisible(),
          "round trip: the loaded word sets other bits");

  // Saving writes the word, loading sets it back
  const int saved = TXshColumnStatusAccess::get(*loaded);
  require(saved == 0x108, "round trip: the saved word differs");
  TXshColumnP reloaded(new TXshLevelColumn);
  TXshColumnStatusAccess::set(*reloaded, saved);
  require(reloaded->isOutlineView() && reloaded->isLocked(),
          "round trip: the bit is lost by a reload");

  // The clone (copy and paste of a column, undo) keeps it
  TXshColumnP copy(loaded->clone());
  require(copy->isOutlineView() && copy->isLocked(),
          "round trip: the bit is lost by clone()");
  require(TXshColumnStatusAccess::get(*copy) == saved,
          "round trip: clone() changed the word");

  // Toggling the copy does not touch the original
  copy->setOutlineView(false);
  require(!copy->isOutlineView() && loaded->isOutlineView(),
          "round trip: a clone shares the flag");
  require(TXshColumnStatusAccess::get(*copy) == 0x8,
          "round trip: off on the clone must leave the lock only");

  // A word of an older scene (no bit) is not an outline
  TXshColumnP old(new TXshLevelColumn);
  TXshColumnStatusAccess::set(*old, 0x1 | 0x2 | 0x40 | 0x80);
  require(!old->isOutlineView(), "round trip: an old word is an outline");
}

}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  try {
    testColors();
    testStatusBit();
    testStatusRoundTrip();
  } catch (const std::exception &e) {
    std::cerr << "FAILED: " << e.what() << "\n";
    return 1;
  }
  std::cout << "outlineview_test passed\n";
  return 0;
}
