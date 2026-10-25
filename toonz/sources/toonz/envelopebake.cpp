// Bake Envelope into Vector Level: applies the warp of an Envelope Deformer
// Plus fx to the strokes of the drawing it is applied to.

// Tnz6 includes
#include "tapp.h"
#include "menubarcommandids.h"

// TnzQt includes
#include "toonzqt/menubarcommand.h"
#include "toonzqt/dvdialog.h"
#include "toonzqt/icongenerator.h"

// TnzLib includes
#include "toonz/tcolumnfx.h"
#include "toonz/tdeformer.h"
#include "toonz/tfxhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/txshcell.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

// TnzBase includes
#include "tfx.h"
#include "tfxparam.h"
#include "tparamcontainer.h"
#include "tparamset.h"

// TnzCore includes
#include "tundo.h"
#include "tvectorimage.h"

#include <algorithm>
#include <string>
#include <vector>

//**************************************************************************
//    Local namespace stuff
//**************************************************************************

namespace {

// The id of the fx, which stdfx/envelopedeformerplusfx.cpp declares
const std::string c_fxType = "STD_envelopeDeformerPlusFx";

//--------------------------------------------------------------

// Builds the envelope of the fx at frame, as the fx does: the first
// "pointCount" points, sampled as a spline if "smooth" is on. Returns false if
// the parameters are not those of the fx.
bool getEnvelopes(TFx *fx, double frame, std::vector<TPointD> &rest,
                  std::vector<TPointD> &deformed) {
  TParamContainer *params = fx->getParams();
  TIntParam *count = dynamic_cast<TIntParam *>(params->getParam("pointCount"));
  TBoolParam *smooth = dynamic_cast<TBoolParam *>(params->getParam("smooth"));
  if (!count || !smooth) return false;

  int n = std::min(16, std::max(3, count->getValue()));

  std::vector<TPointD> restAnchors(n), deformedAnchors(n);
  for (int i = 0; i < n; ++i) {
    std::string suffix = std::to_string(i + 1);
    TPointParam *rest_ =
        dynamic_cast<TPointParam *>(params->getParam("rest" + suffix));
    TPointParam *point =
        dynamic_cast<TPointParam *>(params->getParam("point" + suffix));
    if (!rest_ || !point) return false;

    restAnchors[i]     = rest_->getValue(frame);
    deformedAnchors[i] = point->getValue(frame);
  }

  int samples = smooth->getValue() ? int(TEnvelopeDeformer::SPLINE_SAMPLES) : 1;
  TEnvelopeDeformer::sampleClosedSpline(restAnchors, samples, rest);
  TEnvelopeDeformer::sampleClosedSpline(deformedAnchors, samples, deformed);
  return true;
}

//==============================================================

class BakeEnvelopeUndo final : public TUndo {
  TXshSimpleLevelP m_level;
  TFrameId m_fid;
  TVectorImageP m_before, m_after;

public:
  BakeEnvelopeUndo(const TXshSimpleLevelP &level, const TFrameId &fid,
                   const TVectorImageP &before, const TVectorImageP &after)
      : m_level(level), m_fid(fid), m_before(before), m_after(after) {}

  void undo() const override { setImage(m_before); }
  void redo() const override { setImage(m_after); }

  int getSize() const override {
    return clampSize(getImageMemorySize(m_before.getPointer()) +
                     getImageMemorySize(m_after.getPointer()));
  }

  QString getHistoryString() override {
    return QObject::tr("Bake Envelope into Vector Level  : %1 (%2)")
        .arg(QString::fromStdWString(m_level->getName()))
        .arg(QString::fromStdString(m_fid.expand()));
  }

private:
  void setImage(const TVectorImageP &image) const {
    m_level->setFrame(m_fid, TImageP(image.getPointer()));
    m_level->setDirtyFlag(true);
    IconGenerator::instance()->invalidate(m_level.getPointer(), m_fid);
    TApp::instance()->getCurrentLevel()->notifyLevelChange();
  }
};

//==============================================================

class BakeEnvelopeCommand final : public MenuItemHandler {
public:
  BakeEnvelopeCommand() : MenuItemHandler(MI_BakeEnvelope) {}

  void execute() override {
    TApp *app = TApp::instance();

    // The current fx is an Envelope Deformer Plus, whose Source is a column
    TFx *fx = app->getCurrentFx()->getFx();
    if (!fx || fx->getFxType() != c_fxType) {
      DVGui::error(QObject::tr(
          "Select an Envelope Deformer Plus fx, applied to a vector level "
          "column, in the Schematic."));
      return;
    }

    TLevelColumnFx *columnFx = nullptr;
    if (fx->getInputPortCount() > 0 && fx->getInputPort(0)->getFx())
      columnFx = dynamic_cast<TLevelColumnFx *>(fx->getInputPort(0)->getFx());
    if (!columnFx || !columnFx->getColumn()) {
      DVGui::error(QObject::tr(
          "The Source of the Envelope Deformer Plus must be a level column "
          "directly."));
      return;
    }

    // The drawing in the column at the current frame, which must be a vector
    // one
    TFrameHandle *frameHandle = app->getCurrentFrame();
    if (frameHandle->getFrameType() != TFrameHandle::SceneFrame) {
      DVGui::error(QObject::tr("Go to a frame of the scene first."));
      return;
    }
    int row = frameHandle->getFrame();

    const TXshCell &cell   = columnFx->getColumn()->getCell(row);
    TXshSimpleLevel *level = cell.getSimpleLevel();
    if (!level || level->getType() != PLI_XSHLEVEL) {
      DVGui::error(QObject::tr(
          "The current frame of the column is not a Toonz Vector level "
          "drawing."));
      return;
    }

    TFrameId fid = cell.getFrameId();
    TVectorImageP before(level->getFullsampledFrame(fid, false));
    if (!before) {
      DVGui::error(QObject::tr("The drawing could not be loaded."));
      return;
    }

    // The parameters at the current frame. The fx works in the space of the
    // column's drawings, so its points are used as they are.
    std::vector<TPointD> rest, deformed;
    if (!getEnvelopes(fx, row, rest, deformed)) {
      DVGui::error(QObject::tr("The fx has no envelope parameters."));
      return;
    }

    TEnvelopeDeformer deformer(rest, deformed);
    if (!deformer.isValid() || deformer.isIdentity(1e-6)) {
      DVGui::error(QObject::tr(
          "The envelope does not deform at this frame: there is nothing to "
          "bake."));
      return;
    }

    if (DVGui::MsgBox(
            QObject::tr("The envelope of the current frame will be applied to "
                        "the strokes of the drawing %1 of the level %2. "
                        "The fx will still warp the result again unless you "
                        "set its points back on the rest ones.")
                .arg(QString::fromStdString(fid.expand()))
                .arg(QString::fromStdWString(level->getName())),
            QObject::tr("Bake"), QObject::tr("Cancel"), 2) != 1)
      return;

    TVectorImageP after(before->clone());
    deformVectorImage(*after, deformer, row);

    BakeEnvelopeUndo *undo =
        new BakeEnvelopeUndo(TXshSimpleLevelP(level), fid, before, after);
    undo->redo();
    TUndoManager::manager()->add(undo);
  }
} bakeEnvelopeCommand;

}  // namespace
