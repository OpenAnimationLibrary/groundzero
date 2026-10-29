#pragma once

#ifndef COLUMNCOLOREFFECTPOPUP_H
#define COLUMNCOLOREFFECTPOPUP_H

//=============================================================================
// ColumnColorEffectPopup
//-----------------------------------------------------------------------------
// The Color Effect dialog of a level column (doc/animate_color_effect.md):
// Brightness, Tint, Alpha and Advanced, opened from the transparency settings
// of the column header. It edits the parameters of the column fx, with undo.

class TXshColumn;

namespace ColumnColorEffectPopup {

//! Shows the dialog for the column, if it is a level column.
void open(TXshColumn *column);

}  // namespace ColumnColorEffectPopup

#endif
