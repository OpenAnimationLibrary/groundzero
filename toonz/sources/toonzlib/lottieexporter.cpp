// Lottie (Bodymovin JSON) export of an xsheet: see toonz/lottieexporter.h and
// doc/animate_lottie_export.md.
//
// Mapping:
//  - The composition is the current camera of the xsheet (its resolution in
//    pixels), the frames the exported rows.
//  - Each column with something to show gets a null layer carrying its
//    placement baked frame by frame (local to its parent: parent.inv() *
//    placement, as the Spine export does, so that handles and hooks are
//    followed). Pegbars, the table and other parents get null layers too,
//    unless they never move the column (then they are left out); a moving
//    camera gets a root null layer with the inverse camera motion.
//  - The drawings are layers parented to the column's null layer, one per
//    run of the same drawing, visible from its first frame (ip) to the end of
//    its last one (op): shape layers for vector drawings (vectorShapes()),
//    image layers for raster ones, precomposition layers for sub-xsheets,
//    whose child frames become a time remap (tm) with hold keys. The column
//    opacity is the opacity of these layers.
//  - Lottie layers are listed top first: the columns from the last one.
//
// Not exported (listed in Result::m_skipped when met): FX, color filters,
// clipping masks, Z depth and stacking order changes, shear, Plastic
// deformations and mesh columns, sound and FX columns.

#include "toonz/lottieexporter.h"
#include "toonz/lottiemath.h"

// TnzLib includes
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshcell.h"
#include "toonz/txshlevel.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectid.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/tcamera.h"
#include "toonz/dpiscale.h"
#include "toonz/contouroutline.h"

// TnzCore includes
#include "tvectorimage.h"
#include "tstroke.h"
#include "tregion.h"
#include "tcurves.h"
#include "tpalette.h"
#include "tcolorstyles.h"
#include "tthreadmessage.h"
#include "tfx.h"

// Qt includes
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

