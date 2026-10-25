#pragma once

#ifndef TIMEREMAPCMD_H
#define TIMEREMAPCMD_H

#include <set>

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

class TXsheetHandle;

//=============================================================================
// Commands of the time remap of the level columns (master plan B7,
// doc/time_remap.md). Each command is one undo step, whatever the number of
// columns it changes.
//=============================================================================

namespace TimeRemapCmd {

enum Preset {
  Hold,    //!< shows the cell at the first row along the rows
  Loop,    //!< repeats the cells of the rows from the first row on
  Reverse  //!< plays the cells of the rows backwards
};

//! Sets the remap curve of the level columns \b columnIndices (the others are
//! skipped) to the \b preset, replacing the curve they have, and enables it.
//! The rows are [r0, r1]; a negative \b r0 (\b r1) stands for the first (last)
//! row of the cells of each column. Returns the number of changed columns.
DVAPI int applyPreset(Preset preset, const std::set<int> &columnIndices, int r0,
                      int r1, TXsheetHandle *xshHandle);

//! Disables the remap of the level columns \b columnIndices and removes its
//! keys, so the columns are saved as if they never had one. Returns the number
//! of changed columns.
DVAPI int clear(const std::set<int> &columnIndices, TXsheetHandle *xshHandle);

}  // namespace TimeRemapCmd

#endif
