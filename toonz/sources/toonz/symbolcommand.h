#pragma once

#ifndef SYMBOLCOMMAND_H
#define SYMBOLCOMMAND_H

#include <set>
#include <string>

class TXshChildLevel;

//=============================================================================
// Symbols
//-----------------------------------------------------------------------------
// A symbol is a sub-xsheet level (TXshChildLevel) of the scene cast. Every
// column exposing it is an instance of the symbol, so editing the symbol
// updates all its instances, while the cells of each instance choose which
// symbol frames are shown (the instance timing). Nothing new is saved: the
// symbols are the sub-xsheets the scene already has.
//-----------------------------------------------------------------------------

namespace SymbolCmd {

//! Asks for a name, then collapses the columns into a new symbol (as Collapse).
void convertToSymbol(std::set<int> &indices);

//! Exposes all the symbol frames from \b row in column \b col of the current
//! xsheet if that column is empty. Otherwise a new column is inserted at \b col
//! if \b insertColumn is true, else the first empty column on the right of
//! \b col is used; \b col returns the column used. Returns false, after
//! telling the user, if the symbol contains the current xsheet.
bool insertInstance(TXshChildLevel *symbol, int row, int &col,
                    bool insertColumn);

//! Opens the symbol (as Open Sub-Xsheet) from one of its instances in the
//! current xsheet.
void editSymbol(TXshChildLevel *symbol);

//! Makes a separate, editable copy of the symbol from one of its instances in
//! the current xsheet (as Clone Sub-Xsheet). The copy gets a new column.
void duplicateSymbol(TXshChildLevel *symbol);

//! Renames the symbol. Returns false if the name is empty or already used.
bool renameSymbol(TXshChildLevel *symbol, const std::wstring &name);

enum TimingMode { PlayOnce = 0, Loop, Hold };

//! Fills the rows \b r0..r1 of each column in \b c0..c1 that exposes a single
//! symbol with the symbol frames from \b startFrame (1-based): played once
//! (then holding the last frame), looped, or held. Returns false if no column
//! in the range is a symbol instance.
bool setInstanceTiming(int r0, int c0, int r1, int c1, TimingMode mode,
                       int startFrame);

//! Asks for the timing mode and the start frame, then calls
//! setInstanceTiming().
void openInstanceTimingDialog(int r0, int c0, int r1, int c1);

}  // namespace SymbolCmd

#endif  // SYMBOLCOMMAND_H