// STD includes
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace {

// Not "Point": some platform headers declare a global one
typedef LottieMath::Affine LAffine;
typedef LottieMath::Point LPoint;

// Largest number of nested sub-xsheets followed
const int c_maxDepth = 32;

// The tag id of TSolidColorStyle: other styles are exported with their main
// color
const int c_solidStyleTag = 3;

// Thickness variation below which a stroke is a Lottie stroke, relative to
// its largest thickness
const double c_constantThickness = 0.1;

// Shear above which a dropped shear is reported (cosine of the angle between
// the transformed axes)
const double c_reportedShear = 0.01;

// Distance below which two path points are the same, pixels
const double c_pathEps = 1e-3;

const TAffine c_flipY(1, 0, 0, 0, -1, 0);

//-----------------------------------------------------------------------------

LAffine toMath(const TAffine &a) {
  return LAffine(a.a11, a.a12, a.a13, a.a21, a.a22, a.a23);
}

LPoint toMath(const TPointD &p) { return LPoint(p.x, p.y); }

bool isIdentity(const TAffine &a) {
  return toMath(a).isNear(LAffine(), 1e-9, 1e-6);
}

TAffine safeInv(const TAffine &a) {
  double det = a.det();
  if (!std::isfinite(det) || std::abs(det) < 1e-12) return TAffine();
  return a.inv();
}

double clampW(double w) {
  if (!std::isfinite(w) || w < 1e-9) return 0.0;
  if (w > 1.0 - 1e-9) return 1.0;
  return w;
}

//=============================================================================
// Vector drawings
//-----------------------------------------------------------------------------

// Appends the edge of a region to builder, from m_w0 to m_w1 along its stroke
// (backward when m_w1 comes first), as stroke2polyline() follows it.
void addEdge(const TEdge &edge, const TAffine &toLayer,
             LottieMath::PathBuilder &builder) {
  const TStroke *stroke = edge.m_s;
  if (!stroke || stroke->getChunkCount() <= 0) return;

  double w0 = clampW(edge.m_w0), w1 = clampW(edge.m_w1);
  int i0 = 0, i1 = 0;
  double t0 = 0.0, t1 = 0.0;
  // getChunkAndT() returns true when it fails
  if (stroke->getChunkAndT(w0, i0, t0) || stroke->getChunkAndT(w1, i1, t1))
    return;
  int count = stroke->getChunkCount();
  if (i0 < 0 || i0 >= count || i1 < 0 || i1 >= count) return;

  bool backward = i1 < i0 || (i1 == i0 && t1 < t0);
  int step      = backward ? -1 : 1;
  for (int i = i0;; i += step) {
    const TThickQuadratic *q = stroke->getChunk(i);
    double ta                = (i == i0) ? t0 : (backward ? 1.0 : 0.0);
    double tb                = (i == i1) ? t1 : (backward ? 0.0 : 1.0);
    if (q && ta != tb) {
      LPoint q0, q1, q2;
      LottieMath::subQuadratic(
          toMath(toLayer * q->getP0()), toMath(toLayer * q->getP1()),
          toMath(toLayer * q->getP2()), ta, tb, q0, q1, q2);
      builder.addQuadratic(q0, q1, q2, true);
    }
    if (i == i1) break;
  }
}

//-----------------------------------------------------------------------------

// The outline of a region (its edges, gaps closed by lines) as closed paths.
std::vector<LottieMath::Path> regionPaths(const TRegion &region,
                                          const TAffine &toLayer) {
  LottieMath::PathBuilder builder(c_pathEps);
  for (UINT e = 0; e < region.getEdgeCount(); ++e) {
    const TEdge *edge = region.getEdge(e);
    if (edge) addEdge(*edge, toLayer, builder);
  }
  builder.close();
  std::vector<LottieMath::Path> paths = builder.paths();
  for (LottieMath::Path &path : paths) path.m_closed = true;
  return paths;
}

//-----------------------------------------------------------------------------

struct ShapeContext {
  const TPalette *m_palette;
  TAffine m_toLayer;
  double m_scale;      // of the lengths
  QJsonArray m_items;  // bottom first
  QStringList *m_notes;

  void note(const QString &text) {
    if (m_notes && !m_notes->contains(text)) m_notes->append(text);
  }

  // The main color of a style; false if it shows nothing.
  bool styleColor(int styleId, TPixel32 &color) {
    const TColorStyle *style =
        m_palette ? m_palette->getStyle(styleId) : nullptr;
    if (!style) return false;
    color = style->getMainColor();
    if (style->getTagId() != c_solidStyleTag)
      note(
          QObject::tr("Special styles (gradients, textures, brushes) are "
                      "exported with their main color."));
    return color.m != 0;
  }
};

//-----------------------------------------------------------------------------

// A filled region, its subregions cut as holes (even-odd rule), then the
// subregions, drawn over it, as tglDraw() draws them.
void addRegion(const TRegion *region, ShapeContext &context, int depth = 0) {
  if (!region || depth > 256) return;

  TPixel32 color;
  int styleId = region->getStyle();
  if (styleId != 0 && context.styleColor(styleId, color)) {
    std::vector<LottieMath::Path> paths =
        regionPaths(*region, context.m_toLayer);
    if (!paths.empty()) {
      for (UINT s = 0; s < region->getSubregionCount(); ++s) {
        const TRegion *sub = region->getSubregion(s);
        if (!sub) continue;
        std::vector<LottieMath::Path> holes =
            regionPaths(*sub, context.m_toLayer);
        paths.insert(paths.end(), holes.begin(), holes.end());
      }
      QJsonArray items;
      for (const LottieMath::Path &path : paths)
        items.append(LottieMath::pathShape(path));
      items.append(LottieMath::fillShape(
          LottieMath::color(color.r, color.g, color.b),
          LottieMath::opacityPercent(color.m), LottieMath::EvenOdd));
      context.m_items.append(LottieMath::group("Region", items));
    }
  }

  for (UINT s = 0; s < region->getSubregionCount(); ++s)
    addRegion(region->getSubregion(s), context, depth + 1);
}

//-----------------------------------------------------------------------------

int lottieCap(int capStyle) {
  switch (capStyle) {
  case TStroke::OutlineOptions::ROUND_CAP:
    return LottieMath::RoundCap;
  case TStroke::OutlineOptions::PROJECTING_CAP:
    return LottieMath::SquareCap;
  default:
    return LottieMath::ButtCap;
  }
}

int lottieJoin(int joinStyle) {
  switch (joinStyle) {
  case TStroke::OutlineOptions::ROUND_JOIN:
    return LottieMath::RoundJoin;
  case TStroke::OutlineOptions::BEVEL_JOIN:
    return LottieMath::BevelJoin;
  default:
    return LottieMath::MiterJoin;
  }
}

//-----------------------------------------------------------------------------

// A stroke: a Lottie stroke on its centerline when its thickness is about
// constant (or the stroke is closed, a ring that the outline conversion does
// not handle), else its outline (ContourOutline) filled.
void addStroke(const TStroke *stroke, ShapeContext &context) {
  if (!stroke || stroke->getChunkCount() <= 0) return;
  if (!stroke->getOpacityProfile().empty())
    context.note(QObject::tr(
        "Along-stroke opacity envelopes are exported with uniform stroke opacity. "
        "Use Animated SVG with Render Scene for the rendered appearance; "
        "keep PLI 150.1 drawings to edit the envelopes."));

  TPixel32 color;
  if (!context.styleColor(stroke->getStyle(), color)) return;
  double opacity = LottieMath::opacityPercent(color.m, stroke->getOpacity());
  if (opacity <= 0.0) return;

  int count       = stroke->getControlPointCount();
  double minThick = 0.0, maxThick = 0.0, sumThick = 0.0;
  for (int i = 0; i < count; ++i) {
    double thick = stroke->getControlPoint(i).thick;
    if (!std::isfinite(thick) || thick < 0.0) thick = 0.0;
    minThick = (i == 0) ? thick : std::min(minThick, thick);
    maxThick = std::max(maxThick, thick);
    sumThick += thick;
  }
  if (count <= 0 || maxThick <= 0.0) return;  // invisible: region boundaries

  const TStroke::OutlineOptions &options = stroke->outlineOptions();
  QJsonArray rgb = LottieMath::color(color.r, color.g, color.b);
  QJsonArray items;

  bool constant =
      maxThick - minThick <= c_constantThickness * maxThick || count < 3;
  if (!constant && !stroke->isSelfLoop()) {
    std::vector<TThickPoint> points;
    if (ContourOutline::makeOutlineControlPoints(*stroke, points) &&
        points.size() >= 3) {
      LottieMath::PathBuilder builder(c_pathEps);
      for (size_t i = 0; i + 2 < points.size(); i += 2)
        builder.addQuadratic(toMath(context.m_toLayer * TPointD(points[i])),
                             toMath(context.m_toLayer * TPointD(points[i + 1])),
                             toMath(context.m_toLayer * TPointD(points[i + 2])),
                             true);
      builder.close();
      for (const LottieMath::Path &path : builder.paths())
        items.append(LottieMath::pathShape(path, "Outline"));
      if (!items.isEmpty()) {
        items.append(LottieMath::fillShape(rgb, opacity, LottieMath::NonZero));
        context.m_items.append(LottieMath::group("Stroke", items));
        return;
      }
    }
  }

  // Centerline, with the average thickness
  LottieMath::PathBuilder builder(c_pathEps);
  for (int c = 0; c < stroke->getChunkCount(); ++c) {
    const TThickQuadratic *q = stroke->getChunk(c);
    if (!q) continue;
    builder.addQuadratic(toMath(context.m_toLayer * q->getP0()),
                         toMath(context.m_toLayer * q->getP1()),
                         toMath(context.m_toLayer * q->getP2()), false);
  }
  if (stroke->isSelfLoop()) builder.close();
  items = QJsonArray();
  for (const LottieMath::Path &path : builder.paths())
    items.append(LottieMath::pathShape(path));
  if (items.isEmpty()) return;

  double width = 2.0 * sumThick / count * context.m_scale;
  items.append(LottieMath::strokeShape(
      rgb, opacity, width, lottieCap(options.m_capStyle),
      lottieJoin(options.m_joinStyle), options.m_miterUpper));
  context.m_items.append(LottieMath::group("Stroke", items));
}

//=============================================================================
// Xsheets
//-----------------------------------------------------------------------------

// The frame of reference children of a stage object are placed in: the
// composition (camera view), the root camera layer (the world), or the null
// layer of a stage object.
struct Rig {
  enum Kind { View, World, Object };
  Kind m_kind = View;
  int m_ind   = 0;  // layer index, 0 for View
  TStageObjectId m_id;
};

struct Composition {
  TXsheet *m_xsheet = nullptr;
  int m_r0 = 0, m_count = 1;
  double m_width = 1, m_height = 1;
  TAffine m_toPixels, m_toStage;  // stage units <-> camera pixels
  std::vector<TAffine> m_cameraInv;
  Rig m_base;
  int m_nextInd = 1;
  QJsonArray m_contentLayers;  // top first
  QJsonArray m_nullLayers;
  std::map<TStageObjectId, Rig> m_rigs;
  std::map<TStageObjectId, std::vector<TAffine>> m_worlds;
};

struct ImageInfo {
  QString m_id;  // empty if the image could not be made
  int m_width = 0, m_height = 0;
  TPointD m_offset;
};

struct CompInfo {
  QString m_id;
  double m_width = 1, m_height = 1;
  TAffine m_toStage;
};

//-----------------------------------------------------------------------------

class Exporter {
public:
  Exporter(const LottieExport::Options &options, LottieExport::Result &result)
      : m_options(options), m_result(result) {
    m_fps = LottieMath::finite(options.m_frameRate, 24.0);
    if (m_fps <= 0.0) m_fps = 24.0;
  }

  const LottieExport::Options &m_options;
  LottieExport::Result &m_result;
  double m_fps;
  QJsonArray m_assets;
  std::map<TXsheet *, CompInfo> m_comps;
  std::set<TXsheet *> m_building;
  std::map<std::pair<TXshSimpleLevel *, TFrameId>, ImageInfo> m_images;
  std::map<std::tuple<TXshSimpleLevel *, TFrameId, double, double>, QJsonArray>
      m_shapes;
  int m_imageCount = 0, m_compCount = 0;

  void skip(const QString &what) {
    if (!m_result.m_skipped.contains(what)) m_result.m_skipped.append(what);
  }

  void initComposition(Composition &c, TXsheet *xsheet, int r0, int r1);
  void buildComposition(Composition &c, int depth);
  QJsonArray layers(const Composition &c) const;

  const std::vector<TAffine> &world(Composition &c, const TStageObjectId &id);
  std::vector<TAffine> localPlacements(Composition &c, const TStageObjectId &id,
                                       const Rig &parent);
  std::vector<LAffine> lottieLocals(const Composition &c,
                                    const std::vector<TAffine> &locals,
                                    const Rig &parent) const;
  Rig rigOf(Composition &c, const TStageObjectId &id, int guard = 0);

  void addColumn(Composition &c, int col, int depth);
  const QJsonArray &shapes(Composition &c, TXshSimpleLevel *sl,
                           const TFrameId &fid);
  const ImageInfo &image(TXshSimpleLevel *sl, const TFrameId &fid);
  const CompInfo *composition(TXsheet *child, int depth);

  QJsonObject transform(const std::vector<LAffine> &frames, double firstFrame,
                        double opacity) {
    double shear   = 0.0;
    QJsonObject ks = LottieMath::layerTransform(
        frames, firstFrame, opacity, LottieMath::KeyTolerance(), &shear);
    if (shear > c_reportedShear)
      skip(
          QObject::tr("Shear: Lottie layers are written without skew, "
                      "sheared placements are approximated."));
    return ks;
  }
};

//-----------------------------------------------------------------------------

QJsonObject baseLayer(int ty, int ind, int parent, const QString &name, int ip,
                      int op, const QJsonObject &ks) {
  QJsonObject layer;
  layer["ddd"] = 0;
  layer["ind"] = ind;
  layer["ty"]  = ty;
  layer["nm"]  = name;
  layer["sr"]  = 1;
  layer["ks"]  = ks;
  layer["ao"]  = 0;
  layer["ip"]  = ip;
  layer["op"]  = std::max(op, ip + 1);
  layer["st"]  = 0;
  layer["bm"]  = 0;
  if (parent > 0) layer["parent"] = parent;
  return layer;
}

//-----------------------------------------------------------------------------

void Exporter::initComposition(Composition &c, TXsheet *xsheet, int r0,
                               int r1) {
  c.m_xsheet = xsheet;
  c.m_r0     = r0;
  c.m_count  = std::max(1, r1 - r0 + 1);

  TStageObjectTree *tree  = xsheet->getStageObjectTree();
  TStageObjectId cameraId = tree->getCurrentCameraId();
  TCamera *camera         = tree->getCamera(cameraId);
  if (camera) {
    TDimension res = camera->getRes();
    c.m_width      = std::max(1, res.lx);
    c.m_height     = std::max(1, res.ly);
    c.m_toStage    = getDpiAffine(camera);
    c.m_toPixels   = safeInv(c.m_toStage);
  }

  // The camera motion, on a root layer if the camera moves
  bool moves = false, depth = false;
  for (int f = 0; f < c.m_count; ++f) {
    TAffine inv = safeInv(xsheet->getPlacement(cameraId, r0 + f));
    c.m_cameraInv.push_back(inv);
    if (!isIdentity(inv)) moves = true;
    if (xsheet->getZ(cameraId, r0 + f) != 0.0) depth = true;
  }
  if (depth) skip(QObject::tr("Z depth and perspective (camera and columns)."));
  if (moves) {
    std::vector<LAffine> frames;
    for (const TAffine &inv : c.m_cameraInv)
      frames.push_back(LottieMath::toComposition(
          toMath(c.m_toPixels * inv * c.m_toStage), c.m_width, c.m_height));
    c.m_base.m_kind = Rig::World;
    c.m_base.m_ind  = c.m_nextInd++;
    c.m_nullLayers.append(baseLayer(3, c.m_base.m_ind, 0, "Camera", 0,
                                    c.m_count, transform(frames, 0, 100)));
  }
}

//-----------------------------------------------------------------------------

const std::vector<TAffine> &Exporter::world(Composition &c,
                                            const TStageObjectId &id) {
  auto it = c.m_worlds.find(id);
  if (it != c.m_worlds.end()) return it->second;
  std::vector<TAffine> frames;
  for (int f = 0; f < c.m_count; ++f)
    frames.push_back(c.m_xsheet->getPlacement(id, c.m_r0 + f));
  return c.m_worlds[id] = frames;
}

//-----------------------------------------------------------------------------

// The placements of a stage object relative to the frame of parent, in stage
// units, per frame.
std::vector<TAffine> Exporter::localPlacements(Composition &c,
                                               const TStageObjectId &id,
                                               const Rig &parent) {
  std::vector<TAffine> locals = world(c, id);
  if (parent.m_kind == Rig::View) {
    for (int f = 0; f < c.m_count; ++f)
      locals[f] = c.m_cameraInv[f] * locals[f];
  } else if (parent.m_kind == Rig::Object) {
    const std::vector<TAffine> &parentWorld = world(c, parent.m_id);
    for (int f = 0; f < c.m_count; ++f)
      locals[f] = safeInv(parentWorld[f]) * locals[f];
  }
  return locals;
}

//-----------------------------------------------------------------------------

// The Lottie transforms of a layer from its local placements (stage units).
std::vector<LAffine> Exporter::lottieLocals(const Composition &c,
                                            const std::vector<TAffine> &locals,
                                            const Rig &parent) const {
  std::vector<LAffine> frames;
  for (const TAffine &local : locals) {
    LAffine pixels = toMath(c.m_toPixels * local * c.m_toStage);
    frames.push_back(
        parent.m_kind == Rig::View
            ? LottieMath::toComposition(pixels, c.m_width, c.m_height)
            : LottieMath::flipY(pixels));
  }
  return frames;
}

//-----------------------------------------------------------------------------

// The frame of reference of the children of a stage object: its own null
// layer, made on the first call, or its parent's one when it never moves
// them (columns always get a layer, their drawings are placed in it).
Rig Exporter::rigOf(Composition &c, const TStageObjectId &id, int guard) {
  auto it = c.m_rigs.find(id);
  if (it != c.m_rigs.end()) return it->second;
  if (guard > 256) return c.m_base;
  c.m_rigs[id] = c.m_base;  // against parenting loops

  TStageObject *obj       = c.m_xsheet->getStageObject(id);
  TStageObjectId parentId = obj->getParent();
  Rig parent              = (parentId == TStageObjectId::NoneId)
                                ? c.m_base
                                : rigOf(c, parentId, guard + 1);

  std::vector<TAffine> locals = localPlacements(c, id, parent);
  bool still                  = !id.isColumn();
  for (size_t f = 0; still && f < locals.size(); ++f)
    if (!isIdentity(locals[f])) still = false;
  if (still) return c.m_rigs[id] = parent;

  if (obj->getPlasticSkeletonDeformation())
    skip(
        QObject::tr("Plastic deformations and mesh columns (drawings are "
                    "exported undeformed)."));

  Rig rig;
  rig.m_kind = Rig::Object;
  rig.m_id   = id;
  rig.m_ind  = c.m_nextInd++;
  c.m_nullLayers.append(baseLayer(
      3, rig.m_ind, parent.m_ind, QString::fromStdString(obj->getName()), 0,
      c.m_count, transform(lottieLocals(c, locals, parent), 0, 100)));
  return c.m_rigs[id] = rig;
}

//-----------------------------------------------------------------------------

const QJsonArray &Exporter::shapes(Composition &c, TXshSimpleLevel *sl,
                                   const TFrameId &fid) {
  auto key = std::make_tuple(sl, fid, c.m_toPixels.a11, c.m_toPixels.a22);
  auto it  = m_shapes.find(key);
  if (it != m_shapes.end()) return it->second;

  QJsonArray result;
  TVectorImageP vi = sl->getFrame(fid, false);
  if (vi) {
    const TPalette *palette = vi->getPalette();
    if (!palette) palette = sl->getPalette();
    QStringList notes;
    result = LottieExport::vectorShapes(vi.getPointer(), palette,
                                        c_flipY * c.m_toPixels, &notes);
    for (const QString &note : notes) skip(note);
  }
  return m_shapes[key] = result;
}

//-----------------------------------------------------------------------------

const ImageInfo &Exporter::image(TXshSimpleLevel *sl, const TFrameId &fid) {
  auto key = std::make_pair(sl, fid);
  auto it  = m_images.find(key);
  if (it != m_images.end()) return it->second;

  ImageInfo info;
  LottieExport::RasterImage raster;
  if (m_options.m_renderRaster && m_options.m_renderRaster(sl, fid, raster) &&
      !raster.m_png.isEmpty() && raster.m_width > 0 && raster.m_height > 0) {
    info.m_id     = QString("image_%1").arg(m_imageCount++);
    info.m_width  = raster.m_width;
    info.m_height = raster.m_height;
    info.m_offset = raster.m_offset;

    QJsonObject asset;
    asset["id"] = info.m_id;
    asset["w"]  = info.m_width;
    asset["h"]  = info.m_height;
    if (m_options.m_embedImages) {
      asset["u"] = "";
      asset["p"] = "data:image/png;base64," +
                   QString::fromLatin1(raster.m_png.toBase64());
      asset["e"] = 1;
    } else {
      LottieExport::ImageFile file;
      file.m_fileName = info.m_id + ".png";
      file.m_data     = raster.m_png;
      m_result.m_imageFiles.push_back(file);
      asset["u"] = m_options.m_imagesFolder;
      asset["p"] = file.m_fileName;
      asset["e"] = 0;
    }
    m_assets.append(asset);
  } else
    skip(QObject::tr("Raster drawings that could not be rendered."));
  return m_images[key] = info;
}

//-----------------------------------------------------------------------------

const CompInfo *Exporter::composition(TXsheet *child, int depth) {
  auto it = m_comps.find(child);
  if (it != m_comps.end()) return &it->second;
  if (!child || depth >= c_maxDepth || m_building.count(child)) {
    skip(QObject::tr("Sub-xsheets nested too deeply."));
    return nullptr;
  }

  m_building.insert(child);
  Composition c;
  initComposition(c, child, 0, std::max(1, child->getFrameCount()) - 1);
  buildComposition(c, depth + 1);
  m_building.erase(child);

  CompInfo info;
  info.m_id      = QString("comp_%1").arg(m_compCount++);
  info.m_width   = c.m_width;
  info.m_height  = c.m_height;
  info.m_toStage = c.m_toStage;

  QJsonObject asset;
  asset["id"]     = info.m_id;
  asset["nm"]     = info.m_id;
  asset["layers"] = layers(c);
  m_assets.append(asset);
  return &(m_comps[child] = info);
}

//-----------------------------------------------------------------------------

QJsonArray Exporter::layers(const Composition &c) const {
  QJsonArray result = c.m_contentLayers;
  for (int i = 0; i < c.m_nullLayers.size(); ++i)
    result.append(c.m_nullLayers.at(i));
  return result;
}

//-----------------------------------------------------------------------------

void Exporter::buildComposition(Composition &c, int depth) {
  // Lottie layers are top first
  for (int col = c.m_xsheet->getColumnCount() - 1; col >= 0; --col)
    addColumn(c, col, depth);
}

//-----------------------------------------------------------------------------

void Exporter::addColumn(Composition &c, int col, int depth) {
  TXsheet *xsh       = c.m_xsheet;
  TXshColumn *column = xsh->getColumn(col);
  if (!column || column->isEmpty() || !column->isPreviewVisible()) return;

  switch (column->getColumnType()) {
  case TXshColumn::eLevelType:
    break;
  case TXshColumn::eSoundType:
  case TXshColumn::eSoundTextType:
    skip(QObject::tr("Sound and sound text columns."));
    return;
  case TXshColumn::eZeraryFxType:
    skip(QObject::tr("FX columns."));
    return;
  case TXshColumn::eMeshType:
    skip(
        QObject::tr("Plastic deformations and mesh columns (drawings are "
                    "exported undeformed)."));
    return;
  default:
    return;
  }
  TXshLevelColumn *levelColumn = column->getLevelColumn();
  if (!levelColumn) return;

  if (column->isMask()) {
    skip(
        QObject::tr("Clipping mask columns (the masked columns are "
                    "exported unclipped)."));
    return;
  }
  if (column->getColorFilterId() != 0)
    skip(QObject::tr("Column color filters."));
  if (TFx *fx = column->getFx())
    if (fx->getOutputConnectionCount() > 0)
      skip(QObject::tr("FX (columns are exported without their effects)."));

  TStageObjectId columnId = TStageObjectId::ColumnId(col);
  TStageObject *columnObj = xsh->getStageObject(columnId);
  TStageObjectId parentId = columnObj->getParent();
  if (parentId.isColumn()) {
    TXshColumn *parentColumn = xsh->getColumn(parentId.getIndex());
    if (parentColumn && parentColumn->getColumnType() == TXshColumn::eMeshType)
      skip(
          QObject::tr("Plastic deformations and mesh columns (drawings are "
                      "exported undeformed)."));
  }

  // Runs of the same drawing, or of the same sub-xsheet
  struct Run {
    TXshSimpleLevel *m_sl = nullptr;
    TXshChildLevel *m_cl  = nullptr;
    TFrameId m_fid;
    int m_first = 0, m_last = 0;  // frames
    std::vector<int> m_childRows;
  };
  std::vector<Run> runs;
  bool depthReported = false, orderReported = false;
  for (int f = 0; f < c.m_count; ++f) {
    int row = c.m_r0 + f;
    if (!depthReported && columnObj->getZ(row) != 0.0) {
      skip(QObject::tr("Z depth and perspective (camera and columns)."));
      depthReported = true;
    }
    if (!orderReported && columnObj->getSO(row) != 0.0) {
      skip(
          QObject::tr("Stacking order (SO) changes: the column order is "
                      "kept."));
      orderReported = true;
    }

    TXshCell cell       = levelColumn->getRemappedCell(row);
    TXshLevel *xl       = cell.m_level.getPointer();
    TXshSimpleLevel *sl = xl ? xl->getSimpleLevel() : nullptr;
    TXshChildLevel *cl  = xl ? xl->getChildLevel() : nullptr;
    if (sl && sl->getType() != PLI_XSHLEVEL && sl->getType() != TZP_XSHLEVEL &&
        sl->getType() != OVL_XSHLEVEL) {
      skip(QObject::tr("Levels of other types."));
      sl = nullptr;
    }
    if (!sl && !cl) continue;

    Run *last = runs.empty() ? nullptr : &runs.back();
    bool same = last && last->m_last == f - 1 && last->m_sl == sl &&
                last->m_cl == cl && (cl || last->m_fid == cell.m_frameId);
    if (!same) {
      Run run;
      run.m_sl    = sl;
      run.m_cl    = cl;
      run.m_fid   = cell.m_frameId;
      run.m_first = f;
      runs.push_back(run);
      last = &runs.back();
    }
    last->m_last = f;
    if (cl)
      last->m_childRows.push_back(std::max(0, cell.m_frameId.getNumber() - 1));
  }
  if (runs.empty()) return;

  Rig rig         = rigOf(c, columnId);
  double opacity  = LottieMath::opacityPercent(column->getOpacity());
  QString colName = QString::fromStdString(columnObj->getName());

  // The layers of a column do not overlap in time: their order is free
  for (const Run &run : runs) {
    int ip = run.m_first, op = run.m_last + 1;
    QJsonObject layer;

    if (run.m_sl && run.m_sl->getType() == PLI_XSHLEVEL) {
      const QJsonArray &items = shapes(c, run.m_sl, run.m_fid);
      if (items.isEmpty()) continue;
      layer           = baseLayer(4, c.m_nextInd++, rig.m_ind,
                                  QString::fromStdWString(run.m_sl->getName()) + " " +
                                      QString::fromStdString(run.m_fid.expand()),
                                  ip, op,
                                  LottieMath::staticLayerTransform(LAffine(), opacity));
      layer["shapes"] = items;
    } else if (run.m_sl) {
      const ImageInfo &info = image(run.m_sl, run.m_fid);
      if (info.m_id.isEmpty()) continue;
      // Image pixels (y-down, from the top left corner) to the level
      // reference (y-up, from the image center moved by its offset), then
      // to the column's layer (pixels, y-down)
      double w = info.m_width, h = info.m_height;
      TAffine imageToLevel =
          TTranslation(info.m_offset.x - 0.5 * w, info.m_offset.y - 0.5 * h) *
          TAffine(1, 0, 0, 0, -1, h);
      TAffine aff = c_flipY * c.m_toPixels *
                    getDpiAffine(run.m_sl, run.m_fid, true) * imageToLevel;
      layer          = baseLayer(2, c.m_nextInd++, rig.m_ind,
                                 QString::fromStdWString(run.m_sl->getName()) + " " +
                                     QString::fromStdString(run.m_fid.expand()),
                                 ip, op,
                                 LottieMath::staticLayerTransform(toMath(aff), opacity));
      layer["refId"] = info.m_id;
    } else {
      TXsheet *child       = run.m_cl->getXsheet();
      const CompInfo *info = child ? composition(child, depth) : nullptr;
      if (!info) continue;

      // The precomposition (the child camera's view, y-down from its top
      // left corner) in the column's layer, scaled by the child camera Z as
      // in Stage::visit()
      TStageObjectTree *childTree  = child->getStageObjectTree();
      TStageObjectId childCameraId = childTree->getCurrentCameraId();
      TAffine compToCamera(1, 0, -0.5 * info->m_width, 0, -1,
                           0.5 * info->m_height);
      std::vector<LAffine> frames;
      std::vector<std::vector<double>> times;
      for (int f = ip; f < op; ++f) {
        int childRow = run.m_childRows[f - ip];
        double k     = (1000.0 + child->getZ(childCameraId, childRow)) / 1000.0;
        if (!std::isfinite(k) || k <= 1e-6) k = 1.0;
        frames.push_back(toMath(c_flipY * c.m_toPixels * TScale(1.0 / k) *
                                info->m_toStage * compToCamera));
        times.push_back(std::vector<double>(1, childRow / m_fps));
      }
      layer          = baseLayer(0, c.m_nextInd++, rig.m_ind,
                                 QString::fromStdWString(run.m_cl->getName()), ip, op,
                                 transform(frames, ip, opacity));
      layer["refId"] = info->m_id;
      layer["w"]     = (int)std::lround(info->m_width);
      layer["h"]     = (int)std::lround(info->m_height);
      layer["tm"]    = LottieMath::animatedProperty(
          times, ip, LottieMath::c_timeUnit, 0.0, true, true);
    }
    if (layer.isEmpty()) continue;
    if (layer["nm"].toString().trimmed().isEmpty()) layer["nm"] = colName;
    c.m_contentLayers.append(layer);
  }
}

}  // namespace

