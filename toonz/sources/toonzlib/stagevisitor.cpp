

// TnzCore includes
#include "tpixelutils.h"
#include "tstroke.h"
#include "tofflinegl.h"
#include "tstencilcontrol.h"
#include "tvectorgl.h"
#include "tvectorrenderdata.h"
#include "tcolorfunctions.h"
#include "tfxattributes.h"
#include "tpalette.h"
#include "tropcm.h"
#include "trasterimage.h"
#include "tvectorimage.h"
#include "tmeshimage.h"
#include "tcolorstyles.h"
#include "timage_io.h"
#include "tregion.h"
#include "toonz/toonzscene.h"

// TnzBase includes
#include "tenv.h"

// TnzExt includes
#include "ext/meshutils.h"
#include "ext/plasticskeleton.h"
#include "ext/plasticskeletondeformation.h"
#include "ext/plasticdeformerstorage.h"

// TnzLib includes
#include "toonz/stageplayer.h"
#include "toonz/stage.h"
#include "toonz/stage2.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/fxdag.h"
#include "ttzpimagefx.h"
#include "toonz/txsheet.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshcell.h"
#include "toonz/onionskinmask.h"
#include "toonz/dpiscale.h"
#include "toonz/imagemanager.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/glrasterpainter.h"
#include "toonz/preferences.h"
#include "toonz/fill.h"
#include "toonz/levelproperties.h"
#include "toonz/autoclose.h"
#include "toonz/txshleveltypes.h"
#include "imagebuilders.h"
#include "toonz/tframehandle.h"
#include "toonz/preferences.h"

// Qt includes
#include <QImage>
#include <QPainter>
#include <QPolygon>
#include <QThreadStorage>
#include <QTransform>
#include <QThread>
#include <QGuiApplication>
#include <QOpenGLContext>
#include <QMutex>
#include <QMutexLocker>

// STL includes
#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <tuple>

#include "toonz/stagevisitor.h"

//**********************************************************************************************
//    Stage namespace
//**********************************************************************************************

/*! \namespace Stage
    \brief The Stage namespace provides objects, classes and methods useful to
   view or display images.
*/

using namespace Stage;

/*! \var Stage::inch
                For historical reasons camera stand is defined in a coordinate
   system in which
                an inch is equal to 'Stage::inch' unit.
                Pay attention: modify this value condition apparent line
   thickness of
                images .pli.
*/
// const double Stage::inch = 53.33333;

//**********************************************************************************************
//    Local namespace
//**********************************************************************************************

namespace {

double getOnionSkinOpacity(const Stage::Player &player) {
  if (player.m_onionSkinDistance == 0) return 0.9;
  if (player.m_onionSkinOpacity >= 0.0) return player.m_onionSkinOpacity;
  return 1.0 -
         OnionSkinMask::getOnionSkinFade(player.m_onionSkinDistance);
}

//----------------------------------------------------------------

// Returns a copy of the specified palette with the overrides of the Colour
// Override Fxs that follow the player's column, or 0 if there are none. The Fxs
// are looked for like the render does for a column that goes through a chain of
// Fxs: the column must have a single output connection, and the chain stops at
// the first Fx that has more of them, at the Xsheet and at the Fxs that take
// the column in a port other than the first.
TPaletteP overriddenPalette(const Stage::Player &player, TPalette *palette) {
  if (!palette || !player.m_xsh || player.m_column < 0) return TPaletteP();

  TXshColumn *column = player.m_xsh->getColumn(player.m_column);
  TFx *fx            = column ? column->getFx() : 0;
  if (!fx) return TPaletteP();

  TFxSet *terminalFxs = player.m_xsh->getFxDag()->getTerminalFxs();

  // The data of the innermost Fx come first, so that the outer ones win
  ColourOverrideFxRenderData data;
  for (int depth = 0; fx && depth < 64; ++depth) {
    // A terminal Fx is also rendered as it is, in the Xsheet
    if (fx->getOutputConnectionCount() != 1 || terminalFxs->containsFx(fx))
      break;

    TFxPort *port = fx->getOutputConnection(0);
    TFx *next     = port ? port->getOwnerFx() : 0;
    if (!next || dynamic_cast<TXsheetFx *>(next) ||
        next->getInputPortCount() == 0 || next->getInputPort(0) != port)
      break;

    if (next->getAttributes()->isEnabled()) {
      const TColourOverrideSource *source =
          dynamic_cast<const TColourOverrideSource *>(next);
      if (source) source->getColourOverrides(player.m_frame, data);
    }
    fx = next;
  }

  if (data.m_overrides.empty()) return TPaletteP();

  TPaletteP overridden = palette->clone();
  // Bring animated styles to the frame first, so that the setFrame() of the
  // painters does not overwrite the overridden colors
  overridden->setFrame(player.m_frame);
  data.apply(overridden.getPointer());
  return overridden;
}

//----------------------------------------------------------------

// Returns a copy of the specified palette with the outline colour of the
// player as the main color of every style, so that the inks of a Toonz raster
// image are drawn in it (Outline View). The level's own palette is never
// modified.
TPaletteP outlinePalette(const Stage::Player &player, TPalette *palette) {
  if (!palette) return TPaletteP();

  TPaletteP outlined = palette->clone();
  // Bring animated styles to the frame first, so that the setFrame() of the
  // painters does not overwrite the outline colour
  outlined->setFrame(player.m_frame);
  // The style 0 is the transparent one
  for (int i = 1; i < outlined->getStyleCount(); ++i)
    if (TColorStyle *style = outlined->getStyle(i))
      style->setMainColor(player.m_outlineColor);
  return outlined;
}

//----------------------------------------------------------------

QImage rasterToQImage(const TRaster32P &ras) {
  QImage image(ras->getRawData(), ras->getLx(), ras->getLy(),
               QImage::Format_ARGB32_Premultiplied);
  return image;
}

//----------------------------------------------------------------

QImage rasterToQImage(const TRasterGR8P &ras) {
  QImage image(ras->getLx(), ras->getLy(), QImage::Format_ARGB32_Premultiplied);
  int lx = ras->getLx(), ly = ras->getLy();

  for (int y = 0; y < ly; y++) {
    TPixelGR8 *pix    = ras->pixels(y);
    TPixelGR8 *endPix = pix + lx;
    QRgb *outPix      = (QRgb *)image.scanLine(y);
    for (; pix < endPix; ++pix) {
      int value = pix->value;
      *outPix++ = qRgba(value, value, value, 255);
    }
  }
  return image;
}

//----------------------------------------------------------------

QImage rasterToQImage(const TRasterP &ras) {
  if (TRaster32P src32 = ras)
    return rasterToQImage(src32);
  else if (TRasterGR8P srcGr8 = ras)
    return rasterToQImage(srcGr8);

  // assert(!"Cannot use drawImage with this image!");
  return QImage();
}

//----------------------------------------------------------------

//! Draw orthogonal projection of \b bbox onto x-axis and y-axis.
void draw3DShadow(const TRectD &bbox, double z, double phi) {
  // Not the most elegant solution

  double a = bigBoxSize[0];
  double b = bigBoxSize[1];

  glColor3d(0.9, 0.9, 0.86);
  glBegin(GL_LINE_STRIP);
  glVertex3d(bbox.x0, bbox.y0, z);
  glVertex3d(bbox.x0, bbox.y1, z);
  glVertex3d(bbox.x1, bbox.y1, z);
  glVertex3d(bbox.x1, bbox.y0, z);
  glVertex3d(bbox.x0, bbox.y0, z);
  glEnd();

  double y = -b;
  double x = phi >= 0 ? a : -a;

  double xm = 0.5 * (bbox.x0 + bbox.x1);
  double ym = 0.5 * (bbox.y0 + bbox.y1);

  if (bbox.y0 > y) {
    glBegin(GL_LINE_STRIP);
    glVertex3d(xm, y, z);
    glVertex3d(xm, bbox.y0, z);
    glEnd();
  } else if (bbox.y1 < y) {
    glBegin(GL_LINE_STRIP);
    glVertex3d(xm, y, z);
    glVertex3d(xm, bbox.y1, z);
    glEnd();
  }

  if (bbox.x0 > x) {
    glBegin(GL_LINE_STRIP);
    glVertex3d(x, ym, z);
    glVertex3d(bbox.x0, ym, z);
    glEnd();
  } else if (bbox.x1 < x) {
    glBegin(GL_LINE_STRIP);
    glVertex3d(x, ym, z);
    glVertex3d(bbox.x1, ym, z);
    glEnd();
  }

  glColor3d(0.0, 0.0, 0.0);

  glBegin(GL_LINE_STRIP);
  glVertex3d(bbox.x0, -b, z);
  glVertex3d(bbox.x1, -b, z);
  glEnd();

  glBegin(GL_LINE_STRIP);
  glVertex3d(x, bbox.y0, z);
  glVertex3d(x, bbox.y1, z);
  glEnd();
}

//=====================================================================

//  Plastic function declarations

/*!
  Returns from the specified player the stage object to be plastic
  deformed - or 0 if current Toonz rules prevent it from being deformed.
*/
TStageObject *plasticDeformedObj(const Stage::Player &player,
                                 const PlasticVisualSettings &pvs);

//! Draws the specified mesh image
void onMeshImage(TMeshImage *mi, const Stage::Player &player,
                 const ImagePainter::VisualSettings &vs,
                 const TAffine &viewAff);

//! Applies Plastic deformation of the specified player's stage object.
void onPlasticDeformedImage(TStageObject *playerObj,
                            const Stage::Player &player,
                            const ImagePainter::VisualSettings &vs,
                            const TAffine &viewAff);

//----------------------------------------------------------------
// Transparency Check helper
//----------------------------------------------------------------

// High-tone empty pixels (paint = 0) blended with ink, causing dark gray.
// Force tone = 255 so quickPut treats them as fully transparent.
inline void forceHighToneEmptyPixels(TRasterCM32P &ras, int threshold) {
  if (!ras) return;

  ras->lock();
  TPixelCM32 *pix    = ras->pixels();
  TPixelCM32 *endPix = pix + ras->getLx() * ras->getLy();

  for (; pix < endPix; ++pix) {
    if (pix->getPaint() == 0 && pix->getTone() >= threshold) {
      pix->setTone(255);
    }
  }

  ras->unlock();
}

}  // namespace

//**********************************************************************************************
//    Picker  implementation
//**********************************************************************************************

Picker::Picker(const TAffine &viewAff, const TPointD &point,
               const ImagePainter::VisualSettings &vs, int devPixRatio)
    : Visitor(vs)
    , m_viewAff(viewAff)
    , m_point(point)
    , m_columnIndexes()
    , m_minDist2(25.0)
    , m_devPixRatio(devPixRatio) {}

//-----------------------------------------------------------------------------

void Picker::setMinimumDistance(double d) {
  m_minDist2 = (double)(m_devPixRatio * m_devPixRatio) * d * d;
}

//-----------------------------------------------------------------------------

