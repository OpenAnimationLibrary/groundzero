#include "typetoolvector.h"

#include "tcurves.h"
#include "tregion.h"
#include "tstroke.h"

namespace TypeToolText {
namespace {

void paintOutlineRegion(TRegion *region, const QList<QPainterPath> &paths,
                        int styleId) {
  TPointD point;
  bool filled = false;
  if (region->getInternalPoint(point)) {
    for (const QPainterPath &path : paths) {
      if (path.contains(QPointF(point.x, point.y))) {
        filled = true;
        break;
      }
    }
  }
  region->setStyle(filled ? styleId : 0);
  for (UINT i = 0; i < region->getSubregionCount(); ++i)
    paintOutlineRegion(region->getSubregion(i), paths, styleId);
}

}  // namespace

TVectorImageP vectorizeOutlineLayout(const OutlineLayout &layout, double scale,
                                     int styleId) {
  return vectorizeOutlineLayout(layout, scale, styleId, {});
}

// #lizard forgive: preserve native conversion flow; only add pre-paint inspection.
TVectorImageP vectorizeOutlineLayout(const OutlineLayout &layout, double scale,
                                     int styleId, const VectorInspection &inspection) {
  TVectorImageP image = new TVectorImage;
  QList<QPainterPath> paths;
  const QTransform transform(scale, 0, 0, -scale, 0, 0);
  for (const QPainterPath &outline : layout.paths) {
    const QPainterPath path = transform.map(outline);
    paths.append(path);
    std::vector<TThickPoint> points;
    auto finishContour = [&]() {
      if (points.size() >= 3) {
        if (points.back() != points.front()) {
          points.push_back((points.back() + points.front()) * 0.5);
          points.push_back(points.front());
        }
        TStroke *stroke = new TStroke(points);
        stroke->setSelfLoop(true);
        stroke->setStyle(styleId);
        image->addStroke(stroke);
      }
      points.clear();
    };
    for (int i = 0; i < path.elementCount(); ++i) {
      const QPainterPath::Element element = path.elementAt(i);
      const TThickPoint point(element.x, element.y, 0);
      if (element.type == QPainterPath::MoveToElement) {
        finishContour();
        points.push_back(point);
      } else if (element.type == QPainterPath::LineToElement &&
                 !points.empty()) {
        points.push_back((points.back() + point) * 0.5);
        points.push_back(point);
      } else if (element.type == QPainterPath::CurveToElement &&
                 !points.empty() && i + 2 < path.elementCount()) {
        const QPainterPath::Element control = path.elementAt(++i);
        const QPainterPath::Element end     = path.elementAt(++i);
        std::vector<TThickQuadratic *> quadratics;
        computeQuadraticsFromCubic(
            points.back(), point, TThickPoint(control.x, control.y, 0),
            TThickPoint(end.x, end.y, 0), 0.09, quadratics);
        for (TThickQuadratic *quadratic : quadratics) {
          points.push_back(quadratic->getP1());
          points.push_back(quadratic->getP2());
          delete quadratic;
        }
      }
    }
    finishContour();
  }
  if (image->getStrokeCount() > 1) image->group(0, image->getStrokeCount());
  image->enableRegionComputing(true, true);
  image->findRegions();
  if (inspection) inspection(image);
  for (UINT i = 0; i < image->getRegionCount(); ++i)
    paintOutlineRegion(image->getRegion(i), paths, styleId);
  return image;
}

OutlineLayout inspectOutlineLayout(const TypeToolTextEntry &entry,
                                    const LayoutInspection &inspection) {
  return buildOutlineLayout(entry, false, inspection);
}

}  // namespace TypeToolText
