#include "toonz/strokedeformerdata.h"

#include "toonz/tdeformer.h"
#include "toonz/tcolumnfx.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshcell.h"
#include "toonz/txshleveltypes.h"

#include "tfx.h"

#include <algorithm>

//================================================================================================

TStrokeDeformerRenderData::TStrokeDeformerRenderData(
    const std::shared_ptr<const TDeformer> &deformer, const std::string &key)
    : m_deformer(deformer), m_key(key) {}

//------------------------------------------------------------------------------

bool TStrokeDeformerRenderData::operator==(
    const TRasterFxRenderData &data) const {
  const TStrokeDeformerRenderData *other =
      dynamic_cast<const TStrokeDeformerRenderData *>(&data);
  return other && other->m_key == m_key;
}

//------------------------------------------------------------------------------

std::string TStrokeDeformerRenderData::toString() const {
  return "strokeDeformer{" + m_key + "}";
}

//================================================================================================

bool rendersVectorStrokes(TFx *fx, double frame) {
  // A chain longer than this is not one of deformers and column fxs
  for (int depth = 0; fx && depth < 64; ++depth) {
    if (TLevelColumnFx *columnFx = dynamic_cast<TLevelColumnFx *>(fx)) {
      TXshLevelColumn *column = columnFx->getColumn();
      if (!column) return false;
      const TXshCell cell = column->getRemappedCell((int)frame);
      if (cell.isEmpty()) return false;
      TXshSimpleLevel *sl = cell.getSimpleLevel();
      return sl && sl->getType() == PLI_XSHLEVEL;
    }

    // The column's colour fxs (tbasefx.h, told by type: their classes are
    // not exported) keep the space; so do the deformers that pass their
    // deformer down, whose Source is then checked in turn
    const std::string type        = fx->getFxType();
    const TDeformerSource *source = dynamic_cast<const TDeformerSource *>(fx);
    const bool passes = type == "columnColorFilterFx" ||
                        type == "columnColorEffectFx" ||
                        type == "columnDepthFx" ||
                        (source && source->deformsStrokes());
    if (!passes || fx->getInputPortCount() == 0) return false;

    fx = fx->getInputPort(0)->getFx();
  }
  return false;
}

//------------------------------------------------------------------------------

bool hasStrokeDeformers(const std::vector<TRasterFxRenderDataP> &data) {
  for (const TRasterFxRenderDataP &d : data)
    if (dynamic_cast<const TStrokeDeformerRenderData *>(d.getPointer()))
      return true;
  return false;
}

//------------------------------------------------------------------------------

TVectorImageP applyStrokeDeformers(
    const TVectorImageP &vi, const std::vector<TRasterFxRenderDataP> &data,
    double frame) {
  if (!vi || !hasStrokeDeformers(data)) return vi;

  // The fx nearest to the column added its deformer last, and deforms first
  TVectorImageP deformed = vi->clone();
  for (auto it = data.rbegin(); it != data.rend(); ++it)
    if (const TStrokeDeformerRenderData *d =
            dynamic_cast<const TStrokeDeformerRenderData *>(it->getPointer()))
      deformVectorImage(*deformed, d->deformer(), frame);
  return deformed;
}

//------------------------------------------------------------------------------

TRectD strokeDeformedBounds(const TRectD &bbox, double maxThickness,
                            const std::vector<TRasterFxRenderDataP> &data,
                            double frame) {
  TRectD bounds(bbox);
  bool deformed = false;
  for (auto it = data.rbegin(); it != data.rend(); ++it)
    if (const TStrokeDeformerRenderData *d =
            dynamic_cast<const TStrokeDeformerRenderData *>(it->getPointer())) {
      bounds   = d->deformer().deformedBounds(bounds, frame);
      deformed = true;
    }

  if (!deformed || bounds.isEmpty() || bounds == TConsts::infiniteRectD)
    return bounds;
  return bounds.enlarge(std::max(0.0, maxThickness));
}