void Picker::onImage(const Stage::Player &player) {
  // if m_currentColumnIndex is other than the default value (-1),
  // then pick only the current column.
  if (m_currentColumnIndex != -1 &&
      m_currentColumnIndex != player.m_ancestorColumnIndex)
    return;

  bool picked   = false;
  TAffine aff   = m_viewAff * player.m_placement;
  TPointD point = aff.inv() * m_point;

  const TImageP &img = player.image();

  if (TVectorImageP vi = img) {
    double w         = 0;
    UINT strokeIndex = 0;
    double dist2     = 0;
    TRegion *r       = vi->getRegion(point);
    int styleId      = 0;
    if (r) styleId = r->getStyle();
    if (styleId != 0)
      picked = true;
    else if (vi->getNearestStroke(point, w, strokeIndex, dist2)) {
      dist2 *= aff.det();
      TStroke *stroke        = vi->getStroke(strokeIndex);
      TThickPoint thickPoint = stroke->getThickPoint(w);
      double len2            = thickPoint.thick * thickPoint.thick * aff.det();
      double checkDist       = std::max(m_minDist2, len2);
      if (dist2 < checkDist) picked = true;
    }
  } else if (TRasterImageP ri = img) {
    TRaster32P ras = ri->getRaster();
    if (!ras) return;

    ras->lock();
    TPointD pp = player.m_dpiAff.inv() * point + ras->getCenterD();
    TPoint p(tround(pp.x), tround(pp.y));
    if (!ras->getBounds().contains(p)) return;

    TPixel32 *pix = ras->pixels(p.y);
    if (pix[p.x].m != 0) picked = true;

    TAffine aff2 = (aff * player.m_dpiAff).inv();

    TPointD pa(p.x, p.y);
    TPointD dx = aff2 * (m_point + TPointD(3, 0)) - aff2 * m_point;
    TPointD dy = aff2 * (m_point + TPointD(0, 3)) - aff2 * m_point;
    double rx  = dx.x * dx.x + dx.y * dx.y;
    double ry  = dy.x * dy.x + dy.y * dy.y;
    int radius = tround(sqrt(rx > ry ? rx : ry));
    TRect rect = TRect(p.x - radius, p.y - radius, p.x + radius, p.y + radius) *
                 ras->getBounds();
    for (int y = rect.y0; !picked && y <= rect.y1; y++) {
      pix = ras->pixels(y);
      for (int x = rect.x0; !picked && x <= rect.x1; x++)
        if (pix[x].m != 0) picked = true;
    }

    ras->unlock();
  } else if (TToonzImageP ti = img) {
    TRasterCM32P ras = ti->getRaster();
    if (!ras) return;

    ras->lock();
    TPointD pp = player.m_dpiAff.inv() * point + ras->getCenterD();
    TPoint p(tround(pp.x), tround(pp.y));
    if (!ras->getBounds().contains(p)) return;

    TPixelCM32 *pix = ras->pixels(p.y) + p.x;
    if (!pix->isPurePaint() || pix->getPaint() != 0) picked = true;

    ras->unlock();
  }

  if (picked) {
    int columnIndex = player.m_ancestorColumnIndex;
    if (m_columnIndexes.empty() || m_columnIndexes.back() != columnIndex)
      m_columnIndexes.push_back(columnIndex);

    int row = player.m_frame;
    if (m_rows.empty() || m_rows.back() != row) m_rows.push_back(row);
  }
}

//-----------------------------------------------------------------------------

void Picker::beginMask() {}

//-----------------------------------------------------------------------------

void Picker::endMask() {}

//-----------------------------------------------------------------------------

void Picker::enableMask() {}

//-----------------------------------------------------------------------------

void Picker::disableMask() {}

//-----------------------------------------------------------------------------

int Picker::getColumnIndex() const {
  if (m_columnIndexes.empty())
    return -1;
  else
    return m_columnIndexes.back();
}

//-----------------------------------------------------------------------------

void Picker::getColumnIndexes(std::vector<int> &indexes) const {
  indexes = m_columnIndexes;
}
//-----------------------------------------------------------------------------

int Picker::getRow() const {
  if (m_rows.empty())
    return -1;
  else
    return m_rows.back();
}

//**********************************************************************************************
//    RasterPainter  implementation
//**********************************************************************************************

RasterPainter::RasterPainter(const TDimension &dim, const TAffine &viewAff,
                             const TRect &rect,
                             const ImagePainter::VisualSettings &vs,
                             bool checkFlags)
    : Visitor(vs)
    , m_dim(dim)
    , m_viewAff(viewAff)
    , m_clipRect(rect)
    , m_maskLevel(0)
    , m_singleColumnEnabled(false)
    , m_checkFlags(checkFlags)
    , m_doRasterDarkenBlendedView(false)
    , m_gpuCompositing(false)
    , m_colourOverrides(false) {}

//-----------------------------------------------------------------------------

//! Used only for TAB Pro
void RasterPainter::beginMask() {
  flushRasterImages();  // to avoid doing it after beginMask
  ++m_maskLevel;
  TStencilControl::instance()->beginMask();
}
//! Used only for TAB Pro
void RasterPainter::endMask() {
  flushRasterImages();  // if there are raster images in the mask, they must
                        // come out now
  --m_maskLevel;
  TStencilControl::instance()->endMask();
}
//! Used only for TAB Pro
void RasterPainter::enableMask() {
  TStencilControl::instance()->enableMask(TStencilControl::SHOW_INSIDE);
}
//! Used only for TAB Pro
void RasterPainter::disableMask() {
  flushRasterImages();  // if there are masked raster images, they must come out
                        // now
  TStencilControl::instance()->disableMask();
}

int RasterPainter::getNodesCount() { return m_nodes.size(); }

//-----------------------------------------------------------------------------

void RasterPainter::clearNodes() { m_nodes.clear(); }

//-----------------------------------------------------------------------------

TRasterP RasterPainter::getRaster(int index, QTransform &matrix) {
  if ((int)m_nodes.size() <= index) return TRasterP();

  if (m_nodes[index].m_onionMode != Node::eOnionSkinNone) return TRasterP();

  if (m_nodes.empty()) return TRasterP();

  double delta = sqrt(fabs(m_nodes[0].m_aff.det()));
  TRectD bbox  = m_nodes[0].m_bbox.enlarge(delta);

  int i;
  for (i = 1; i < (int)m_nodes.size(); i++) {
    delta = sqrt(fabs(m_nodes[i].m_aff.det()));
    bbox += m_nodes[i].m_bbox.enlarge(delta);
  }
  TRect rect(tfloor(bbox.x0), tfloor(bbox.y0), tceil(bbox.x1), tceil(bbox.y1));
  rect = rect * TRect(0, 0, m_dim.lx - 1, m_dim.ly - 1);

  TAffine aff = TTranslation(-rect.x0, -rect.y0) * m_nodes[index].m_aff;
  matrix = QTransform(aff.a11, aff.a21, aff.a12, aff.a22, aff.a13, aff.a23);

  return m_nodes[index].m_raster;
}

//-----------------------------------------------------------------------------

/*! Make frame visualization.
\n If onion-skin is active, create a new raster with dimension containing
all frame with onion-skin; recall \b TRop::quickPut with argument
each frame with onion-skin and new raster. If onion-skin is not active
recall \b TRop::quickPut with argument current frame and new raster.
*/

namespace {
QThreadStorage<std::vector<char> *> threadBuffers;
}

void RasterPainter::flushRasterImages() {
  // With GPU compositing off, free the textures cached for the current context
  // by previous GPU flushes (with the rasters they keep alive), and their
  // pending invalidations, which are recorded only while a cache exists
  if (!m_gpuCompositing) releaseGpuTextures();

  if (m_nodes.empty()) return;

  // The experimental GPU path returns false when it did not draw anything
  if (m_gpuCompositing && flushRasterImagesGpu()) return;

  flushRasterImagesCpu(true);
}

//-----------------------------------------------------------------------------

