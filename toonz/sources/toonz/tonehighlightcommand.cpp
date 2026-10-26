// Tone / Highlight shading setup: Xsheet > Add Tone / Highlight Shading

// Tnz6 includes
#include "menubarcommandids.h"
#include "columnselection.h"
#include "tapp.h"

// TnzQt includes
#include "toonzqt/tselectionhandle.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/dvdialog.h"

// TnzLib includes
#include "toonz/txsheethandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"
#include "toonz/fxcommand.h"

// TnzBase includes
#include "tfx.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "tdoubleparam.h"
#include "tnotanimatableparam.h"

// TnzCore includes
#include "tmsgcore.h"
#include "tundo.h"

// Qt includes
#include <QList>

#include <set>
#include <string>

//=============================================================================
// Tone / Highlight setup
//-----------------------------------------------------------------------------
// For each selected level column, inserts two Tone / Highlight Fxs after the
// column: a Tone (multiply) and then a Highlight (screen) one. Their Matte
// ports are left free: the shapes of the shadows and highlights are drawn on
// other columns and connected there. All the Fxs of the command are created in
// one undo step.

namespace {

const std::string c_toneHighlightFxId = "toneHighlightFx";

// A Tone / Highlight Fx with the default parameters of the preset.
TFx *createToneHighlightFx(bool highlight) {
  TFx *fx = TFx::create(c_toneHighlightFxId);
  if (!fx) return nullptr;

  TParamContainer *params = fx->getParams();

  if (TIntEnumParam *mode =
          dynamic_cast<TIntEnumParam *>(params->getParam("mode"))) {
    mode->setDefaultValue(highlight ? 1 : 0);
    mode->setValue(highlight ? 1 : 0);
  }
  if (TPixelParam *color =
          dynamic_cast<TPixelParam *>(params->getParam("color"))) {
    TPixel32 value =
        highlight ? TPixel32(255, 244, 214) : TPixel32(90, 80, 140);
    color->setDefaultValue(value);
  }
  if (TDoubleParam *intensity =
          dynamic_cast<TDoubleParam *>(params->getParam("intensity"))) {
    double value = highlight ? 0.5 : 0.6;
    intensity->setDefaultValue(value);
  }
  return fx;
}

// The Tone / Highlight Fx connected to the output of the given Fx, if any.
TFx *findToneHighlightOutput(TFx *fx) {
  for (int i = 0; i < fx->getOutputConnectionCount(); i++) {
    TFxPort *port = fx->getOutputConnection(i);
    TFx *owner    = port ? port->getOwnerFx() : nullptr;
    if (owner && owner->getFxType() == c_toneHighlightFxId) return owner;
  }
  return nullptr;
}

}  // namespace

//-----------------------------------------------------------------------------

class ToneHighlightSetupCommand final : public MenuItemHandler {
public:
  ToneHighlightSetupCommand() : MenuItemHandler(MI_ToneHighlightSetup) {}

  void execute() override {
    TApp *app    = TApp::instance();
    TXsheet *xsh = app->getCurrentXsheet()->getXsheet();

    // selected columns, or the current one
    std::set<int> indices;
    TColumnSelection *selection = dynamic_cast<TColumnSelection *>(
        app->getCurrentSelection()->getSelection());
    if (selection && !selection->isEmpty())
      indices = selection->getIndices();
    else
      indices.insert(app->getCurrentColumn()->getColumnIndex());

    QList<TFxP> columnFxs;
    for (int c : indices) {
      if (c < 0 || xsh->isColumnEmpty(c)) continue;
      TXshColumn *column = xsh->getColumn(c);
      if (!column || !column->getLevelColumn() || !column->getFx()) continue;
      columnFxs.push_back(column->getFx());
    }
    if (columnFxs.isEmpty()) {
      DVGui::warning(
          QObject::tr("Select one or more level columns to add the Tone / "
                      "Highlight shading to."));
      return;
    }

    int col = *indices.begin();
    int row = app->getCurrentFrame()->getFrameIndex();

    TUndoScopedBlock undoBlock;
    for (const TFxP &columnFx : columnFxs) {
      // Each column gets its own Fxs, with unlinked parameters.
      TFx *tone = createToneHighlightFx(false);
      if (!tone) {
        DVGui::warning(
            QObject::tr("The Tone / Highlight Fx is not available."));
        break;
      }
      TFxCommand::insertFx(tone, QList<TFxP>() << columnFx,
                           QList<TFxCommand::Link>(), app, col, row);

      TFx *insertedTone = findToneHighlightOutput(columnFx.getPointer());
      if (!insertedTone) continue;

      TFx *highlight = createToneHighlightFx(true);
      if (!highlight) continue;
      TFxCommand::insertFx(highlight, QList<TFxP>() << TFxP(insertedTone),
                           QList<TFxCommand::Link>(), app, col, row);
    }
    app->getCurrentXsheet()->notifyXsheetChanged();
  }
} toneHighlightSetupCommand;