//=============================================================================

QJsonArray LottieExport::vectorShapes(const TVectorImage *vi,
                                      const TPalette *palette,
                                      const TAffine &toLayer,
                                      QStringList *notes) {
  if (!vi) return QJsonArray();

  ShapeContext context;
  context.m_palette = palette ? palette : vi->getPalette();
  context.m_toLayer = toLayer;
  context.m_scale   = std::sqrt(std::abs(toLayer.det()));
  if (!std::isfinite(context.m_scale)) context.m_scale = 1.0;
  context.m_notes = notes;

  TThread::MutexLocker lock(vi->getMutex());

  // As tglDraw() draws them: group by group, the regions of the group first
  UINT strokeCount = vi->getStrokeCount();
  UINT strokeIndex = 0;
  while (strokeIndex < strokeCount) {
    UINT current = strokeIndex;
    for (UINT r = 0; r < vi->getRegionCount(); ++r)
      if (vi->sameGroupStrokeAndRegion(current, r))
        addRegion(vi->getRegion(r), context);
    while (strokeIndex < strokeCount && vi->sameGroup(strokeIndex, current)) {
      addStroke(vi->getStroke(strokeIndex), context);
      ++strokeIndex;
    }
  }

  // Lottie shapes are top first
  QJsonArray result;
  for (int i = context.m_items.size() - 1; i >= 0; --i)
    result.append(context.m_items.at(i));
  return result;
}