void RasterPainter::flushRasterImagesCpu(bool drawBBox) {
  if (m_nodes.empty()) return;

  // Build nodes bbox union
  double delta = sqrt(fabs(m_nodes[0].m_aff.det()));
  TRectD bbox  = m_nodes[0].m_bbox.enlarge(delta);

  int i, nodesCount = m_nodes.size();
  for (i = 1; i < nodesCount; ++i) {
    delta = sqrt(fabs(m_nodes[i].m_aff.det()));
    bbox += m_nodes[i].m_bbox.enlarge(delta);
  }

  TRect rect(tfloor(bbox.x0), tfloor(bbox.y0), tceil(bbox.x1), tceil(bbox.y1));
  rect = rect * TRect(0, 0, m_dim.lx - 1, m_dim.ly - 1);

  int lx = rect.getLx(), ly = rect.getLy();
  TDimension dim(lx, ly);

  // this is needed since a stop motion live view doesn't register
  // as a node correctly there is probably a better way to do this.
  if (rect.getLx() == 0 && lx == 0) {
    rect = m_clipRect;
    dim  = m_dim;
  }

  // Build a raster buffer of sufficient size to hold said union.
  // The buffer is per-thread cached in order to improve the rendering speed.
  if (!threadBuffers.hasLocalData())
    threadBuffers.setLocalData(new std::vector<char>());

  int size = dim.lx * dim.ly * sizeof(TPixel32);

  std::vector<char> *vbuff = (std::vector<char> *)threadBuffers.localData();
  if (size > (int)vbuff->size()) vbuff->resize(size);

  TRaster32P ras(dim.lx, dim.ly, dim.lx, (TPixel32 *)&(*vbuff)[0]);
  TRaster32P ras2;

  if (m_vs.m_colorMask != 0) {
    ras2 = TRaster32P(ras->getSize());
    ras->clear();
  } else
    ras2 = ras;

  // Clear the buffer - it will hold all the stacked nodes content to be overed
  // on top of the OpenGL buffer through a glDrawPixel()
  ras->lock();
  ras->clear();  // ras is typically reused and we need it transparent first

  TRect r                 = rect - rect.getP00();
  TRaster32P viewedRaster = ras->extract(r);

  int current = -1;

  // Retrieve preferences-related data
  int tc    = m_checkFlags ? ToonzCheck::instance()->getChecks() : 0;
  int index = ToonzCheck::instance()->getColorIndex();

  TPixel32 frontOnionColor, backOnionColor;
  bool onionInksOnly;

  Preferences::instance()->getOnionData(frontOnionColor, backOnionColor,
                                        onionInksOnly);

  // Stack every node on top of the raster buffer
  for (i = 0; i < nodesCount; ++i) {
    if (m_nodes[i].m_isCurrentColumn) current = i;

    TAffine aff         = TTranslation(-rect.x0, -rect.y0) * m_nodes[i].m_aff;
    TDimension imageDim = m_nodes[i].m_raster->getSize();
    TPointD offset(0.5, 0.5);
    aff *= TTranslation(offset);  // very quick and very dirty fix: in
                                  // camerastand the images seems shifted of an
                                  // half pixel...it's a quickput approximation?

    TPixel32 colorscale = TPixel32(0, 0, 0, m_nodes[i].m_alpha);
    int inksOnly;

    if (m_nodes[i].m_onionMode != Node::eOnionSkinNone) {
      inksOnly = onionInksOnly;

      if (m_nodes[i].m_onionMode == Node::eOnionSkinFront)
        colorscale = TPixel32(frontOnionColor.r, frontOnionColor.g,
                              frontOnionColor.b, m_nodes[i].m_alpha);
      else if (m_nodes[i].m_onionMode == Node::eOnionSkinBack)
        colorscale = TPixel32(backOnionColor.r, backOnionColor.g,
                              backOnionColor.b, m_nodes[i].m_alpha);
    } else {
      if (m_nodes[i].m_filterColor != TPixel32::Black) {
        colorscale   = m_nodes[i].m_filterColor;
        colorscale.m = (typename TPixel32::Channel)((int)colorscale.m *
                                                    (int)m_nodes[i].m_alpha /
                                                    TPixel32::maxChannelValue);
      }
      inksOnly = tc & ToonzCheck::eInksOnly;
    }
    // Outline View shows the inks only
    if (m_nodes[i].m_outlineView) inksOnly = true;

    if (TRaster32P src32 = m_nodes[i].m_raster)
      TRop::quickPut(viewedRaster, src32, aff, colorscale,
                     m_nodes[i].m_doPremultiply, m_nodes[i].m_whiteTransp,
                     m_nodes[i].m_isFirstColumn, m_doRasterDarkenBlendedView);
    else if (TRasterGR8P srcGr8 = m_nodes[i].m_raster)
      TRop::quickPut(viewedRaster, srcGr8, aff, colorscale);
    else if (TRasterCM32P srcCm = m_nodes[i].m_raster) {
      assert(m_nodes[i].m_palette);
      int oldframe = m_nodes[i].m_palette->getFrame();
      m_nodes[i].m_palette->setFrame(m_nodes[i].m_frame);

      TPaletteP plt;
      int gapCheckIndex = -1;
      TRasterCM32P aux;
      if ((tc & ToonzCheck::eGap || tc & ToonzCheck::eAutoclose) &&
          m_nodes[i].m_isCurrentColumn) {
        aux           = srcCm->clone();
        srcCm         = Preferences::instance()->getFillOnlySavebox()
                            ? (TRasterCM32P)aux->extract(m_nodes[i].m_savebox)
                            : aux;
        plt           = m_nodes[i].m_palette->clone();
        gapCheckIndex = plt->addStyle(TPixel::Magenta);
        if (srcCm && srcCm->getLx() > 0 && srcCm->getLy() > 0) {
          if (tc & ToonzCheck::eGap)
            AreaFiller(srcCm).rectFastFill(m_nodes[i].m_savebox, 1);
        }
        if (tc & ToonzCheck::eAutoclose &&
            m_nodes[i].m_onionMode == Node::eOnionSkinNone) {
          auto autocloseSettings =
              ToonzCheck::instance()->getAutocloseSettings();
          std::set<int> autoPaints;
          if (autocloseSettings.m_ignoreAPInks) {
            for (int styleIdx = 0; styleIdx < plt->getStyleCount(); ++styleIdx)
              if (plt->getStyle(styleIdx)->getFlags() != 0)
                autoPaints.insert(styleIdx);
          }
          TAutocloser ac(srcCm, gapCheckIndex, autocloseSettings,
                         std::move(autoPaints));

          if (srcCm && srcCm->getLx() > 0 && srcCm->getLy() > 0) {
            bool hasContent = false;
            srcCm->lock();
            TPixelCM32 *pix = srcCm->pixels();
            TPixelCM32 *end = pix + srcCm->getLx() * srcCm->getLy();
            for (; pix < end; ++pix) {
              if (pix->getInk() != 0 || pix->getPaint() != 0) {
                hasContent = true;
                break;
              }
            }
            srcCm->unlock();
            if (hasContent) {
              if (ac.hasSegmentCache(m_currentImageId))
                ac.draw(ac.getSegmentCache(m_currentImageId));
              else
                ac.exec(m_currentImageId);
            }
          }
          if (tc & ToonzCheck::eGap) {
            int gapFillIndex = plt->addStyle(TPixelRGBM32(244, 186, 148, 0xff));
            plt->getStyle(gapFillIndex)->setFlags(1);
            if (srcCm && srcCm->getLx() > 0 && srcCm->getLy() > 0) {
              AreaFiller(srcCm).rectFastFill(m_nodes[i].m_savebox,
                                             gapFillIndex);
            }
          }
        }
        srcCm = aux;
      } else {
        plt = m_nodes[i].m_palette;
      }

      if (tc == 0 || tc == ToonzCheck::eBlackBg ||
          !m_nodes[i].m_isCurrentColumn) {
        TRop::quickPut(viewedRaster, srcCm, plt, aff, colorscale, inksOnly);
      } else {
        TRop::CmappedQuickputSettings settings;

        settings.m_globalColorScale = colorscale;
        settings.m_inksOnly         = inksOnly;
        settings.m_transparencyCheck =
            tc & (ToonzCheck::eTransparency | ToonzCheck::eGap);
        settings.m_blackBgCheck = tc & ToonzCheck::eBlackBg;

        settings.m_inkIndex = m_nodes[i].m_isCurrentColumn ? index : -1;

        settings.m_paintIndex = m_nodes[i].m_isCurrentColumn
                                    ? (tc & ToonzCheck::ePaint ? index : -1)
                                    : -1;

        settings.m_inkCheckEnabled   = (tc & ToonzCheck::eInk) != 0;
        settings.m_ink1CheckEnabled  = (tc & ToonzCheck::eInk1) != 0;
        settings.m_paintCheckEnabled = (tc & ToonzCheck::ePaint) != 0;

        Preferences::instance()->getTranspCheckData(
            settings.m_transpCheckBg, settings.m_transpCheckInk,
            settings.m_transpCheckPaint);

        settings.m_isOnionSkin = m_nodes[i].m_onionMode != Node::eOnionSkinNone;
        settings.m_gapCheckIndex = gapCheckIndex;

        settings.m_inkCheckColor = Preferences::instance()->getInkCheckColor();
        settings.m_ink1CheckColor =
            Preferences::instance()->getInk1CheckColor();
        settings.m_paintCheckColor =
            Preferences::instance()->getPaintCheckColor();

        // ==============================================================
        // Transparency Check: (see helper above)
        // Apply high-tone empty pixel fix
        // When "Inks Only" check is ON, the high-tone modification will not be
        // applied
        // ==============================================================
        TRasterCM32P rasterToUse = srcCm;

        if (settings.m_transparencyCheck && !settings.m_inksOnly) {
          const int threshold = 80;  // Minimum tone for "false" high antialias

          // Clone and adjust high-tone empty pixels
          rasterToUse = srcCm->clone();
          forceHighToneEmptyPixels(rasterToUse, threshold);
        }

        // Final render using original OpenToonz quickPut behavior
        TRop::quickPut(viewedRaster, rasterToUse, plt, aff, settings);
        // Temporary clone automatically destroyed when leaving scope
      }

      srcCm = TRasterCM32P();
      plt   = TPaletteP();

      m_nodes[i].m_palette->setFrame(oldframe);
    } else {
      assert(!"Cannot use quickput with this raster combination!");
    }
  }
  if (m_vs.m_colorMask != 0) {
    TRop::setChannel(ras, ras, m_vs.m_colorMask, false);
    TRop::quickPut(ras2, ras, TAffine());
  }

  // Now, output the raster buffer on top of the OpenGL buffer
  glPushAttrib(GL_COLOR_BUFFER_BIT);  // Preserve blending and stuff

  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE,
              GL_ONE_MINUS_SRC_ALPHA);  // The raster buffer is intended in
  // premultiplied form thus the GL_ONE on src
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_DITHER);
  glDisable(GL_LOGIC_OP);

  /* disable, since these features are never enabled, and cause OpenGL to assert
   * on systems that don't support them: see #591 */
#if 0
#ifdef GL_EXT_convolution
  if( GLEW_EXT_convolution ) {
    glDisable(GL_CONVOLUTION_1D_EXT);
    glDisable(GL_CONVOLUTION_2D_EXT);
    glDisable(GL_SEPARABLE_2D_EXT);
  }
#endif

#ifdef GL_EXT_histogram
  if( GLEW_EXT_histogram ) {
    glDisable(GL_HISTOGRAM_EXT);
    glDisable(GL_MINMAX_EXT);
  }
#endif
#endif

#ifdef GL_EXT_texture3D
  if (GL_EXT_texture3D) {
    glDisable(GL_TEXTURE_3D_EXT);
  }
#endif

  glPushMatrix();
  glLoadIdentity();

  glRasterPos2d(rect.x0, rect.y0);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  glDrawPixels(ras2->getLx(), ras2->getLy(), TGL_FMT, TGL_TYPE,
               ras2->getRawData());

  ras->unlock();
  glPopMatrix();

  glPopAttrib();  // Restore blending status

  if (drawBBox && m_vs.m_showBBox && current > -1) {
    glPushMatrix();
    glLoadIdentity();
    tglColor(TPixel(200, 200, 200));
    tglMultMatrix(m_nodes[current].m_aff);
    tglDrawRect(TRect(0, 0, m_nodes[current].m_raster->getLx(),
                      m_nodes[current].m_raster->getLy()));
    glPopMatrix();
  }

  m_nodes.clear();
}

//-----------------------------------------------------------------------------
//    Experimental GPU compositing
//-----------------------------------------------------------------------------

/*
  When enabled (Preferences: "GPU Compositing in the Viewer"), raster nodes are
  drawn as textured quads blended with GL_ONE, GL_ONE_MINUS_SRC_ALPHA directly
  on the viewer's framebuffer. Since the premultiplied "over" is associative,
  this gives the same result as stacking the nodes in a RAM buffer and drawing
  it with glDrawPixels.

  Each texture is "baked" with the very same TRop::quickPut call used by the
  CPU path, using an identity affine on a transparent raster: this applies
  palette, inks-only, color filter / onion skin color, premultiply, white
  transparency and ignore-alpha exactly like the CPU path does. Only the node
  opacity is applied at draw time, through glColor4 modulation. Textures are
  sampled with GL_NEAREST and a half pixel offset, which replicates the
  closest-pixel sampling of quickPut.

  Nodes that cannot be cached safely or reproduced exactly are composited by
  the unchanged CPU code, in runs that preserve the stacking order.
*/

namespace {

// Maximum amount of texture memory kept by the cache of each OpenGL context
const qint64 c_gpuTextureBudget = qint64(512) << 20;

// Maximum size of a texture tile
const int c_gpuMaxTileSize = 4096;

enum GpuTextureFlags {
  eGpuRaster32    = 0x1,
  eGpuPremultiply = 0x2,
  eGpuWhiteTransp = 0x4,
  eGpuIgnoreAlpha = 0x8,
  eGpuRasterCM32  = 0x10,
  eGpuInksOnly    = 0x20,
  eGpuColorScaled = 0x40,
  eGpuOutline     = 0x80  // Outline View
};

struct GpuTextureKey {
  const TRaster *m_raster;  //!< Source raster (kept alive by the texture)
  int m_flags;              //!< GpuTextureFlags used to bake the texture
  unsigned int m_color;     //!< Baked color scale (rgb only)
  unsigned long long m_paletteHash;  //!< Palette colors (cm32 only)
  int m_x0, m_y0, m_x1, m_y1;        //!< Baked box of the source raster

  bool operator<(const GpuTextureKey &other) const {
    return std::tie(m_raster, m_flags, m_color, m_paletteHash, m_x0, m_y0, m_x1,
                    m_y1) < std::tie(other.m_raster, other.m_flags,
                                     other.m_color, other.m_paletteHash,
                                     other.m_x0, other.m_y0, other.m_x1,
                                     other.m_y1);
  }
};

struct GpuTexture {
  struct Tile {
    GLuint m_texId;
    TRect m_rect;  //!< Tile rect, relative to m_box
  };

