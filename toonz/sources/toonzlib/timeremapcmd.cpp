#include "toonz/timeremapcmd.h"

// TnzLib includes
#include "toonz/timeremap.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshcell.h"

// TnzBase includes
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"

// TnzCore includes
#include "tundo.h"

// Qt includes
#include <QObject>
#include <QString>

// STD includes
#include <algorithm>
#include <vector>

namespace {

//=============================================================================
// The state of the time remap of a column
//=============================================================================

struct RemapState {
  bool m_enabled;
  bool m_cycled;
  std::vector<TDoubleKeyframe> m_keys;

  RemapState() : m_enabled(false), m_cycled(false) {}

  static RemapState fromColumn(TXshLevelColumn *column) {
    RemapState state;
    TDoubleParam *curve = column->getTimeRemapParam();
    state.m_enabled     = column->isTimeRemapEnabled();
    state.m_cycled      = curve->isCycleEnabled();
    int n               = curve->getKeyframeCount();
    for (int i = 0; i < n; i++) state.m_keys.push_back(curve->getKeyframe(i));
    return state;
  }

  void toColumn(TXshLevelColumn *column) const {
    TDoubleParam *curve = column->getTimeRemapParam();
    curve->clearKeyframes();
    for (const TDoubleKeyframe &key : m_keys) curve->setKeyframe(key);
    curve->enableCycle(m_cycled);
    column->setTimeRemapEnabled(m_enabled);
  }
};

//=============================================================================
// The undo of a command: the states of the changed columns, by column index
//=============================================================================

class TimeRemapUndo final : public TUndo {
public:
  struct Entry {
    int m_columnIndex;
    RemapState m_before, m_after;
  };

private:
  TXsheetHandle *m_xshHandle;
  // The xsheet the command changed: a sub-xsheet may be open, or closed,
  // when the undo runs, and the column indexes are those of this one
  TXsheetP m_xsh;
  std::vector<Entry> m_entries;
  QString m_name;

  void set(bool after) const {
    TXsheet *xsh = m_xsh.getPointer();
    if (!xsh) return;
    for (const Entry &entry : m_entries) {
      TXshColumn *column           = xsh->getColumn(entry.m_columnIndex);
      TXshLevelColumn *levelColumn = column ? column->getLevelColumn() : 0;
      if (!levelColumn) continue;
      (after ? entry.m_after : entry.m_before).toColumn(levelColumn);
    }
    m_xshHandle->notifyXsheetChanged();
  }

public:
  TimeRemapUndo(TXsheetHandle *xshHandle, const std::vector<Entry> &entries,
                const QString &name)
      : m_xshHandle(xshHandle)
      , m_xsh(xshHandle->getXsheet())
      , m_entries(entries)
      , m_name(name) {}

  void undo() const override { set(false); }
  void redo() const override { set(true); }
  int getSize() const override {
    int size = sizeof(*this);
    for (const Entry &entry : m_entries)
      size += sizeof(Entry) +
              sizeof(TDoubleKeyframe) *
                  (entry.m_before.m_keys.size() + entry.m_after.m_keys.size());
    return size;
  }
  QString getHistoryString() override { return m_name; }
};

//-----------------------------------------------------------------------------

TXshLevelColumn *getUnlockedLevelColumn(TXsheet *xsh, int index) {
  TXshColumn *column = xsh->getColumn(index);
  if (!column || column->isLocked()) return 0;
  return column->getLevelColumn();
}

//-----------------------------------------------------------------------------

//! The state that applies the preset to the rows [r0, r1] of the column; false
//! if there is nothing to remap there.
bool makeState(TimeRemapCmd::Preset preset, TXshLevelColumn *column, int r0,
               int r1, RemapState &state) {
  int c0 = 0, c1 = -1;
  if (!column->getRange(c0, c1) || c1 < c0) return false;

  int first = (r0 < 0) ? c0 : r0;
  int last  = (r1 < 0) ? c1 : r1;
  if (last < first) return false;

  std::vector<TimeRemap::Key> keys;
  switch (preset) {
  case TimeRemapCmd::Hold:
    // The held cell has to exist
    if (column->getCell(first).isEmpty()) return false;
    keys = TimeRemap::holdKeys(first, last, first);
    break;

  case TimeRemapCmd::Loop:
  case TimeRemapCmd::Reverse:
    // Only the cells of the column take part in the loop and in the reverse
    first = std::max(first, c0);
    last  = std::min(last, c1);
    if (last < first) return false;
    keys = (preset == TimeRemapCmd::Loop) ? TimeRemap::loopKeys(first, last)
                                          : TimeRemap::reverseKeys(first, last);
    break;
  }

  state           = RemapState();
  state.m_enabled = true;
  state.m_cycled  = (preset == TimeRemapCmd::Loop);
  for (const TimeRemap::Key &key : keys) {
    TDoubleKeyframe k(key.first, key.second);
    k.m_type = TDoubleKeyframe::Linear;
    state.m_keys.push_back(k);
  }
  return true;
}

//-----------------------------------------------------------------------------

//! Applies the new states to the columns, as a single undo
int commit(TXsheetHandle *xshHandle,
           const std::vector<TimeRemapUndo::Entry> &entries,
           const QString &name) {
  if (entries.empty()) return 0;

  TXsheet *xsh = xshHandle->getXsheet();
  for (const TimeRemapUndo::Entry &entry : entries) {
    TXshLevelColumn *column =
        xsh->getColumn(entry.m_columnIndex)->getLevelColumn();
    entry.m_after.toColumn(column);
  }

  TUndoManager::manager()->add(new TimeRemapUndo(xshHandle, entries, name));
  xshHandle->notifyXsheetChanged();
  return (int)entries.size();
}

}  // namespace

//=============================================================================

int TimeRemapCmd::applyPreset(Preset preset, const std::set<int> &columnIndices,
                              int r0, int r1, TXsheetHandle *xshHandle) {
  TXsheet *xsh = xshHandle->getXsheet();

  std::vector<TimeRemapUndo::Entry> entries;
  for (int index : columnIndices) {
    TXshLevelColumn *column = getUnlockedLevelColumn(xsh, index);
    if (!column) continue;

    TimeRemapUndo::Entry entry;
    if (!makeState(preset, column, r0, r1, entry.m_after)) continue;
    entry.m_columnIndex = index;
    entry.m_before      = RemapState::fromColumn(column);
    entries.push_back(entry);
  }

  QString name = (preset == Hold)   ? QObject::tr("Hold Column")
                 : (preset == Loop) ? QObject::tr("Loop Column")
                                    : QObject::tr("Reverse Column");
  return commit(xshHandle, entries, name);
}

//-----------------------------------------------------------------------------

int TimeRemapCmd::clear(const std::set<int> &columnIndices,
                        TXsheetHandle *xshHandle) {
  TXsheet *xsh = xshHandle->getXsheet();

  std::vector<TimeRemapUndo::Entry> entries;
  for (int index : columnIndices) {
    TXshLevelColumn *column = getUnlockedLevelColumn(xsh, index);
    if (!column || !column->isTimeRemapUsed()) continue;

    TimeRemapUndo::Entry entry;
    entry.m_columnIndex = index;
    entry.m_before      = RemapState::fromColumn(column);
    // m_after is the unused state: disabled, no keys, not cycled
    entries.push_back(entry);
  }

  return commit(xshHandle, entries, QObject::tr("Clear Time Remap"));
}