//-----------------------------------------------------------------------------

LottieExport::Result LottieExport::exportXsheet(TXsheet *xsheet,
                                                const Options &options) {
  Result result;
  if (!xsheet) return result;

  Exporter exporter(options, result);

  int r0 = std::max(0, options.m_r0);
  int r1 = (options.m_r1 < 0) ? xsheet->getFrameCount() - 1 : options.m_r1;
  if (r1 < r0) r1 = r0;

  Composition c;
  exporter.initComposition(c, xsheet, r0, r1);
  exporter.buildComposition(c, 0);
  result.m_contentLayers = c.m_contentLayers.size();

  QJsonObject root;
  root["v"]  = "5.7.4";
  root["nm"] = options.m_name.isEmpty() ? QString("animation") : options.m_name;
  root["fr"] = LottieMath::num(exporter.m_fps, 1e-3);
  root["ip"] = 0;
  root["op"] = c.m_count;
  root["w"]  = (int)std::lround(c.m_width);
  root["h"]  = (int)std::lround(c.m_height);
  root["ddd"]     = 0;
  root["assets"]  = exporter.m_assets;
  root["layers"]  = exporter.layers(c);
  root["markers"] = QJsonArray();

  // Every number went through LottieMath::num(): this is only a safety net
  if (!LottieMath::allFinite(root))
    root = LottieMath::sanitized(root).toObject();
  result.m_animation = root;
  return result;
}