  TRasterP m_raster;  //!< Keeps the source alive, so its address is not reused
  std::string m_imageId;  //!< Base image id, used for invalidation
  TRect m_box;            //!< Baked box of the source raster
  std::vector<Tile> m_tiles;
  qint64 m_bytes         = 0;
  unsigned int m_lastUse = 0;
};

typedef std::map<GpuTextureKey, GpuTexture> GpuTextures;

struct GpuTextureCache {
  GpuTextures m_textures;
  std::set<std::string> m_invalidIds;  //!< Guarded by gpuCacheMutex()
  qint64 m_bytes       = 0;
  unsigned int m_clock = 0;
  int m_maxTileSize    = 0;
};

// NOTE: Intentionally leaked, as images may be invalidated during the static
// objects destruction.
QMutex &gpuCacheMutex() {
  static QMutex *mutex = new QMutex;
  return *mutex;
}

// Caches by OpenGL context. Guarded by gpuCacheMutex(); the cache contents
// are only accessed from the main thread, except for the invalidation sets.
std::map<QOpenGLContext *, GpuTextureCache *> &gpuCaches() {
  static auto *caches = new std::map<QOpenGLContext *, GpuTextureCache *>;
  return *caches;
}

//-----------------------------------------------------------------------------

void releaseGpuTexture(GpuTexture &tex) {
  for (const GpuTexture::Tile &tile : tex.m_tiles)
    glDeleteTextures(1, &tile.m_texId);
  tex.m_tiles.clear();
}

//-----------------------------------------------------------------------------

GpuTextures::iterator eraseGpuTexture(GpuTextureCache &cache,
                                      GpuTextures::iterator it) {
  releaseGpuTexture(it->second);
  cache.m_bytes -= it->second.m_bytes;
  return cache.m_textures.erase(it);
}

//-----------------------------------------------------------------------------

//! Removes the cache of the specified context. Its textures are deleted only
//! if the context is current - otherwise they go away with the context.
void dropGpuTextureCache(QOpenGLContext *context) {
  GpuTextureCache *cache = 0;
  {
    QMutexLocker locker(&gpuCacheMutex());
    auto it = gpuCaches().find(context);
    if (it == gpuCaches().end()) return;
    cache = it->second;
    gpuCaches().erase(it);
  }

  if (QOpenGLContext::currentContext() == context)
    for (auto &pair : cache->m_textures) releaseGpuTexture(pair.second);
  delete cache;
}

//-----------------------------------------------------------------------------

//! Returns the cache of the current context, or 0 if GPU compositing is not
//! supported there.
GpuTextureCache *currentGpuTextureCache(QOpenGLContext *context) {
  {
    QMutexLocker locker(&gpuCacheMutex());
    auto it = gpuCaches().find(context);
    if (it != gpuCaches().end())
      return (it->second->m_maxTileSize > 0) ? it->second : 0;
  }

  GpuTextureCache *cache = new GpuTextureCache;

  // Non power-of-2 textures are required (OpenGL 2.0)
  const char *version = (const char *)glGetString(GL_VERSION);
  if (version && std::atoi(version) >= 2) {
    GLint maxTexSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexSize);
    cache->m_maxTileSize = std::min((int)maxTexSize, c_gpuMaxTileSize);
  } else
    cache->m_maxTileSize = -1;

  {
    QMutexLocker locker(&gpuCacheMutex());
    gpuCaches()[context] = cache;
  }

  // Forget the cache when the context dies, so that a new context allocated
  // at the same address does not inherit it. The owner of the context should
  // release the textures first through RasterPainter::releaseGpuTextures().
  QObject::connect(context, &QOpenGLContext::aboutToBeDestroyed,
                   [context]() { dropGpuTextureCache(context); });

  return (cache->m_maxTileSize > 0) ? cache : 0;
}

//-----------------------------------------------------------------------------

//! Whether the player's image may be cached. The current frame of the current
//! column may be modified in place by tools, without notifications, so it is
//! never cached. Images without a level are never cached either.
bool isGpuCacheable(const Stage::Player &player) {
  if (!player.m_sl) return false;
  return !(player.m_isCurrentColumn &&
           (player.m_onionSkinDistance == c_noOnionSkin ||
            player.m_onionSkinDistance == 0));
}

//-----------------------------------------------------------------------------

unsigned long long hashPalette(TPalette *palette, int frame) {
  int oldFrame = palette->getFrame();
  palette->setFrame(frame);

  // FNV-1a over the colors used by the colormap quickPut
  unsigned long long hash = 14695981039346656037ULL;
  auto add                = [&hash](unsigned int value) {
    hash = (hash ^ value) * 1099511628211ULL;
  };

  int stylesCount = palette->getStyleCount();
  add(stylesCount);
  for (int s = 0; s < stylesCount; ++s) {
    TPixel32 color = palette->getStyle(s)->getAverageColor();
    add(color.r), add(color.g), add(color.b), add(color.m);
  }

  palette->setFrame(oldFrame);
  return hash;
}

//-----------------------------------------------------------------------------

//! Builds the premultiplied texture image of a node through the same quickPut
//! calls of the CPU path, with an identity affine on a transparent raster.
TRaster32P bakeGpuRaster(const TRasterP &ras, TRect box, TPalette *palette,
                         int frame, const TPixel32 &colorScale, int flags) {
  TRaster32P out(box.getLx(), box.getLy());
  out->clear();

  if (TRaster32P src32 = ras)
    TRop::quickPut(out, src32->extract(box), TAffine(), colorScale,
                   flags & eGpuPremultiply, flags & eGpuWhiteTransp,
                   flags & eGpuIgnoreAlpha, false);
  else if (TRasterCM32P srcCm = ras) {
    int oldFrame = palette->getFrame();
    palette->setFrame(frame);

    TPaletteP plt(palette);
    TRasterCM32P srcBox = srcCm->extract(box);
    TRop::quickPut(out, srcBox, plt, TAffine(), colorScale,
                   flags & eGpuInksOnly);

    palette->setFrame(oldFrame);
  }

  return out;
}

//-----------------------------------------------------------------------------

bool uploadGpuTexture(GpuTexture &tex, const TRaster32P &ras, int maxTileSize) {
  glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
  glPushAttrib(GL_TEXTURE_BIT);

  glPixelStorei(GL_UNPACK_ROW_LENGTH, ras->getWrap());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

  // Discard previous errors, so that only ours are checked below
  while (glGetError() != GL_NO_ERROR) {
  }

  ras->lock();

  int lx = ras->getLx(), ly = ras->getLy();
  for (int y = 0; y < ly; y += maxTileSize) {
    for (int x = 0; x < lx; x += maxTileSize) {
      int w = std::min(maxTileSize, lx - x), h = std::min(maxTileSize, ly - y);

      GpuTexture::Tile tile;
      glGenTextures(1, &tile.m_texId);
      glBindTexture(GL_TEXTURE_2D, tile.m_texId);

      // Closest pixel sampling, like TRop::quickPut
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, TGL_FMT, TGL_TYPE,
                   ras->pixels(y) + x);

      tile.m_rect = TRect(x, y, x + w - 1, y + h - 1);
      tex.m_tiles.push_back(tile);
    }
  }

  ras->unlock();

  bool ok = (glGetError() == GL_NO_ERROR);

  glPopAttrib();
  glPopClientAttrib();

  if (!ok) releaseGpuTexture(tex);
  return ok;
}

}  // namespace

//-----------------------------------------------------------------------------

void RasterPainter::invalidateGpuTextures(const std::string &imageId) {
  QMutexLocker locker(&gpuCacheMutex());
  for (auto &pair : gpuCaches())
    if (pair.second->m_maxTileSize > 0)
      pair.second->m_invalidIds.insert(imageId);
}

//-----------------------------------------------------------------------------

void RasterPainter::releaseGpuTextures() {
  if (QOpenGLContext *context = QOpenGLContext::currentContext())
    dropGpuTextureCache(context);
}

//-----------------------------------------------------------------------------

/*! Experimental GPU composition of the stacked nodes. Returns false, without
    drawing anything, when the whole stack must be composited by the CPU.

    Always composited by the CPU:
    - the whole stack, with darken blended view, a channel (color mask) view,
      while drawing a mask, or when the OpenGL context is not suitable;
    - the current frame of the current column (it may be edited in place by
      tools) and any node sharing its raster, nodes without a level (e.g. stop
      motion live view), rasters other than 32-bit and colormap ones (e.g.
      grayscale);
    - colormap nodes of the current column when Toonz checks (transparency,
      ink, paint, gap, autoclose...) apply to them.
*/
bool RasterPainter::flushRasterImagesGpu() {
  if (m_maskLevel > 0 || m_vs.m_colorMask != 0 || m_doRasterDarkenBlendedView)
    return false;

  if (QThread::currentThread() != qGuiApp->thread()) return false;

  QOpenGLContext *context = QOpenGLContext::currentContext();
  if (!context) return false;

  int i, nodesCount = m_nodes.size();

  // Build the output rect exactly as the CPU path does
  double delta = sqrt(fabs(m_nodes[0].m_aff.det()));
  TRectD bbox  = m_nodes[0].m_bbox.enlarge(delta);
  for (i = 1; i < nodesCount; ++i) {
    delta = sqrt(fabs(m_nodes[i].m_aff.det()));
    bbox += m_nodes[i].m_bbox.enlarge(delta);
  }

  TRect rect(tfloor(bbox.x0), tfloor(bbox.y0), tceil(bbox.x1), tceil(bbox.y1));
  rect = rect * TRect(0, 0, m_dim.lx - 1, m_dim.ly - 1);
  if (rect.isEmpty()) return false;  // CPU-only special cases

  GpuTextureCache *cache = currentGpuTextureCache(context);
  if (!cache) return false;

  // Release invalidated and orphaned textures. A texture is orphaned when it
  // holds the only reference to its source raster, which then cannot be
  // displayed anymore.
  std::set<std::string> invalidIds;
  {
    QMutexLocker locker(&gpuCacheMutex());
    invalidIds.swap(cache->m_invalidIds);
  }

  for (auto it = cache->m_textures.begin(); it != cache->m_textures.end();) {
    if (it->second.m_raster->getRefCount() <= 1 ||
        invalidIds.count(it->second.m_imageId))
      it = eraseGpuTexture(*cache, it);
    else
      ++it;
  }

  unsigned int clock = ++cache->m_clock;

  // Rasters of uncacheable nodes may change in place - so any node sharing
  // them must not be cached either
  std::set<const TRaster *> volatileRasters;
  for (i = 0; i < nodesCount; ++i)
    if (!m_nodes[i].m_gpuCacheable)
      volatileRasters.insert(m_nodes[i].m_raster.getPointer());

  // Retrieve preferences-related data, as in the CPU path
  int tc = m_checkFlags ? ToonzCheck::instance()->getChecks() : 0;

  TPixel32 frontOnionColor, backOnionColor;
  bool onionInksOnly;
  Preferences::instance()->getOnionData(frontOnionColor, backOnionColor,
                                        onionInksOnly);

  // Retrieve (or build) the texture of each node that can be drawn on GPU
  std::vector<const GpuTexture *> textures(nodesCount, 0);
  std::vector<double> opacities(nodesCount, 1.0), colorFactors(nodesCount, 1.0);
  int gpuCount = 0;

  for (i = 0; i < nodesCount; ++i) {
    const Node &node = m_nodes[i];
    if (!node.m_gpuCacheable ||
        volatileRasters.count(node.m_raster.getPointer()))
      continue;

    // Color scale, as in the CPU path
    TPixel32 colorscale = TPixel32(0, 0, 0, node.m_alpha);
    bool inksOnly;

    if (node.m_onionMode != Node::eOnionSkinNone) {
      inksOnly = onionInksOnly;

      if (node.m_onionMode == Node::eOnionSkinFront)
        colorscale = TPixel32(frontOnionColor.r, frontOnionColor.g,
                              frontOnionColor.b, node.m_alpha);
      else if (node.m_onionMode == Node::eOnionSkinBack)
        colorscale = TPixel32(backOnionColor.r, backOnionColor.g,
                              backOnionColor.b, node.m_alpha);
    } else {
      if (node.m_filterColor != TPixel32::Black) {
        colorscale   = node.m_filterColor;
        colorscale.m = (typename TPixel32::Channel)(
            (int)colorscale.m * (int)node.m_alpha / TPixel32::maxChannelValue);
      }
      inksOnly = tc & ToonzCheck::eInksOnly;
    }
    // Outline View shows the inks only
    if (node.m_outlineView) inksOnly = true;

    // The color part of the scale is baked, the matte is applied when drawing
    TPixel32 bakeScale(colorscale.r, colorscale.g, colorscale.b, 255);
    bool colorScaled = (bakeScale != TPixel32::Black);

    GpuTextureKey key;
    key.m_raster      = node.m_raster.getPointer();
    key.m_flags       = colorScaled ? eGpuColorScaled : 0;
    key.m_color       = (bakeScale.r << 16) | (bakeScale.g << 8) | bakeScale.b;
    key.m_paletteHash = 0;

    TRect box = node.m_raster->getBounds();

    if (TRaster32P(node.m_raster)) {
      key.m_flags |= eGpuRaster32;
      if (node.m_doPremultiply) key.m_flags |= eGpuPremultiply;
      if (node.m_whiteTransp) key.m_flags |= eGpuWhiteTransp;
      if (node.m_isFirstColumn) key.m_flags |= eGpuIgnoreAlpha;
    } else if (TRasterCM32P(node.m_raster) && node.m_palette) {
      // Toonz checks on the current column need the CPU path
      if (tc != 0 && tc != ToonzCheck::eBlackBg && node.m_isCurrentColumn)
        continue;

      key.m_flags |= eGpuRasterCM32;
      if (inksOnly) key.m_flags |= eGpuInksOnly;
      if (node.m_outlineView) key.m_flags |= eGpuOutline;
      key.m_paletteHash = hashPalette(node.m_palette, node.m_frame);

      // Colormap pixels outside the savebox are transparent
      box *= node.m_savebox;
    } else
      continue;

    if (box.isEmpty()) continue;

    key.m_x0 = box.x0, key.m_y0 = box.y0, key.m_x1 = box.x1, key.m_y1 = box.y1;

    GpuTextures::iterator it = cache->m_textures.find(key);
    if (it == cache->m_textures.end()) {
      // Release the textures of other, currently unused variants of the same
      // raster (e.g. after a palette or a color filter change)
      for (auto jt = cache->m_textures.begin();
           jt != cache->m_textures.end();) {
        if (jt->first.m_raster == key.m_raster && jt->second.m_lastUse != clock)
          jt = eraseGpuTexture(*cache, jt);
        else
          ++jt;
      }

      TRaster32P baked = bakeGpuRaster(node.m_raster, box, node.m_palette,
                                       node.m_frame, bakeScale, key.m_flags);

      GpuTexture tex;
      if (!uploadGpuTexture(tex, baked, cache->m_maxTileSize)) continue;

      tex.m_raster  = node.m_raster;
      tex.m_imageId = node.m_imageId;
      tex.m_box     = box;
      tex.m_bytes   = qint64(box.getLx()) * box.getLy() * sizeof(TPixel32);

      it = cache->m_textures.insert(std::make_pair(key, tex)).first;
      cache->m_bytes += tex.m_bytes;
    }

    it->second.m_lastUse = clock;
    textures[i]          = &it->second;

    // quickPut scales premultiplied pixels by the matte of the color scale.
    // When premultiplying a color-scaled pixel, it applies it twice.
    double opacity  = colorscale.m / 255.0;
    opacities[i]    = opacity;
    colorFactors[i] = ((key.m_flags & eGpuPremultiply) && colorScaled)
                          ? opacity * opacity
                          : opacity;
    ++gpuCount;
  }

  if (gpuCount == 0) return false;

  // Clip to the same rect the CPU path draws in
  TRect scissorRect = rect;
  if (glIsEnabled(GL_SCISSOR_TEST)) {
    GLint box[4];
    glGetIntegerv(GL_SCISSOR_BOX, box);
    scissorRect *=
        TRect(box[0], box[1], box[0] + box[2] - 1, box[1] + box[3] - 1);
  }

  // Draw the nodes in stacking order, alternating GPU and CPU runs
  std::vector<Node> nodes;
  nodes.swap(m_nodes);

  int current = -1;
  for (i = 0; i < nodesCount; ++i)
    if (nodes[i].m_isCurrentColumn) current = i;

  for (int begin = 0; begin < nodesCount;) {
    bool gpuRun = (textures[begin] != 0);

    int end = begin + 1;
    while (end < nodesCount && (textures[end] != 0) == gpuRun) ++end;

    if (!gpuRun) {
      m_nodes.assign(nodes.begin() + begin, nodes.begin() + end);
      flushRasterImagesCpu(false);  // clears m_nodes
    } else if (!scissorRect.isEmpty()) {
      glPushAttrib(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT | GL_TEXTURE_BIT |
                   GL_CURRENT_BIT | GL_SCISSOR_BIT);

      glEnable(GL_BLEND);
      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // Premultiplied textures
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_DITHER);
      glDisable(GL_LOGIC_OP);

      glEnable(GL_SCISSOR_TEST);
      glScissor(scissorRect.x0, scissorRect.y0, scissorRect.getLx(),
                scissorRect.getLy());

      glEnable(GL_TEXTURE_2D);
      glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

      glPushMatrix();

      for (i = begin; i < end; ++i) {
        const GpuTexture *tex = textures[i];

        double k = colorFactors[i];
        glColor4d(k, k, k, opacities[i]);

        // The half pixel shift makes GL_NEAREST pick, for each output pixel,
        // the same source pixel picked by quickPut in the CPU path
        glLoadIdentity();
        tglMultMatrix(TTranslation(0.5, 0.5) * nodes[i].m_aff);

        for (const GpuTexture::Tile &tile : tex->m_tiles) {
          double x0 = tex->m_box.x0 + tile.m_rect.x0,
                 y0 = tex->m_box.y0 + tile.m_rect.y0,
                 x1 = tex->m_box.x0 + tile.m_rect.x1 + 1,
                 y1 = tex->m_box.y0 + tile.m_rect.y1 + 1;

          glBindTexture(GL_TEXTURE_2D, tile.m_texId);

          glBegin(GL_QUADS);
          glTexCoord2d(0.0, 0.0), glVertex2d(x0, y0);
          glTexCoord2d(1.0, 0.0), glVertex2d(x1, y0);
          glTexCoord2d(1.0, 1.0), glVertex2d(x1, y1);
          glTexCoord2d(0.0, 1.0), glVertex2d(x0, y1);
          glEnd();
        }
      }

      glPopMatrix();
      glPopAttrib();
    }

    begin = end;
  }

  if (m_vs.m_showBBox && current > -1) {
    glPushMatrix();
    glLoadIdentity();
    tglColor(TPixel(200, 200, 200));
    tglMultMatrix(nodes[current].m_aff);
    tglDrawRect(TRect(0, 0, nodes[current].m_raster->getLx(),
                      nodes[current].m_raster->getLy()));
    glPopMatrix();
  }

  m_nodes.clear();

  // Keep the cache within its budget, releasing the least recently used
  // textures first
  if (cache->m_bytes > c_gpuTextureBudget) {
    std::vector<GpuTextures::iterator> unused;
    for (auto it = cache->m_textures.begin(); it != cache->m_textures.end();
         ++it)
      if (it->second.m_lastUse != clock) unused.push_back(it);

    std::sort(
        unused.begin(), unused.end(),
        [](const GpuTextures::iterator &a, const GpuTextures::iterator &b) {
          return a->second.m_lastUse < b->second.m_lastUse;
        });

    for (size_t u = 0; u < unused.size() && cache->m_bytes > c_gpuTextureBudget;
         ++u)
      eraseGpuTexture(*cache, unused[u]);
  }

  return true;
}

//-----------------------------------------------------------------------------
/*! Make frame visualization in QPainter.
\n	Draw in painter mode just raster image in m_nodes.
\n  Onion-skin or channel mode are not considered.
*/
void RasterPainter::drawRasterImages(QPainter &p, QPolygon cameraPol) {
  if (m_nodes.empty()) return;

  double delta = sqrt(fabs(m_nodes[0].m_aff.det()));
  TRectD bbox  = m_nodes[0].m_bbox.enlarge(delta);

  int i;
  for (i = 1; i < (int)m_nodes.size(); i++) {
    delta = sqrt(fabs(m_nodes[i].m_aff.det()));
    bbox += m_nodes[i].m_bbox.enlarge(delta);
  }
  TRect rect(tfloor(bbox.x0), tfloor(bbox.y0), tceil(bbox.x1), tceil(bbox.y1));
  rect = rect * TRect(0, 0, m_dim.lx - 1, m_dim.ly - 1);

  TRect r = rect - rect.getP00();
  TAffine flipY(1, 0, 0, 0, -1, m_dim.ly);
  p.setClipRegion(QRegion(cameraPol));
  for (i = 0; i < (int)m_nodes.size(); i++) {
    if (m_nodes[i].m_onionMode != Node::eOnionSkinNone) continue;
    p.resetTransform();
    TRasterP ras = m_nodes[i].m_raster;
    TAffine aff  = TTranslation(-rect.x0, -rect.y0) * flipY * m_nodes[i].m_aff;
    QTransform matrix(aff.a11, aff.a12, aff.a21, aff.a22, aff.a13,
                      aff.a23);  // Updated line
    QImage image = rasterToQImage(ras);
    if (image.isNull()) continue;
    p.setWorldTransform(matrix);
    p.drawImage(rect.getP00().x, rect.getP00().y, image);
  }

  p.resetTransform();
  m_nodes.clear();
}

static void buildAutocloseImage(
    TVectorImage *vaux, TVectorImage *vi,
    const std::vector<std::pair<int, double>> &startPoints,
    const std::vector<std::pair<int, double>> &endPoints) {
  for (UINT i = 0; i < startPoints.size(); i++) {
    TThickPoint p1 = vi->getStroke(startPoints[i].first)
                         ->getThickPoint(startPoints[i].second);
    TThickPoint p2 =
        vi->getStroke(endPoints[i].first)->getThickPoint(endPoints[i].second);
    std::vector<TThickPoint> points(3);
    points[0]       = p1;
    points[1]       = 0.5 * (p1 + p2);
    points[2]       = p2;
    points[0].thick = points[1].thick = points[2].thick = 0.0;
    TStroke *auxStroke                                  = new TStroke(points);
    auxStroke->setStyle(2);
    vaux->addStroke(auxStroke);
  }
}

TEnv::DoubleVar AutocloseFactor("InknpaintAutocloseFactor", 4.0);

static void drawAutocloses(TVectorImage *vi, TVectorRenderData &rd) {
  static TPalette *plt = 0;
  if (!plt) {
    plt = new TPalette();
    plt->addStyle(TPixel::Magenta);
  }

  std::vector<std::pair<int, double>> startPoints, endPoints;
  getClosingPoints(vi->getBBox(), AutocloseFactor, vi, startPoints, endPoints);
  TVectorImage *vaux = new TVectorImage();

  rd.m_palette = plt;
  buildAutocloseImage(vaux, vi, startPoints, endPoints);
  // temporarily disable fill check, to preserve the gap indicator color
  bool tCheckEnabledOriginal = rd.m_tcheckEnabled;
  rd.m_tcheckEnabled         = false;
  // draw
  tglDraw(rd, vaux);
  // restore original value
  rd.m_tcheckEnabled = tCheckEnabledOriginal;
  delete vaux;
}

//-----------------------------------------------------------------------------

/*! Take image from \b Stage::Player \b data and recall the right method for
this kind of image, for vector image recall \b onVectorImage(),for raster
image recall \b onRasterImage() for toonz image    recall \b onToonzImage().
*/
void RasterPainter::onImage(const Stage::Player &player) {
  if (m_singleColumnEnabled && !player.m_isCurrentColumn) return;

  // Attempt Plastic-deformed drawing
  // For now generating icons of plastic-deformed image causes crash as
  // QOffscreenSurface is created outside the gui thread.
  // As a quick workaround, ignore the deformation if this is called from
  // non-gui thread (i.e. icon generator thread)
  // 12/1/2018 Now the scene icon is rendered without deformation either
  // since it causes unknown error with opengl contexts..
  TStageObject *obj =
      ::plasticDeformedObj(player, m_vs.m_plasticVisualSettings);
  if (obj && QThread::currentThread() == qGuiApp->thread() &&
      !m_vs.m_forSceneIcon) {
    flushRasterImages();
    ::onPlasticDeformedImage(obj, player, m_vs, m_viewAff);
  } else {
    // Common image draw
    const TImageP &img = player.image();

    if (TVectorImageP vi = img)
      onVectorImage(vi.getPointer(), player);
    else if (TRasterImageP ri = img)
      onRasterImage(ri.getPointer(), player);
    else if (TToonzImageP ti = img)
      onToonzImage(ti.getPointer(), player);
    else if (TMeshImageP mi = img) {
      flushRasterImages();
      ::onMeshImage(mi.getPointer(), player, m_vs, m_viewAff);
    }
  }
}

//-----------------------------------------------------------------------------
/*! View a vector cell images.
\n	If onion-skin is active compute \b TOnionFader value.
Create and boot a \b TVectorRenderData and recall \b tglDraw().
*/
void RasterPainter::onVectorImage(TVectorImage *vi,
                                  const Stage::Player &player) {
  flushRasterImages();

  // When loaded, vectorimages needs to have regions recomputed, but doing
  // that while loading them is quite slow (think about loading whole
  // scenes!..). They are recomputed the first time they are selected and
  // shown on screen...except when playing back, to avoid slowness!

  // (Daniele) This function should *NOT* be responsible of that.
  //           It's the *image itself* that should recalculate or initialize
  //           said data
  //           if queried about it and turns out it's not available...

  if (!player.m_isPlaying && player.m_isCurrentColumn)
    vi->recomputeRegionsIfNeeded();

  const Preferences &prefs = *Preferences::instance();

  TColorFunction *cf = 0, *guidedCf = 0;
  TPalette *vPalette = vi->getPalette();
  TPixel32 bgColor   = TPixel32::White;

  // Show the overrides of the Colour Override Fxs that follow the column, on a
  // copy of the palette that lives until the end of the function
  TPaletteP overridden;
  if (m_colourOverrides) {
    overridden = overriddenPalette(player, vPalette);
    if (overridden) vPalette = overridden.getPointer();
  }

  int tc        = (m_checkFlags && player.m_isCurrentColumn)
                      ? ToonzCheck::instance()->getChecks()
                      : 0;
  bool inksOnly = tc & ToonzCheck::eInksOnly;

  int oldFrame = vPalette->getFrame();
  vPalette->setFrame(player.m_frame);

  // Outline View: the drawing of the column is drawn as strokes only, all in
  // the outline colour. The onion skins of the column keep their colours, but
  // show the strokes only.
  const bool outlineColor =
      player.m_outlineView && (player.m_onionSkinDistance == c_noOnionSkin ||
                               player.m_onionSkinDistance == 0);

  if (outlineColor) {
    // Every colour becomes the outline colour, the alpha keeps the opacity of
    // the column
    const double m[4] = {0.0, 0.0, 0.0, player.m_opacity / 255.0};
    const double c[4] = {double(player.m_outlineColor.r),
                         double(player.m_outlineColor.g),
                         double(player.m_outlineColor.b), 0.0};
    cf                = new TGenericColorFunction(m, c);
  } else if (player.m_onionSkinDistance != c_noOnionSkin) {
    TPixel32 frontOnionColor, backOnionColor;

    if (player.m_onionSkinDistance != 0 &&
        (!player.m_isShiftAndTraceEnabled ||
         Preferences::instance()->areOnionColorsUsedForShiftAndTraceGhosts())) {
      prefs.getOnionData(frontOnionColor, backOnionColor, inksOnly);
      bgColor =
          (player.m_onionSkinDistance < 0) ? backOnionColor : frontOnionColor;
    }

    double m[4] = {1.0, 1.0, 1.0, 1.0}, c[4];

    // Weighted addition to RGB and matte multiplication
    m[3] = getOnionSkinOpacity(player);
    c[0] = (1.0 - m[3]) * bgColor.r, c[1] = (1.0 - m[3]) * bgColor.g,
    c[2] = (1.0 - m[3]) * bgColor.b;
    c[3] = 0.0;

    cf = new TGenericColorFunction(m, c);
  } else if (player.m_filterColor != TPixel::Black) {
    TPixel32 colorScale = player.m_filterColor;
    colorScale.m        = player.m_opacity;
    cf                  = new TColumnColorFilterFunction(colorScale);
  } else if (player.m_opacity < 255)
    cf = new TTranspFader(player.m_opacity / 255.0);

  TVectorRenderData rd(m_viewAff * player.m_placement, TRect(), vPalette, cf,
                       true  // alpha enabled
  );

  rd.m_drawRegions           = !inksOnly && !player.m_outlineView;
  rd.m_inkCheckEnabled       = tc & ToonzCheck::eInk;
  rd.m_ink1CheckEnabled      = tc & ToonzCheck::eInk1;
  rd.m_paintCheckEnabled     = tc & ToonzCheck::ePaint;
  rd.m_blackBgEnabled        = tc & ToonzCheck::eBlackBg;
  rd.m_colorCheckIndex       = ToonzCheck::instance()->getColorIndex();
  rd.m_paintIndex            = ToonzCheck::instance()->getColorIndex();
  rd.m_inkCheckColor         = Preferences::instance()->getInkCheckColor();
  rd.m_ink1CheckColor        = Preferences::instance()->getInk1CheckColor();
  rd.m_paintCheckColor       = Preferences::instance()->getPaintCheckColor();
  rd.m_show0ThickStrokes     = prefs.getShow0ThickLines();
  rd.m_regionAntialias       = prefs.getRegionAntialias();
  rd.m_animatedGuidedDrawing = prefs.getAnimatedGuidedDrawing();
  if (player.m_isCurrentColumn &&
      (player.m_onionSkinDistance == 0 ||
       player.m_onionSkinDistance == c_noOnionSkin)) {
    if (m_vs.m_showColourArt) rd.m_show0ThickStrokes = true;
    rd.m_fadeThickStrokes  = m_vs.m_fadeLineArt;
    rd.m_fade0ThickStrokes = m_vs.m_fadeColourArt;
    rd.m_hiddenRoleMask    = m_vs.m_hiddenArtRoles;
  }
  if (player.m_onionSkinDistance != 0 &&
      (player.m_isCurrentColumn || player.m_isCurrentXsheetLevel)) {
    if (player.m_isGuidedDrawingEnabled == 3         // show guides on all
        || (player.m_isGuidedDrawingEnabled == 1 &&  // show guides on closest
            (player.m_onionSkinDistance == player.m_firstBackOnionSkin ||
             player.m_onionSkinDistance == player.m_firstFrontOnionSkin)) ||
        (player.m_isGuidedDrawingEnabled == 2 &&  // show guides on farthest
         (player.m_onionSkinDistance == player.m_onionSkinBackSize ||
          player.m_onionSkinDistance == player.m_onionSkinFrontSize)) ||
        (player.m_isEditingLevel &&  // fix for level editing mode sending
                                     // extra players
         player.m_isGuidedDrawingEnabled == 2 &&
         player.m_onionSkinDistance == player.m_lastBackVisibleSkin)) {
      rd.m_showGuidedDrawing = player.m_isGuidedDrawingEnabled > 0;
      int currentStrokeCount = 0;
      int totalStrokes       = vi->getStrokeCount();
      TXshSimpleLevel *sl    = player.m_sl;

      if (sl) {
        TImageP image          = sl->getFrame(player.m_currentFrameId, false);
        TVectorImageP vecImage = image;
        if (vecImage) currentStrokeCount = vecImage->getStrokeCount();
        if (currentStrokeCount < 0) currentStrokeCount = 0;
        if (player.m_guidedFrontStroke != -1 &&
            (player.m_onionSkinDistance == player.m_onionSkinFrontSize ||
             player.m_onionSkinDistance == player.m_firstFrontOnionSkin))
          rd.m_indexToHighlight = player.m_guidedFrontStroke;
        else if (player.m_guidedBackStroke != -1 &&
                 (player.m_onionSkinDistance == player.m_onionSkinBackSize ||
                  player.m_onionSkinDistance == player.m_firstBackOnionSkin))
          rd.m_indexToHighlight = player.m_guidedBackStroke;
        else if (currentStrokeCount < totalStrokes)
          rd.m_indexToHighlight = currentStrokeCount;

        double guidedM[4] = {1.0, 1.0, 1.0, 1.0}, guidedC[4];
        TPixel32 bgColor  = TPixel32::Blue;
        guidedM[3] = getOnionSkinOpacity(player);

        guidedC[0] = (1.0 - guidedM[3]) * bgColor.r,
        guidedC[1] = (1.0 - guidedM[3]) * bgColor.g,
        guidedC[2] = (1.0 - guidedM[3]) * bgColor.b;
        guidedC[3] = 0.0;

        guidedCf      = new TGenericColorFunction(guidedM, guidedC);
        rd.m_guidedCf = guidedCf;
      }
    }
  }

  if (tc & (ToonzCheck::eTransparency | ToonzCheck::eGap)) {
    TPixel dummy;
    rd.m_tcheckEnabled = true;

    if (rd.m_blackBgEnabled)
      prefs.getTranspCheckData(rd.m_tCheckInk, dummy, rd.m_tCheckPaint);
    else
      prefs.getTranspCheckData(dummy, rd.m_tCheckInk, rd.m_tCheckPaint);
  }

  if (m_vs.m_colorMask != 0) {
    glColorMask((m_vs.m_colorMask & TRop::RChan) ? GL_TRUE : GL_FALSE,
                (m_vs.m_colorMask & TRop::GChan) ? GL_TRUE : GL_FALSE,
                (m_vs.m_colorMask & TRop::BChan) ? GL_TRUE : GL_FALSE, GL_TRUE);
  }
  TVectorImageP viDelete;
  if (tc & ToonzCheck::eGap) {
    viDelete = vi->clone();
    vi       = viDelete.getPointer();
    vi->selectFill(vi->getBBox(), 0, 1, true, true, false);
  }

  TStroke *guidedStroke = 0;
  if (m_maskLevel > 0)
    tglDrawMask(rd, vi);
  else
    tglDraw(rd, vi, &guidedStroke);

  if (tc & ToonzCheck::eAutoclose) drawAutocloses(vi, rd);

  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  vPalette->setFrame(oldFrame);

  delete cf;
  delete guidedCf;

  if (guidedStroke) m_guidedStrokes.push_back(guidedStroke);
}

//-----------------------------------------------------

/*! Create a \b Node and put it in \b m_nodes.
 */
void RasterPainter::onRasterImage(TRasterImage *ri,
                                  const Stage::Player &player) {
  TRasterP r = ri->getRaster();

  TAffine aff;
  aff = m_viewAff * player.m_placement * player.m_dpiAff;

  aff = TTranslation(m_dim.lx * 0.5, m_dim.ly * 0.5) * aff *
        TTranslation(-r->getCenterD() +
                     convert(ri->getOffset()));  // this offset is !=0 only if
                                                 // in cleanup camera test mode

  TRectD bbox = TRectD(0, 0, m_dim.lx, m_dim.ly);
  bbox *= convert(m_clipRect);
  if (bbox.isEmpty()) return;

  int alpha                 = 255;
  Node::OnionMode onionMode = Node::eOnionSkinNone;
  if (player.m_onionSkinDistance != c_noOnionSkin) {
    // GetOnionSkinFade is good for vector while raster works the opposite way
    // 1 opaque -> 0 completely transparent
    // So I invert the result of the function, being careful about case 0
    // (where the value 0.9 was carved)
    double onionSkinOpacity = getOnionSkinOpacity(player);
    alpha = tcrop(tround(onionSkinOpacity * 255.0), 0, 255);
    if (player.m_isShiftAndTraceEnabled &&
        !Preferences::instance()->areOnionColorsUsedForShiftAndTraceGhosts())
      onionMode = Node::eOnionSkinNone;
    else {
      onionMode =
          (player.m_onionSkinDistance > 0)
              ? Node::eOnionSkinFront
              : ((player.m_onionSkinDistance < 0) ? Node::eOnionSkinBack
                                                  : Node::eOnionSkinNone);
    }
  } else if (player.m_opacity < 255)
    alpha = player.m_opacity;
  TXshSimpleLevel *sl = player.m_sl;
  bool doPremultiply  = false;
  bool whiteTransp    = false;
  if (sl) {
    LevelProperties *levelProp = sl->getProperties();
    if (levelProp->doPremultiply())
      doPremultiply = true;
    else if (levelProp->whiteTransp())
      whiteTransp = true;
  }

  bool ignoreAlpha =
      (Preferences::instance()->isIgnoreAlphaonColumn1Enabled() &&
       player.m_column == 0 &&
       isSubsheetChainOnColumn0(sl->getScene()->getTopXsheet(), player.m_xsh,
                                player.m_frame));

  m_nodes.push_back(Node(r, 0, alpha, aff, ri->getSavebox(), bbox,
                         player.m_frame, player.m_isCurrentColumn, onionMode,
                         doPremultiply, whiteTransp, ignoreAlpha,
                         player.m_filterColor));

  if (m_gpuCompositing && sl) {
    m_nodes.back().m_imageId      = sl->getImageId(player.m_fid);
    m_nodes.back().m_gpuCacheable = isGpuCacheable(player);
  }
}

//-----------------------------------------------------------------------------
/*! Create a \b Node and put it in \b m_nodes.
 */
void RasterPainter::onToonzImage(TToonzImage *ti, const Stage::Player &player) {
  TRasterCM32P r = ti->getRaster();
  if (!ti->getPalette()) return;

  TAffine aff = m_viewAff * player.m_placement * player.m_dpiAff;
  aff         = TTranslation(m_dim.lx / 2.0, m_dim.ly / 2.0) * aff *
        TTranslation(-r->getCenterD());

  TRectD bbox = TRectD(0, 0, m_dim.lx, m_dim.ly);
  bbox *= convert(m_clipRect);
  if (bbox.isEmpty()) return;

  int alpha                 = 255;
  Node::OnionMode onionMode = Node::eOnionSkinNone;
  if (player.m_onionSkinDistance != c_noOnionSkin) {
    // GetOnionSkinFade is good for the vector while the raster works at the
    //    Opposite 1 opaque -> 0 completely transparent
    //    I therefore reverse the result of the function by being attentive to
    //    case 0
    //    (where the value 0.9 was carved)
    double onionSkinOpacity = getOnionSkinOpacity(player);
    alpha = tcrop(tround(onionSkinOpacity * 255.0), 0, 255);

    if (player.m_isShiftAndTraceEnabled &&
        !Preferences::instance()->areOnionColorsUsedForShiftAndTraceGhosts())
      onionMode = Node::eOnionSkinNone;
    else {
      onionMode =
          (player.m_onionSkinDistance > 0)
              ? Node::eOnionSkinFront
              : ((player.m_onionSkinDistance < 0) ? Node::eOnionSkinBack
                                                  : Node::eOnionSkinNone);
    }

  } else if (player.m_opacity < 255)
    alpha = player.m_opacity;

  // Show the overrides of the Colour Override Fxs that follow the column; the
  // node keeps the copy of the palette alive
  TPalette *palette = ti->getPalette();
  TPaletteP overridden;
  TPixel32 filterColor = player.m_filterColor;
  if (player.m_outlineView && onionMode == Node::eOnionSkinNone) {
    // Outline View: the inks are drawn in the outline colour of the column,
    // which the color filter of the column must not tint
    overridden  = outlinePalette(player, palette);
    filterColor = TPixel32::Black;
    if (overridden) palette = overridden.getPointer();
  } else if (m_colourOverrides) {
    overridden = overriddenPalette(player, palette);
    if (overridden) palette = overridden.getPointer();
  }

  m_nodes.push_back(Node(r, palette, alpha, aff, ti->getSavebox(), bbox,
                         player.m_frame, player.m_isCurrentColumn, onionMode,
                         false, false, false, filterColor));
  m_nodes.back().m_ownedPalette = overridden;
  m_nodes.back().m_outlineView  = player.m_outlineView;

  if (m_gpuCompositing && player.m_sl) {
    m_nodes.back().m_imageId      = player.m_sl->getImageId(player.m_fid);
    m_nodes.back().m_gpuCacheable = isGpuCacheable(player);
  }
}

//**********************************************************************************************
//    OpenGLPainter  implementation
//**********************************************************************************************

OpenGlPainter::OpenGlPainter(const TAffine &viewAff, const TRect &rect,
                             const ImagePainter::VisualSettings &vs,
                             bool isViewer, bool alphaEnabled)
    : Visitor(vs)
    , m_viewAff(viewAff)
    , m_clipRect(rect)
    , m_camera3d(false)
    , m_phi(0)
    , m_maskLevel(0)
    , m_isViewer(isViewer)
    , m_alphaEnabled(alphaEnabled)
    , m_paletteHasChanged(false)
    , m_minZ(0) {}

//-----------------------------------------------------------------------------

void OpenGlPainter::onImage(const Stage::Player &player) {
  if (player.m_z < m_minZ) m_minZ = player.m_z;

  glPushAttrib(GL_ALL_ATTRIB_BITS);
  glPushMatrix();

  if (m_camera3d)
    glTranslated(
        0, 0,
        player.m_z);  // Ok, move object along z as specified in the player

  // Attempt Plastic-deformed drawing
  if (TStageObject *obj =
          ::plasticDeformedObj(player, m_vs.m_plasticVisualSettings))
    ::onPlasticDeformedImage(obj, player, m_vs, m_viewAff);
  else if (const TImageP &image = player.image()) {
    if (TVectorImageP vi = image)
      onVectorImage(vi.getPointer(), player);
    else if (TRasterImageP ri = image)
      onRasterImage(ri.getPointer(), player);
    else if (TToonzImageP ti = image)
      onToonzImage(ti.getPointer(), player);
    else if (TMeshImageP mi = image)
      onMeshImage(mi.getPointer(), player, m_vs, m_viewAff);
  }

  glPopMatrix();
  glPopAttrib();
}

//-----------------------------------------------------------------------------

void OpenGlPainter::onVectorImage(TVectorImage *vi,
                                  const Stage::Player &player) {
  if (m_camera3d && (player.m_onionSkinDistance == c_noOnionSkin ||
                     player.m_onionSkinDistance == 0)) {
    const TRectD &bbox = player.m_placement * player.m_dpiAff * vi->getBBox();
    draw3DShadow(bbox, player.m_z, m_phi);
  }

  TColorFunction *cf = 0;

  TPalette *vPalette = vi->getPalette();

  int oldFrame = vPalette->getFrame();
  vPalette->setFrame(player.m_frame);  // Hehe. Should be locking
                                       // vPalette's mutex here...
  if (player.m_onionSkinDistance != c_noOnionSkin) {
    TPixel32 bgColor = TPixel32::White;
    double opacity   = getOnionSkinOpacity(player);
    double m[4]      = {1.0, 1.0, 1.0, opacity};
    double c[4]      = {(1.0 - opacity) * bgColor.r,
                        (1.0 - opacity) * bgColor.g,
                        (1.0 - opacity) * bgColor.b, 0.0};
    cf               = new TGenericColorFunction(m, c);
  }

  TVectorRenderData rd =
      isViewer() ? TVectorRenderData(TVectorRenderData::ViewerSettings(),
                                     m_viewAff * player.m_placement, m_clipRect,
                                     vPalette, cf)
                 : TVectorRenderData(TVectorRenderData::ProductionSettings(),
                                     m_viewAff * player.m_placement, m_clipRect,
                                     vPalette, cf);

  rd.m_alphaChannel = m_alphaEnabled;
  rd.m_is3dView     = m_camera3d;

  if (m_maskLevel > 0)
    tglDrawMask(rd, vi);
  else
    tglDraw(rd, vi);

  vPalette->setFrame(oldFrame);
  delete cf;
}

//-----------------------------------------------------------------------------

void OpenGlPainter::onRasterImage(TRasterImage *ri,
                                  const Stage::Player &player) {
  if (m_camera3d && (player.m_onionSkinDistance == c_noOnionSkin ||
                     player.m_onionSkinDistance == 0)) {
    TRectD bbox(ri->getBBox());

    // Since bbox is in image coordinates, adjust to level coordinates first
    bbox -= ri->getRaster()->getCenterD() - convert(ri->getOffset());

    // Then, to world coordinates
    bbox = player.m_placement * player.m_dpiAff * bbox;

    draw3DShadow(bbox, player.m_z, m_phi);
  }

  bool tlvFlag = player.m_sl && player.m_sl->getType() == TZP_XSHLEVEL;

  if (tlvFlag &&
      m_paletteHasChanged)  // o.o! Raster images here - should never be
    assert(false);          // dealing with tlv stuff!

  bool premultiplied = tlvFlag;  // xD
  static std::vector<char>
      matteChan;  // Wtf this is criminal. Although probably this
                  // stuff is used only in the main thread... hmmm....
  TRaster32P r = (TRaster32P)ri->getRaster();
  r->lock();

  if (c_noOnionSkin != player.m_onionSkinDistance) {
    double opacity =
        player.m_onionSkinOpacity >= 0.0
            ? player.m_onionSkinOpacity
            : 0.5 - 0.45 *
                        (1 - 1 /
                                 (1 + 0.15 * abs(player.m_onionSkinDistance)));
    if ((int)matteChan.size() < r->getLx() * r->getLy())
      matteChan.resize(r->getLx() * r->getLy());

    int k = 0;
    for (int y = 0; y < r->getLy(); ++y) {
      TPixel32 *pix    = r->pixels(y);
      TPixel32 *endPix = pix + r->getLx();

      while (pix < endPix) {
        matteChan[k++] = pix->m;
        pix->m         = (int)(pix->m * opacity);
        pix++;
      }
    }

    premultiplied = false;  // pfff
  }

  TAffine aff = player.m_dpiAff;

  glPushAttrib(GL_ALL_ATTRIB_BITS);
  tglEnableBlending(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  GLRasterPainter::drawRaster(m_viewAff * player.m_placement * aff *
                                  TTranslation(convert(ri->getOffset())),
                              ri, premultiplied);

  glPopAttrib();

  if (c_noOnionSkin != player.m_onionSkinDistance) {
    int k = 0;
    for (int y = 0; y < r->getLy(); ++y) {
      TPixel32 *pix    = r->pixels(y);
      TPixel32 *endPix = pix + r->getLx();

      while (pix < endPix) pix++->m = matteChan[k++];
    }
  }

  r->unlock();
}

//-----------------------------------------------------------------------------

void OpenGlPainter::onToonzImage(TToonzImage *ti, const Stage::Player &player) {
  if (m_camera3d && (player.m_onionSkinDistance == c_noOnionSkin ||
                     player.m_onionSkinDistance == 0)) {
    TRectD bbox(ti->getBBox());

    bbox -= ti->getRaster()->getCenterD();
    bbox = player.m_placement * player.m_dpiAff * bbox;

    draw3DShadow(bbox, player.m_z, m_phi);
  }

  TRasterCM32P ras = ti->getRaster();
  TRaster32P ras32(ras->getSize());
  ras32->fill(TPixel32(0, 0, 0, 0));

  // onionSkin
  TRop::quickPut(ras32, ras, ti->getPalette(), TAffine());
  TAffine aff = player.m_dpiAff;

  TRasterImageP ri(ras32);

  GLRasterPainter::drawRaster(m_viewAff * player.m_placement * aff, ri, true);
}

//-----------------------------------------------------------------------------

void OpenGlPainter::beginMask() {
  ++m_maskLevel;
  TStencilControl::instance()->beginMask();
}
void OpenGlPainter::endMask() {
  --m_maskLevel;
  TStencilControl::instance()->endMask();
}
void OpenGlPainter::enableMask() {
  TStencilControl::instance()->enableMask(TStencilControl::SHOW_INSIDE);
}
void OpenGlPainter::disableMask() {
  TStencilControl::instance()->disableMask();
}

//**********************************************************************************************
//    Plastic functions  implementation
//**********************************************************************************************

namespace {

TStageObject *plasticDeformedObj(const Stage::Player &player,
                                 const PlasticVisualSettings &pvs) {
  struct locals {
    static inline bool isDeformableMeshColumn(
        TXshColumn *column, const PlasticVisualSettings &pvs) {
      return (column->getColumnType() == TXshColumn::eMeshType) &&
             (pvs.m_showOriginalColumn != column);
    }

    static inline bool isDeformableMeshLevel(TXshSimpleLevel *sl) {
      return sl && (sl->getType() == MESH_XSHLEVEL);
    }
  };  // locals

  if (pvs.m_applyPlasticDeformation && player.m_column >= 0) {
    // Check whether the player's column is a direct stage-schematic child of
    // a mesh object
    TStageObject *playerObj =
        player.m_xsh->getStageObject(TStageObjectId::ColumnId(player.m_column));
    assert(playerObj);

    const TStageObjectId &parentId = playerObj->getParent();
    if (parentId.isColumn() && playerObj->getParentHandle()[0] != 'H') {
      TXshColumn *parentCol = player.m_xsh->getColumn(parentId.getIndex());

      if (locals::isDeformableMeshColumn(parentCol, pvs) &&
          !locals::isDeformableMeshLevel(player.m_sl)) {
        const SkDP &sd = player.m_xsh->getStageObject(parentId)
                             ->getPlasticSkeletonDeformation();

        const TXshCell &parentCell =
            player.m_xsh->getCell(player.m_frame, parentId.getIndex());
        TXshSimpleLevel *parentSl = parentCell.getSimpleLevel();

        if (sd && locals::isDeformableMeshLevel(parentSl)) return playerObj;
      }
    }
  }

  return 0;
}

//-----------------------------------------------------------------------------

void onMeshImage(TMeshImage *mi, const Stage::Player &player,
                 const ImagePainter::VisualSettings &vs,
                 const TAffine &viewAff) {
  assert(mi);

  static const double soMinColor[4]  = {0.0, 0.0, 0.0,
                                        0.6};  // Translucent black
  static const double soMaxColor[4]  = {1.0, 1.0, 1.0,
                                        0.6};  // Translucent white
  static const double rigMinColor[4] = {0.0, 1.0, 0.0,
                                        0.6};  // Translucent green
  static const double rigMaxColor[4] = {1.0, 0.0, 0.0, 0.6};  // Translucent red

  bool doOnionSkin    = (player.m_onionSkinDistance != c_noOnionSkin);
  bool onionSkinImage = doOnionSkin && (player.m_onionSkinDistance != 0);
  bool drawMeshes =
      vs.m_plasticVisualSettings.m_drawMeshesWireframe && !onionSkinImage;
  bool drawSO = vs.m_plasticVisualSettings.m_drawSO && !onionSkinImage;
  bool drawRigidity =
      vs.m_plasticVisualSettings.m_drawRigidity && !onionSkinImage;

  // Currently skipping onion skinned meshes
  if (onionSkinImage) return;

  // Build dpi
  TPointD meshSlDpi(player.m_sl->getDpi(player.m_fid, 0));
  assert(meshSlDpi.x != 0.0 && meshSlDpi.y != 0.0);

  // Build reference change affines

  const TAffine &worldMeshToMeshAff =
      TScale(meshSlDpi.x / Stage::inch, meshSlDpi.y / Stage::inch);
  const TAffine &meshToWorldMeshAff =
      TScale(Stage::inch / meshSlDpi.x, Stage::inch / meshSlDpi.y);
  const TAffine &worldMeshToWorldAff = player.m_placement;

  const TAffine &meshToWorldAff = worldMeshToWorldAff * meshToWorldMeshAff;

  // Prepare OpenGL
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_LINE_SMOOTH);

  // Push mesh coordinates
  glPushMatrix();
  tglMultMatrix(viewAff * meshToWorldAff);

  // Fetch deformation
  PlasticSkeletonDeformation *deformation = 0;
  double sdFrame;

  if (vs.m_plasticVisualSettings.m_applyPlasticDeformation &&
      player.m_column >= 0) {
    TXshColumn *column = player.m_xsh->getColumn(player.m_column);

    if (column != vs.m_plasticVisualSettings.m_showOriginalColumn) {
      TStageObject *playerObj = player.m_xsh->getStageObject(
          TStageObjectId::ColumnId(player.m_column));

      deformation = playerObj->getPlasticSkeletonDeformation().getPointer();
      sdFrame     = playerObj->paramsTime(player.m_frame);
    }
  }

  if (deformation) {
    // Retrieve the associated plastic deformers data (this may eventually
    // update the deforms)
    const PlasticDeformerDataGroup *dataGroup =
        PlasticDeformerStorage::instance()->process(
            sdFrame, mi, deformation, deformation->skeletonId(sdFrame),
            worldMeshToMeshAff);

    // Draw faces first
    if (drawSO)
      tglDrawSO(*mi, (double *)soMinColor, (double *)soMaxColor, dataGroup,
                true);

    if (drawRigidity)
      tglDrawRigidity(*mi, (double *)rigMinColor, (double *)rigMaxColor,
                      dataGroup, true);

    // Draw edges next
    if (drawMeshes) {
      glColor4d(0.0, 1.0, 0.0, 0.7 * player.m_opacity / 255.0);  // Green
      tglDrawEdges(*mi, dataGroup);  // The mesh must be deformed
    }
  } else {
    // Draw un-deformed data

    // Draw faces first
    if (drawSO) tglDrawSO(*mi, (double *)soMinColor, (double *)soMaxColor);

    if (drawRigidity)
      tglDrawRigidity(*mi, (double *)rigMinColor, (double *)rigMaxColor);

    // Just draw the mesh image next
    if (drawMeshes) {
      glColor4d(0.0, 1.0, 0.0, 0.7 * player.m_opacity / 255.0);  // Green
      tglDrawEdges(*mi);
    }
  }

  // Cleanup OpenGL
  glPopMatrix();

  glDisable(GL_LINE_SMOOTH);
  glDisable(GL_BLEND);
}

//-----------------------------------------------------------------------------

//! Applies Plastic deformation of the specified player's stage object.
void onPlasticDeformedImage(TStageObject *playerObj,
                            const Stage::Player &player,
                            const ImagePainter::VisualSettings &vs,
                            const TAffine &viewAff) {
  bool doOnionSkin    = (player.m_onionSkinDistance != c_noOnionSkin);
  bool onionSkinImage = doOnionSkin && (player.m_onionSkinDistance != 0);

  // Deal with color scaling due to transparency / onion skin
  double pixScale[4] = {1.0, 1.0, 1.0, 1.0};

  if (doOnionSkin) {
    if (onionSkinImage) {
      TPixel32 frontOnionColor, backOnionColor;
      bool inksOnly;

      Preferences::instance()->getOnionData(frontOnionColor, backOnionColor,
                                            inksOnly);

      const TPixel32 &refColor =
          (player.m_onionSkinDistance < 0) ? backOnionColor : frontOnionColor;

      pixScale[3] = getOnionSkinOpacity(player);
      pixScale[0] =
          (refColor.r / 255.0) * pixScale[3];  // refColor is not premultiplied
      pixScale[1] = (refColor.g / 255.0) * pixScale[3];
      pixScale[2] = (refColor.b / 255.0) * pixScale[3];
    }
  } else if (player.m_opacity < 255) {
    pixScale[3] = player.m_opacity / 255.0;
    pixScale[0] = pixScale[1] = pixScale[2] = 0.0;
  }

  // Build the Mesh-related data
  const TXshCell &cell =
      player.m_xsh->getCell(player.m_frame, playerObj->getParent().getIndex());

  TXshSimpleLevel *meshLevel = cell.getSimpleLevel();
  const TFrameId &meshFid    = cell.getFrameId();

  const TMeshImageP &mi = meshLevel->getFrame(meshFid, false);
  if (!mi) return;

  // Build deformation-related data
  TStageObject *parentObj =
      player.m_xsh->getStageObject(playerObj->getParent());
  assert(playerObj);

  const PlasticSkeletonDeformationP &deformation =
      parentObj->getPlasticSkeletonDeformation().getPointer();
  assert(deformation);

  double sdFrame = parentObj->paramsTime(player.m_frame);

  // Build dpis

  TPointD meshSlDpi(meshLevel->getDpi(meshFid, 0));
  assert(meshSlDpi.x != 0.0 && meshSlDpi.y != 0.0);

  TPointD slDpi(player.m_sl ? player.m_sl->getDpi(player.m_fid, 0) : TPointD());
  if (slDpi.x == 0.0 || slDpi.y == 0.0 ||
      player.m_sl->getType() == PLI_XSHLEVEL)
    slDpi.x = slDpi.y = Stage::inch;

  // Build reference transforms

  const TAffine &worldTextureToWorldMeshAff =
      playerObj->getLocalPlacement(player.m_frame);
  const TAffine &worldTextureToWorldAff = player.m_placement;

  if (fabs(worldTextureToWorldMeshAff.det()) < 1e-6)
    return;  // Skip near-singular mesh/texture placements

  const TAffine &worldMeshToWorldTextureAff = worldTextureToWorldMeshAff.inv();
  const TAffine &worldMeshToWorldAff =
      worldTextureToWorldAff * worldMeshToWorldTextureAff;

  const TAffine &meshToWorldMeshAff =
      TScale(Stage::inch / meshSlDpi.x, Stage::inch / meshSlDpi.y);
  const TAffine &worldMeshToMeshAff =
      TScale(meshSlDpi.x / Stage::inch, meshSlDpi.y / Stage::inch);
  const TAffine &worldTextureToTextureAff = TScale(
      slDpi.x / Stage::inch,
      slDpi.y /
          Stage::inch);  // ::getDpiScale().inv() should be used instead...

  const TAffine &meshToWorldAff   = worldMeshToWorldAff * meshToWorldMeshAff;
  const TAffine &meshToTextureAff = worldTextureToTextureAff *
                                    worldMeshToWorldTextureAff *
                                    meshToWorldMeshAff;

  // Retrieve a drawable texture from the player's simple level
  const DrawableTextureDataP &texData = player.texture();
  if (!texData) return;

  // Retrieve the associated plastic deformers data (this may eventually
  // update the deforms)
  const PlasticDeformerDataGroup *dataGroup =
      PlasticDeformerStorage::instance()->process(
          sdFrame, mi.getPointer(), deformation.getPointer(),
          deformation->skeletonId(sdFrame), worldMeshToMeshAff);
  assert(dataGroup);

  // Set up OpenGL stuff
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_LINE_SMOOTH);

  // Push mesh coordinates
  glPushMatrix();

  tglMultMatrix(viewAff * meshToWorldAff);

  glEnable(GL_TEXTURE_2D);

  // Applying modulation by the specified transparency parameter

  glColor4d(pixScale[3], pixScale[3], pixScale[3], pixScale[3]);
  tglDraw(*mi, *texData, meshToTextureAff, *dataGroup);

  glDisable(GL_TEXTURE_2D);

  if (onionSkinImage) {
    glBlendFunc(GL_ONE, GL_ONE);

    // Add Onion skin color. Observe that this way we don't consider blending
    // with the texture's alpha to obtain that, there is no simple way...

    double k = (1.0 - pixScale[3]);
    glColor4d(k * pixScale[0], k * pixScale[1], k * pixScale[2], 0.0);
    tglDrawFaces(*mi, dataGroup);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }

  glPopMatrix();

  glDisable(GL_LINE_SMOOTH);
  glDisable(GL_BLEND);
  assert(glGetError() == GL_NO_ERROR);
}

}  // namespace
