// Regression test of the generic deformer layer (TDeformer) through the
// Envelope deformer: the mean value mapping, its inverse, and the raster warp;
// of the deformer stack (TDeformerStack) built on it; and of the free-form
// lattice deformer (TFreeFormDeformer); and of the curve deformer
// (TCurveDeformer) and its Bezier chain; and of the bone deformer
// (TBoneDeformer).

#include "toonz/tcurvedeformer.h"
#include "toonz/tbonedeformer.h"
#include "toonz/tdeformer.h"
#include "toonz/tdeformerstack.h"
#include "toonz/tfreeformdeformer.h"
#include "traster.h"

#include "tstroke.h"
#include "tvectorimage.h"
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

bool isClose(const TPointD &a, const TPointD &b, double tolerance) {
  return norm(a - b) <= tolerance;
}

std::vector<TPointD> square() {
  const double s = 100.0;
  return {TPointD(-s, -s), TPointD(0.0, -s), TPointD(s, -s), TPointD(s, 0.0),
          TPointD(s, s),   TPointD(0.0, s),  TPointD(-s, s), TPointD(-s, 0.0)};
}

std::vector<TPointD> samplePoints() {
  return {TPointD(0.0, 0.0),     TPointD(30.0, -45.0),    TPointD(-99.0, 99.0),
          TPointD(100.0, 50.0),  TPointD(-100.0, -100.0), TPointD(170.0, 20.0),
          TPointD(-250.0, -80.0)};
}

//------------------------------------------------------------------

void testIdentity() {
  TEnvelopeDeformer deformer(square(), square());
  require(deformer.isValid(), "identity: not valid");
  require(deformer.isIdentity(0.0), "identity: not identity");

  for (const TPointD &p : samplePoints()) {
    TPointD q;
    require(deformer.undeform(p, 0.0, q), "identity: undeform failed");
    require(isClose(q, p, 1e-9), "identity: undeform moved a point");
    require(isClose(deformer.deform(p, 0.0), p, 1e-9),
            "identity: deform moved a point");
  }
}

//------------------------------------------------------------------

void testDegenerate() {
  std::vector<TPointD> line(8);
  for (int i = 0; i < 8; ++i) line[i] = TPointD(10.0 * i, 10.0 * i);

  require(!TEnvelopeDeformer(line, square()).isValid(),
          "degenerate: a collinear rest envelope is valid");
  require(!TEnvelopeDeformer(square(), std::vector<TPointD>(3)).isValid(),
          "degenerate: envelopes of different sizes are valid");
}

//------------------------------------------------------------------

void testAffinePrecision() {
  // Mean value coordinates reproduce affine maps exactly, inside and outside
  TAffine aff = TTranslation(12.0, -7.0) * TRotation(30.0) * TScale(1.5, 0.75);

  std::vector<TPointD> rest = square(), deformed = square();
  for (TPointD &p : deformed) p = aff * p;

  TEnvelopeDeformer deformer(rest, deformed);
  for (const TPointD &p : samplePoints()) {
    TPointD q;
    require(deformer.undeform(aff * p, 0.0, q), "affine: undeform failed");
    require(isClose(q, p, 1e-6), "affine: wrong undeform");
    require(isClose(deformer.deform(p, 0.0), aff * p, 1e-4),
            "affine: wrong deform");
  }
}

//------------------------------------------------------------------

void testBendAndRoundTrip() {
  std::vector<TPointD> rest = square(), deformed = square();
  deformed[2] += TPointD(40.0, 30.0);
  deformed[5] += TPointD(-20.0, 10.0);

  TEnvelopeDeformer deformer(rest, deformed);
  require(!deformer.isIdentity(1.0), "bend: identity");

  // The envelope's vertices and edges move onto the deformed ones
  for (int i = 0; i < 8; ++i) {
    TPointD q;
    require(deformer.undeform(deformed[i], 0.0, q) && isClose(q, rest[i], 1e-9),
            "bend: a deformed vertex is not mapped to its rest vertex");
    require(isClose(deformer.deform(rest[i], 0.0), deformed[i], 1e-4),
            "bend: a rest vertex is not mapped to its deformed vertex");

    int j = (i + 1) % 8;
    TPointD mid((deformed[i] + deformed[j]) * 0.5);
    require(deformer.undeform(mid, 0.0, q) &&
                isClose(q, (rest[i] + rest[j]) * 0.5, 1e-6),
            "bend: an edge is not mapped linearly");
  }

  // deform() inverts undeform() inside the envelope
  for (int y = -90; y <= 90; y += 30)
    for (int x = -90; x <= 90; x += 30) {
      TPointD p(x, y), q;
      require(deformer.undeform(deformer.deform(p, 0.0), 0.0, q),
              "bend: undeform failed");
      require(isClose(q, p, 1e-3), "bend: deform does not invert undeform");
    }

  // The bounds contain the deformed envelope
  TRectD bounds =
      deformer.deformedBounds(TRectD(-100.0, -100.0, 100.0, 100.0), 0.0);
  for (const TPointD &p : deformed)
    require(bounds.x0 <= p.x && p.x <= bounds.x1 && bounds.y0 <= p.y &&
                p.y <= bounds.y1,
            "bend: the deformed bounds miss a deformed vertex");
}

//------------------------------------------------------------------

void testRasterWarp() {
  const int size = 8;

  TRaster32P in(size, size), out(size, size);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x)
      in->pixels(y)[x] = TPixel32(30 * x, 30 * y, 100, 255);

  // Identity: the raster is copied
  TEnvelopeDeformer identity(square(), square());
  identity.warpRaster(out, TPointD(), in, TPointD(), 0.0);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x)
      require(out->pixels(y)[x] == in->pixels(y)[x],
              "warp: the identity changed a pixel");

  // A translation by 3 pixels, of a source placed at (-1, 0): the output
  // pixel x shows the source pixel x - 2. (Pixel 1 samples the source's
  // border, which rounding may put on either side.)
  std::vector<TPointD> moved = square();
  for (TPointD &p : moved) p += TPointD(3.0, 0.0);

  TEnvelopeDeformer translation(square(), moved);
  translation.warpRaster(out, TPointD(), in, TPointD(-1.0, 0.0), 0.0);
  for (int y = 0; y < size; ++y) {
    require(out->pixels(y)[0] == TPixel32::Transparent,
            "warp: a pixel without source is not transparent");
    for (int x = 2; x < size; ++x)
      require(out->pixels(y)[x] == in->pixels(y)[x - 2],
              "warp: wrong translated pixel");
  }
}

//------------------------------------------------------------------

std::shared_ptr<const TDeformer> bentLayer(const TPointD &d2,
                                           const TPointD &d5) {
  std::vector<TPointD> deformed = square();
  deformed[2] += d2;
  deformed[5] += d5;
  return std::make_shared<TEnvelopeDeformer>(square(), deformed);
}

//------------------------------------------------------------------

void testStackIdentity() {
  // No layer, and layers that do not move anything
  TDeformerStack empty;
  TDeformerStack stack;
  stack.setScale(200.0);
  for (int i = 0; i < 3; ++i)
    stack.addLayer(std::make_shared<TEnvelopeDeformer>(square(), square()),
                   TDeformerWeightMap::makeUniform(0.25 * (i + 1)));
  require(stack.getLayerCount() == 3, "stack: wrong layer count");

  for (const TPointD &p : samplePoints()) {
    TPointD q;
    require(empty.undeform(p, 0.0, q) && isClose(q, p, 0.0) &&
                isClose(empty.deform(p, 0.0), p, 0.0),
            "stack: an empty stack is not the identity");

    require(isClose(stack.deform(p, 0.0), p, 1e-9),
            "stack: identity layers moved a point");
    require(stack.undeform(p, 0.0, q) && isClose(q, p, 1e-9),
            "stack: identity layers moved a point back");
  }
}

//------------------------------------------------------------------

void testStackWeights() {
  std::shared_ptr<const TDeformer> bent =
      bentLayer(TPointD(40.0, 30.0), TPointD(-20.0, 10.0));

  // Weight 0 is the identity, and the weight is clamped to [0, 1]
  TDeformerStack zero;
  zero.setScale(200.0);
  zero.addLayer(bent, TDeformerWeightMap::makeUniform(0.0));
  zero.addLayer(bent, TDeformerWeightMap::makeUniform(-3.0));

  // Weight 1 is the layer itself
  TDeformerStack one, over;
  one.setScale(200.0);
  one.addLayer(bent, TDeformerWeightMap::makeUniform(1.0));
  over.setScale(200.0);
  over.addLayer(bent, TDeformerWeightMap::makeUniform(7.0));

  // Weight 1/2 is halfway between the point and the layer's image
  TDeformerStack half;
  half.setScale(200.0);
  half.addLayer(bent, TDeformerWeightMap::makeUniform(0.5));

  for (const TPointD &p : samplePoints()) {
    TPointD q, expected;
    require(isClose(zero.deform(p, 0.0), p, 0.0),
            "weights: weight 0 moved a point");
    require(zero.undeform(p, 0.0, q) && isClose(q, p, 0.0),
            "weights: weight 0 moved a point back");

    for (const TDeformerStack *s : {&one, &over}) {
      require(isClose(s->deform(p, 0.0), bent->deform(p, 0.0), 1e-12),
              "weights: weight 1 is not the layer's deform");

      TPointD expectedBack;
      bool ok = bent->undeform(p, 0.0, expectedBack);
      require(s->undeform(p, 0.0, q) == ok &&
                  (!ok || isClose(q, expectedBack, 1e-12)),
              "weights: weight 1 is not the layer's undeform");
    }

    expected = (p + bent->deform(p, 0.0)) * 0.5;
    require(isClose(half.deform(p, 0.0), expected, 1e-9),
            "weights: wrong half weight");
  }
}

//------------------------------------------------------------------

void testWeightMap() {
  TDeformerWeightMap uniform = TDeformerWeightMap::makeUniform(0.4);
  require(uniform.isUniform() && uniform.weightAt(TPointD(1e5, -1e5)) == 0.4,
          "map: wrong uniform weight");
  require(TDeformerWeightMap().weightAt(TPointD()) == 1.0,
          "map: the default weight is not 1");

  TPointD c(10.0, 20.0);
  TDeformerWeightMap map =
      TDeformerWeightMap::makeDistance(0.8, c, 50.0, 150.0);
  require(!map.isUniform(), "map: a distance map is uniform");
  require(map.weightAt(c) == 0.8 && map.weightAt(c + TPointD(50.0, 0.0)) == 0.8,
          "map: the weight is not full within the inner radius");
  require(map.weightAt(c + TPointD(0.0, 150.0)) == 0.0 &&
              map.weightAt(c + TPointD(-400.0, 0.0)) == 0.0,
          "map: the weight is not 0 from the outer radius");
  require(std::fabs(map.weightAt(c + TPointD(100.0, 0.0)) - 0.4) < 1e-12,
          "map: the weight is not halved midway");

  double prev = 1.0;
  for (double d = 50.0; d <= 150.0; d += 5.0) {
    double w = map.weightAt(c + TPointD(0.0, -d));
    require(w <= prev, "map: the weight is not decreasing");
    prev = w;
  }

  // A sharp map: full weight up to the radius, and none after
  TDeformerWeightMap sharp =
      TDeformerWeightMap::makeDistance(1.0, c, 30.0, 10.0);
  require(sharp.weightAt(c + TPointD(30.0, 0.0)) == 1.0 &&
              sharp.weightAt(c + TPointD(31.0, 0.0)) == 0.0,
          "map: wrong sharp falloff");
}

//------------------------------------------------------------------

void testStackInverse() {
  // Layers of different weights, one of them local, on top of each other
  TDeformerStack stack;
  stack.setScale(283.0);
  stack.addLayer(bentLayer(TPointD(40.0, 30.0), TPointD(-20.0, 10.0)),
                 TDeformerWeightMap::makeUniform(0.6));
  stack.addLayer(
      bentLayer(TPointD(-15.0, 25.0), TPointD(10.0, -30.0)),
      TDeformerWeightMap::makeDistance(0.9, TPointD(30.0, 20.0), 60.0, 160.0));
  stack.addLayer(bentLayer(TPointD(10.0, 10.0), TPointD(20.0, 0.0)),
                 TDeformerWeightMap::makeUniform(1.0));

  for (int y = -90; y <= 90; y += 30)
    for (int x = -90; x <= 90; x += 30) {
      TPointD p(x, y), q;
      TPointD d = stack.deform(p, 0.0);
      require(stack.undeform(d, 0.0, q), "inverse: undeform failed");
      require(isClose(q, p, 2e-3), "inverse: undeform does not invert deform");
    }

  // A point outside the falloff of a local layer is not moved, in either
  // direction
  TDeformerStack local;
  local.setScale(283.0);
  local.addLayer(bentLayer(TPointD(40.0, 30.0), TPointD(-20.0, 10.0)),
                 TDeformerWeightMap::makeDistance(1.0, TPointD(), 20.0, 40.0));
  TPointD outside(300.0, -300.0), q;
  require(isClose(local.deform(outside, 0.0), outside, 0.0),
          "inverse: a point outside the falloff moved");
  require(local.undeform(outside, 0.0, q) && isClose(q, outside, 0.0),
          "inverse: a point outside the falloff moved back");

  // The bounds of the stack contain the images of the envelope's vertices
  TRectD bounds =
      stack.deformedBounds(TRectD(-100.0, -100.0, 100.0, 100.0), 0.0);
  for (const TPointD &v : square()) {
    TPointD d = stack.deform(v, 0.0);
    require(bounds.x0 <= d.x && d.x <= bounds.x1 && bounds.y0 <= d.y &&
                d.y <= bounds.y1,
            "inverse: the stack's bounds miss a deformed vertex");
  }
}

//------------------------------------------------------------------

// Lattice of nx x ny cells over rect, deformed by map
std::vector<TPointD> latticeMapped(const TRectD &rect, int nx, int ny,
                                   const TAffine &map) {
  std::vector<TPointD> points;
  for (int j = 0; j <= ny; ++j)
    for (int i = 0; i <= nx; ++i)
      points.push_back(map * TPointD(rect.x0 + rect.getLx() * i / nx,
                                     rect.y0 + rect.getLy() * j / ny));
  return points;
}

// Lattice displaced by a smooth pseudo-random field of the given amplitude
std::vector<TPointD> latticeRandom(const TRectD &rect, int nx, int ny,
                                   double amplitude) {
  std::vector<TPointD> points = latticeMapped(rect, nx, ny, TAffine());
  for (size_t n = 0; n < points.size(); ++n) {
    points[n].x += amplitude * std::sin(1.7 * n + 0.3);
    points[n].y += amplitude * std::cos(2.3 * n + 1.1);
  }
  return points;
}

std::vector<TPointD> latticeSamples() {
  return {
      TPointD(0.0, 0.0),     TPointD(30.0, -45.0),    TPointD(-99.0, 99.0),
      TPointD(100.0, 50.0),  TPointD(-100.0, -100.0), TPointD(61.3, 17.9),
      TPointD(-37.7, 80.2),  TPointD(170.0, 20.0),    TPointD(-250.0, -80.0),
      TPointD(30.0, 1000.0), TPointD(-5000.0, 4000.0)};
}

//------------------------------------------------------------------

void testFreeFormIdentity() {
  TRectD rect(-100.0, -100.0, 100.0, 100.0);
  TFreeFormDeformer deformer(rect, 3, 3, latticeMapped(rect, 3, 3, TAffine()));
  require(deformer.isValid(), "free-form identity: not valid");
  require(deformer.isIdentity(1e-9), "free-form identity: not identity");

  for (const TPointD &p : latticeSamples()) {
    require(isClose(deformer.deform(p, 0.0), p, 1e-9),
            "free-form identity: deform moved a point");

    TPointD q;
    require(deformer.undeform(p, 0.0, q),
            "free-form identity: undeform failed");
    require(isClose(q, p, 1e-9), "free-form identity: undeform moved a point");
  }

  require(isClose(deformer.getRestPoint(1, 2),
                  TPointD(-33.3333333333, 33.3333333333), 1e-6),
          "free-form identity: wrong rest point");
}

//------------------------------------------------------------------

void testFreeFormInvalid() {
  TRectD rect(-100.0, -100.0, 100.0, 100.0);
  std::vector<TPointD> lattice = latticeMapped(rect, 3, 3, TAffine());

  TPointD p(12.0, 34.0), q;
  TFreeFormDeformer wrongCount(rect, 2, 3, lattice);
  require(!wrongCount.isValid() && !wrongCount.isIdentity(1.0) &&
              isClose(wrongCount.deform(p, 0.0), p, 0.0) &&
              wrongCount.undeform(p, 0.0, q) && isClose(q, p, 0.0),
          "free-form invalid: wrong point count");

  require(
      !TFreeFormDeformer(TRectD(0.0, 0.0, 0.0, 10.0), 3, 3, lattice).isValid(),
      "free-form invalid: flat rect");
  require(!TFreeFormDeformer(rect, 0, 3, lattice).isValid(),
          "free-form invalid: no cells");
  require(!TFreeFormDeformer(rect, 3, 3, lattice, TScale(0.0, 1.0)).isValid(),
          "free-form invalid: singular placement");
}

//------------------------------------------------------------------

void testFreeFormTranslation() {
  TRectD rect(-100.0, -100.0, 100.0, 100.0);
  TPointD t(13.0, -8.5);
  TFreeFormDeformer deformer(rect, 3, 3,
                             latticeMapped(rect, 3, 3, TTranslation(t)));
  require(!deformer.isIdentity(1e-3), "free-form translation: identity");
  // The control points move by 15.5
  require(!deformer.isIdentity(15.0), "free-form translation: tolerance");
  require(deformer.isIdentity(16.0), "free-form translation: tolerance");

  // A translation moves the whole plane, inside and outside the lattice
  for (const TPointD &p : latticeSamples()) {
    require(isClose(deformer.deform(p, 0.0), p + t, 1e-9),
            "free-form translation: wrong deform");

    TPointD q;
    require(deformer.undeform(p + t, 0.0, q),
            "free-form translation: undeform failed");
    require(isClose(q, p, 1e-9), "free-form translation: wrong undeform");
  }
}

//------------------------------------------------------------------

void testFreeFormAffine() {
  // The B-spline reproduces affine maps, up to the border, and the corners
  TAffine aff = TTranslation(12.0, -7.0) * TRotation(30.0) * TScale(1.5, 0.75);
  TRectD rect(-100.0, -60.0, 140.0, 100.0);

  for (int cells = 1; cells <= 4; ++cells) {
    TFreeFormDeformer deformer(rect, cells, cells + 1,
                               latticeMapped(rect, cells, cells + 1, aff));
    require(isClose(deformer.getDeformedPoint(cells, 0),
                    aff * TPointD(rect.x1, rect.y0), 1e-9),
            "free-form affine: wrong deformed point");

    for (const TPointD &p : latticeSamples()) {
      if (p.x < rect.x0 || p.x > rect.x1 || p.y < rect.y0 || p.y > rect.y1)
        continue;

      require(isClose(deformer.deform(p, 0.0), aff * p, 1e-8),
              "free-form affine: wrong deform");

      TPointD q;
      require(deformer.undeform(aff * p, 0.0, q),
              "free-form affine: undeform failed");
      require(isClose(q, p, 1e-6), "free-form affine: wrong undeform");
    }

    // The corners are interpolated
    TPointD c(rect.x1, rect.y1);
    require(isClose(deformer.deform(c, 0.0), aff * c, 1e-8),
            "free-form affine: corner not reached");
  }
}

//------------------------------------------------------------------

void testFreeFormRoundTrip() {
  TRectD rect(-100.0, -100.0, 100.0, 100.0);

  // Placed through a similarity: the deformer is the unplaced one, seen
  // through the map.
  TAffine placement =
      TTranslation(40.0, -25.0) * TRotation(-20.0) * TScale(1.3);

  std::vector<TPointD> lattice = latticeRandom(rect, 3, 3, 12.0);
  TFreeFormDeformer plain(rect, 3, 3, lattice);
  require(!plain.isIdentity(1.0), "free-form round trip: identity");

  TFreeFormDeformer deformer(rect, 3, 3, lattice, placement);

  for (double y = -140.0; y <= 140.0; y += 13.7)
    for (double x = -140.0; x <= 140.0; x += 11.3) {
      TPointD p(x, y);

      // Same deformation through the placement
      require(isClose(deformer.deform(placement * p, 0.0),
                      placement * plain.deform(p, 0.0), 1e-8),
              "free-form round trip: placement changes the deformation");

      TPointD d = plain.deform(p, 0.0), q;
      require(plain.undeform(d, 0.0, q), "free-form round trip: undeform");
      require(isClose(q, p, 1e-6), "free-form round trip: wrong inverse");

      TPointD pl = placement * p, ql;
      require(deformer.undeform(deformer.deform(pl, 0.0), 0.0, ql),
              "free-form round trip: placed undeform");
      require(isClose(ql, pl, 1e-6), "free-form round trip: placed inverse");

      // And the other way round
      TPointD e = plain.deform(q, 0.0);
      require(isClose(e, d, 1e-6), "free-form round trip: wrong forward");
    }

  // The displacement is held at the border's value outside the lattice
  TPointD border(100.0, 20.0), far(900.0, 20.0);
  require(isClose(plain.deform(far, 0.0) - far,
                  plain.deform(border, 0.0) - border, 1e-9),
          "free-form round trip: displacement not held outside");

  TPointD corner(100.0, 100.0), beyond(300.0, 700.0);
  require(isClose(plain.deform(beyond, 0.0) - beyond,
                  plain.deform(corner, 0.0) - corner, 1e-9),
          "free-form round trip: displacement not held beyond a corner");

  TPointD q;
  require(plain.undeform(plain.deform(beyond, 0.0), 0.0, q) &&
              isClose(q, beyond, 1e-6),
          "free-form round trip: wrong inverse outside");
}

//------------------------------------------------------------------

void testFreeFormFold() {
  // A lattice squashed to a line is not invertible: undeform still answers
  TRectD rect(-100.0, -100.0, 100.0, 100.0);
  std::vector<TPointD> lattice = latticeMapped(rect, 3, 3, TScale(1.0, 0.0));
  TFreeFormDeformer deformer(rect, 3, 3, lattice);
  require(deformer.isValid(), "free-form fold: not valid");

  TPointD q;
  require(deformer.undeform(TPointD(10.0, 5.0), 0.0, q),
          "free-form fold: undeform failed");
  require(std::isfinite(q.x) && std::isfinite(q.y),
          "free-form fold: undeform gave a non finite point");
}

//------------------------------------------------------------------

void testFreeFormStack() {
  // A free-form layer in a stack, with the raster warp and the bounds
  TRectD rect(-100.0, -100.0, 100.0, 100.0);
  TPointD t(20.0, 0.0);
  auto layer = std::make_shared<TFreeFormDeformer>(
      rect, 3, 3, latticeMapped(rect, 3, 3, TTranslation(t)));

  TDeformerStack stack;
  stack.addLayer(layer, TDeformerWeightMap::makeUniform(0.5));

  TPointD p(10.0, 10.0), q;
  require(isClose(stack.deform(p, 0.0), p + t * 0.5, 1e-9),
          "free-form stack: wrong deform");
  require(stack.undeform(p + t * 0.5, 0.0, q) && isClose(q, p, 2e-3),
          "free-form stack: wrong undeform");

  TRectD bounds = layer->deformedBounds(rect, 0.0);
  require(bounds.x0 <= -80.0 && bounds.x1 >= 120.0 && bounds.y0 <= -100.0 &&
              bounds.y1 >= 100.0,
          "free-form stack: bounds too small");
}

}  // namespace

//------------------------------------------------------------------

//------------------------------------------------------------------
// Curve deformer

typedef TBezierChain::Anchor Anchor;

// Anchors at x = -150, -50, 50 and 150, with handles a third of the way to the
// neighbours, on the curve y = scale * (x + 150)^2 / 1000 (a straight line
// for scale 0), moved by offset.
std::vector<Anchor> curveAnchors(double scale, const TPointD &offset) {
  std::vector<Anchor> anchors;
  for (int i = 0; i < 4; ++i) {
    double x     = -150.0 + 100.0 * i;
    double y     = scale * (x + 150.0) * (x + 150.0) / 1000.0;
    double slope = scale * 2.0 * (x + 150.0) / 1000.0;
    TPointD h(100.0 / 3.0, 100.0 / 3.0 * slope);
    TPointD pos(TPointD(x, y) + offset);
    anchors.push_back(Anchor(pos, pos - h, pos + h));
  }
  return anchors;
}

//------------------------------------------------------------------

void testBezierChain() {
  TBezierChain chain(curveAnchors(0.0, TPointD()));
  require(chain.isValid(), "chain: not valid");
  require(std::fabs(chain.getLength() - 300.0) < 1e-6,
          "chain: wrong length of a straight chain");
  require(isClose(chain.getPoint(0.0), TPointD(-150.0, 0.0), 1e-9) &&
              isClose(chain.getPoint(0.5), TPointD(0.0, 0.0), 1e-6) &&
              isClose(chain.getPoint(1.0), TPointD(150.0, 0.0), 1e-9),
          "chain: wrong points");
  require(isClose(chain.getPoint(-0.1), TPointD(-180.0, 0.0), 1e-6) &&
              isClose(chain.getPoint(1.1), TPointD(180.0, 0.0), 1e-6),
          "chain: wrong extension beyond the ends");
  require(isClose(chain.getNormal(0.3), TPointD(0.0, 1.0), 1e-9),
          "chain: the normal is not the tangent rotated counterclockwise");

  TBezierChain::Projection pr;
  require(chain.project(TPointD(30.0, -20.0), pr), "chain: project failed");
  require(std::fabs(pr.m_u - 0.6) < 1e-6 && std::fabs(pr.m_d + 20.0) < 1e-6 &&
              std::fabs(pr.m_distance - 20.0) < 1e-6,
          "chain: wrong projection");
  require(chain.project(TPointD(-200.0, 15.0), pr),
          "chain: project beyond the start failed");
  require(std::fabs(pr.m_u + 50.0 / 300.0) < 1e-6 &&
              std::fabs(pr.m_d - 15.0) < 1e-6 &&
              std::fabs(pr.m_distance - std::sqrt(50.0 * 50.0 + 15.0 * 15.0)) <
                  1e-6,
          "chain: wrong projection beyond the start");

  // The projection inverts the parametrization on a bent chain too
  TBezierChain bent(curveAnchors(1.0, TPointD()));
  for (double u = -0.2; u <= 1.2; u += 0.1) {
    for (double d = -30.0; d <= 30.0; d += 15.0) {
      TPointD p = bent.getPoint(u) + bent.getNormal(u) * d;
      require(bent.project(p, pr), "chain: bent project failed");
      require(std::fabs(pr.m_u - u) < 1e-6 && std::fabs(pr.m_d - d) < 1e-5,
              "chain: the projection does not invert the parametrization");
    }
  }

  require(!TBezierChain(std::vector<Anchor>(1)).isValid(),
          "chain: a single anchor is valid");
}

//------------------------------------------------------------------

void testCurveIdentity() {
  TCurveDeformer deformer(curveAnchors(1.0, TPointD()),
                          curveAnchors(1.0, TPointD()), 40.0, 120.0);
  require(deformer.isValid(), "curve identity: not valid");
  require(deformer.isIdentity(0.0), "curve identity: not identity");

  for (const TPointD &p : samplePoints()) {
    TPointD q;
    require(deformer.undeform(p, 0.0, q), "curve identity: undeform failed");
    require(isClose(q, p, 1e-6), "curve identity: undeform moved a point");
    require(isClose(deformer.deform(p, 0.0), p, 1e-6),
            "curve identity: deform moved a point");
  }

  // Different numbers of anchors, or a degenerate curve, are not valid
  std::vector<Anchor> three = curveAnchors(0.0, TPointD());
  three.pop_back();
  require(!TCurveDeformer(three, curveAnchors(0.0, TPointD()), 40.0, 120.0)
               .isValid(),
          "curve identity: curves of different sizes are valid");
  require(!TCurveDeformer(std::vector<Anchor>(4), std::vector<Anchor>(4), 40.0,
                          120.0)
               .isValid(),
          "curve identity: a degenerate curve is valid");
}

//------------------------------------------------------------------

void testCurveTranslation() {
  // Moving every anchor and handle translates the points near the curve, as
  // far as the width, and fades out to the falloff
  TPointD move(30.0, 20.0);
  TCurveDeformer deformer(curveAnchors(0.0, TPointD()), curveAnchors(0.0, move),
                          40.0, 120.0);
  require(!deformer.isIdentity(1e-6), "curve translation: identity");

  const TPointD nearPoints[] = {TPointD(0.0, 10.0),    TPointD(-80.0, -30.0),
                                TPointD(120.0, 40.0),  TPointD(-170.0, 10.0),
                                TPointD(180.0, -10.0), TPointD(150.0, 0.0)};
  for (const TPointD &p : nearPoints) {
    require(isClose(deformer.deform(p, 0.0), p + move, 1e-6),
            "curve translation: a near point is not translated");

    TPointD q;
    require(deformer.undeform(p + move, 0.0, q) && isClose(q, p, 1e-6),
            "curve translation: wrong undeform of a near point");
  }

  // Halfway through the falloff, half of the move
  TPointD mid(0.0, 80.0);
  require(isClose(deformer.deform(mid, 0.0), mid + move * 0.5, 1e-6),
          "curve translation: wrong weight in the falloff");

  // The rest of the plane stays put
  TPointD farPoint(0.0, 500.0), q;
  require(isClose(deformer.deform(farPoint, 0.0), farPoint, 0.0),
          "curve translation: a far point moved");
  require(deformer.undeform(farPoint, 0.0, q) && isClose(q, farPoint, 0.0),
          "curve translation: a far point moved back");

  // The width scale multiplies the distances from the curve
  TCurveDeformer wide(curveAnchors(0.0, TPointD()),
                      curveAnchors(0.0, TPointD()), 40.0, 120.0, 2.0);
  require(
      isClose(wide.deform(TPointD(0.0, 10.0), 0.0), TPointD(0.0, 20.0), 1e-6),
      "curve translation: wrong width scale");
  require(wide.undeform(TPointD(0.0, 20.0), 0.0, q) &&
              isClose(q, TPointD(0.0, 10.0), 1e-6),
          "curve translation: wrong undeform with a width scale");
}

//------------------------------------------------------------------

void testCurveBend() {
  // The curve bends from a straight line: the points follow it, are inverted
  // on a grid (in full, and in the falloff where the inverse is iterative),
  // and the bounds contain them
  TCurveDeformer deformer(curveAnchors(0.0, TPointD()),
                          curveAnchors(0.5, TPointD()), 40.0, 200.0);

  // A point on the curve goes to the curve
  TBezierChain bent(curveAnchors(0.5, TPointD()));
  require(isClose(deformer.deform(TPointD(-150.0, 0.0), 0.0),
                  bent.getPoint(0.0), 1e-6) &&
              isClose(deformer.deform(TPointD(0.0, 0.0), 0.0),
                      bent.getPoint(0.5), 1e-6) &&
              isClose(deformer.deform(TPointD(150.0, 0.0), 0.0),
                      bent.getPoint(1.0), 1e-6),
          "curve bend: points of the curve do not follow it");

  TRectD source(-300.0, -200.0, 300.0, 200.0);
  TRectD bounds = deformer.deformedBounds(source, 0.0);

  for (int y = -90; y <= 90; y += 30)
    for (int x = -200; x <= 200; x += 40) {
      TPointD p(x, y), q;
      TPointD d = deformer.deform(p, 0.0);
      require(deformer.undeform(d, 0.0, q), "curve bend: undeform failed");

      // The inverse is direct where the deformation applies in full
      bool full = x >= -150 && x <= 150 && y >= -30 && y <= 30;
      require(isClose(q, p, full ? 1e-4 : 1e-3),
              "curve bend: undeform does not invert deform");

      require(bounds.x0 <= d.x && d.x <= bounds.x1 && bounds.y0 <= d.y &&
                  d.y <= bounds.y1,
              "curve bend: the bounds miss a deformed point");

      TRectD around(d.x - 1.0, d.y - 1.0, d.x + 1.0, d.y + 1.0);
      TRectD srcBounds(deformer.sourceBounds(around, 0.0));
      require(srcBounds.x0 <= p.x && p.x <= srcBounds.x1 &&
                  srcBounds.y0 <= p.y && p.y <= srcBounds.y1,
              "curve bend: the source bounds miss a source point");
    }

  // Handles shape the curve: moving one handle moves the points near it
  std::vector<Anchor> handled = curveAnchors(0.0, TPointD());
  handled[1].m_out += TPointD(0.0, 30.0);
  TCurveDeformer pulled(curveAnchors(0.0, TPointD()), handled, 40.0, 120.0);
  require(
      norm(pulled.deform(TPointD(-30.0, 0.0), 0.0) - TPointD(-30.0, 0.0)) > 1.0,
      "curve bend: a handle does not bend the curve");
  require(isClose(pulled.deform(TPointD(150.0, 0.0), 0.0), TPointD(150.0, 0.0),
                  1e-6),
          "curve bend: a handle moved the far end");
}

}  // namespace

//------------------------------------------------------------------

// Joints of a horizontal chain of 2 bones, from -100 to 100
std::vector<TPointD> chainJoints() {
  return {TPointD(-100.0, 0.0), TPointD(0.0, 0.0), TPointD(100.0, 0.0)};
}

// The chain with its second bone turned by degrees about the middle joint
std::vector<TPointD> bentJoints(double degrees) {
  double a = degrees * M_PI_180;
  return {TPointD(-100.0, 0.0), TPointD(0.0, 0.0),
          TPointD(100.0 * std::cos(a), 100.0 * std::sin(a))};
}

//------------------------------------------------------------------

void testBoneIdentity() {
  TBoneDeformer deformer(
      TBoneDeformer::makeChain(chainJoints(), chainJoints(), 30.0, 80.0));
  require(deformer.isValid() && deformer.getBoneCount() == 2,
          "bone identity: not valid");
  require(deformer.isIdentity(0.0), "bone identity: not identity");
  require(
      deformer.getBone(0).m_parent == -1 && deformer.getBone(1).m_parent == 0,
      "bone identity: wrong parents of a chain");

  for (const TPointD &p : samplePoints()) {
    TPointD q;
    require(isClose(deformer.deform(p, 0.0), p, 1e-9),
            "bone identity: deform moved a point");
    require(deformer.undeform(p, 0.0, q) && isClose(q, p, 1e-9),
            "bone identity: undeform moved a point");
  }

  TBoneDeformer bent(
      TBoneDeformer::makeChain(chainJoints(), bentJoints(10.0), 30.0, 80.0));
  require(!bent.isIdentity(1e-6), "bone identity: a bent chain is identity");
}

//------------------------------------------------------------------

void testBoneInvalid() {
  std::vector<TPointD> joints = chainJoints();

  // Bones: none, a parent that comes later, a non finite value
  TBoneDeformer none((std::vector<TBoneDeformer::Bone>()));
  require(!none.isValid(), "bone invalid: no bone is valid");

  std::vector<TBoneDeformer::Bone> bones =
      TBoneDeformer::makeChain(joints, bentJoints(30.0), 30.0, 80.0);
  bones[0].m_parent = 1;
  TBoneDeformer badParent(bones);
  require(!badParent.isValid(), "bone invalid: a later parent is valid");

  bones = TBoneDeformer::makeChain(joints, bentJoints(30.0), 30.0, 80.0);
  bones[1].m_head.x = std::nan("");
  TBoneDeformer badValue(bones);
  require(!badValue.isValid(), "bone invalid: a NaN is valid");

  bones = TBoneDeformer::makeChain(joints, bentJoints(30.0), 30.0, 80.0);
  TBoneDeformer badPlacement(bones, TScale(0.0));
  require(!badPlacement.isValid(), "bone invalid: a null placement is valid");

  // A deformer that is not valid does not move anything
  TPointD p(10.0, 20.0), q;
  require(isClose(badParent.deform(p, 0.0), p, 0.0) &&
              badParent.undeform(p, 0.0, q) && isClose(q, p, 0.0),
          "bone invalid: moved a point");

  require(TBoneDeformer::makeChain(joints, std::vector<TPointD>(2), 1.0, 2.0)
              .empty(),
          "bone invalid: a chain of mismatched joints");

  // A tree: three bones from the same joint
  std::vector<TBoneDeformer::Bone> tree;
  tree.push_back(TBoneDeformer::Bone(TPointD(0.0, 0.0), TPointD(100.0, 0.0),
                                     TPointD(0.0, 0.0), TPointD(100.0, 0.0),
                                     20.0, 50.0, -1));
  tree.push_back(TBoneDeformer::Bone(TPointD(100.0, 0.0), TPointD(200.0, 0.0),
                                     TPointD(100.0, 0.0), TPointD(200.0, 0.0),
                                     20.0, 50.0, 0));
  tree.push_back(TBoneDeformer::Bone(TPointD(100.0, 0.0), TPointD(100.0, 100.0),
                                     TPointD(100.0, 0.0), TPointD(100.0, 100.0),
                                     20.0, 50.0, 0));
  TBoneDeformer treeDeformer(tree);
  require(treeDeformer.isValid() && treeDeformer.getBone(2).m_parent == 0 &&
              treeDeformer.isIdentity(0.0),
          "bone invalid: a tree is not valid");
}

//------------------------------------------------------------------

void testBoneTranslation() {
  // A single bone translated rigidly
  TPointD t(20.0, -10.0);
  std::vector<TBoneDeformer::Bone> bones(1);
  bones[0] = TBoneDeformer::Bone(TPointD(-100.0, 0.0), TPointD(100.0, 0.0),
                                 TPointD(-100.0, 0.0) + t,
                                 TPointD(100.0, 0.0) + t, 40.0, 100.0);
  TBoneDeformer deformer(bones);

  // Within the inner radius the points follow exactly
  const TPointD inside[] = {TPointD(0.0, 30.0),    TPointD(0.0, 0.0),
                            TPointD(-100.0, 0.0),  TPointD(100.0, -40.0),
                            TPointD(-130.0, 20.0), TPointD(60.0, -25.0)};
  for (const TPointD &p : inside) {
    TPointD q;
    require(isClose(deformer.deform(p, 0.0), p + t, 1e-9),
            "bone translation: a point of the inner radius is not moved");
    require(deformer.undeform(p + t, 0.0, q) && isClose(q, p, 1e-4),
            "bone translation: wrong undeform");
  }

  // Beyond the outer radius they stay
  const TPointD outside[] = {TPointD(0.0, 101.0), TPointD(0.0, -150.0),
                             TPointD(250.0, 0.0), TPointD(-300.0, 300.0)};
  for (const TPointD &p : outside) {
    TPointD q;
    require(isClose(deformer.deform(p, 0.0), p, 1e-12),
            "bone translation: a far point is moved");
    require(deformer.undeform(p, 0.0, q) && isClose(q, p, 1e-9),
            "bone translation: a far point is moved back");
  }

  // Halfway of the falloff: the smoothstep is 1/2, so is the displacement
  TPointD p(0.0, 70.0), q;
  require(isClose(deformer.deform(p, 0.0), p + t * 0.5, 1e-9),
          "bone translation: wrong falloff");
  require(deformer.undeform(p + t * 0.5, 0.0, q) && isClose(q, p, 1e-4),
          "bone translation: wrong undeform in the falloff");

  std::vector<double> weights;
  double rest = deformer.weightsAt(p, weights);
  require(weights.size() == 1 && std::fabs(weights[0] - 0.5) < 1e-12 &&
              std::fabs(rest - 0.5) < 1e-12,
          "bone translation: wrong weights");

  // The displacement decreases with the distance
  double previous = norm(t) + 1e-9;
  for (double y = 0.0; y <= 120.0; y += 1.0) {
    TPointD d = deformer.deform(TPointD(0.0, y), 0.0) - TPointD(0.0, y);
    require(norm(d) <= previous + 1e-12,
            "bone translation: the falloff is not monotone");
    previous = norm(d);
  }
}

//------------------------------------------------------------------

void testBoneBend() {
  // Two bones sharing the middle joint, the second one turned by 30 degrees
  const double a = 30.0 * M_PI_180;
  TBoneDeformer deformer(
      TBoneDeformer::makeChain(chainJoints(), bentJoints(30.0), 30.0, 80.0));
  require(deformer.isValid(), "bone bend: not valid");

  // Out of the second bone's reach, the first one holds
  TPointD p(-90.0, 0.0);
  require(isClose(deformer.deform(p, 0.0), p, 1e-9),
          "bone bend: the first bone moved");

  // Out of the first bone's reach, the second one turns the points about the
  // joint
  p = TPointD(90.0, 0.0);
  require(isClose(deformer.deform(p, 0.0),
                  TPointD(90.0 * std::cos(a), 90.0 * std::sin(a)), 1e-9),
          "bone bend: the second bone did not turn");

  // The joint is common to both
  p = TPointD(0.0, 0.0);
  require(isClose(deformer.deform(p, 0.0), p, 1e-9),
          "bone bend: the joint moved");

  // Next to the joint, both bones act with the same weight
  p = TPointD(0.0, 20.0);
  TPointD expected =
      p * 0.5 + TPointD(-20.0 * std::sin(a), 20.0 * std::cos(a)) * 0.5;
  require(isClose(deformer.deform(p, 0.0), expected, 1e-9),
          "bone bend: wrong blend at the joint");

  // The bend is smooth: no jump along a line across both bones, and the
  // weights make a partition of the unity
  TPointD previous = deformer.deform(TPointD(-90.0, 0.0), 0.0);
  for (double x = -89.0; x <= 90.0; x += 1.0) {
    TPointD current = deformer.deform(TPointD(x, 0.0), 0.0);
    require(norm(current - previous) < 3.0, "bone bend: a jump");
    previous = current;

    std::vector<double> weights;
    double rest = deformer.weightsAt(TPointD(x, 0.0), weights), sum = rest;
    for (double w : weights) {
      require(w >= 0.0, "bone bend: a negative weight");
      sum += w;
    }
    require(std::fabs(sum - 1.0) < 1e-12, "bone bend: weights do not sum to 1");
  }

  // A placement by a similarity gives the same bend, seen through it
  TAffine place = TTranslation(30.0, -10.0) * TRotation(25.0) * TScale(2.0);
  TBoneDeformer placed(
      TBoneDeformer::makeChain(chainJoints(), bentJoints(30.0), 30.0, 80.0),
      place);
  const TPointD samples[] = {TPointD(0.0, 20.0), TPointD(40.0, 10.0),
                             TPointD(-60.0, -30.0), TPointD(90.0, 0.0),
                             TPointD(10.0, 70.0)};
  for (const TPointD &s : samples)
    require(isClose(placed.deform(place * s, 0.0),
                    place * deformer.deform(s, 0.0), 1e-8),
            "bone bend: the placement changes the bend");
}

//------------------------------------------------------------------

void testBoneRoundTrip() {
  // A moderate bend is invertible: map a grid and back
  TBoneDeformer deformer(
      TBoneDeformer::makeChain(chainJoints(), bentJoints(15.0), 40.0, 120.0));

  for (double y = -90.0; y <= 90.0; y += 15.0)
    for (double x = -120.0; x <= 120.0; x += 15.0) {
      TPointD p(x, y), q = deformer.deform(p, 0.0), r;

      require(deformer.undeform(q, 0.0, r), "bone round trip: undeform failed");
      require(isClose(deformer.deform(r, 0.0), q, 1e-3),
              "bone round trip: the result does not map to the point");
      require(isClose(r, p, 5e-3), "bone round trip: wrong source point");
    }

  // The bounds hold the deformed corners
  TRectD source(-120.0, -90.0, 120.0, 90.0);
  TRectD bounds           = deformer.deformedBounds(source, 0.0);
  const TPointD corners[] = {source.getP00(), source.getP10(), source.getP11(),
                             source.getP01()};
  for (const TPointD &c : corners) {
    TPointD q = deformer.deform(c, 0.0);
    require(q.x >= bounds.x0 && q.x <= bounds.x1 && q.y >= bounds.y0 &&
                q.y <= bounds.y1,
            "bone round trip: bounds too small");
  }
}

}  // namespace

//------------------------------------------------------------------

//------------------------------------------------------------------

void testSplineSamples() {
  std::vector<TPointD> anchors = square(), samples;

  // One sample per edge gives the anchors back
  TEnvelopeDeformer::sampleClosedSpline(anchors, 1, samples);
  require(samples.size() == anchors.size(), "spline: wrong sample count");
  for (size_t i = 0; i < anchors.size(); ++i)
    require(isClose(samples[i], anchors[i], 1e-12),
            "spline: a single sample is not the anchor");

  // The curve goes through the anchors, which start each edge
  TEnvelopeDeformer::sampleClosedSpline(anchors, 8, samples);
  require(samples.size() == 8 * anchors.size(), "spline: wrong sample count");
  for (size_t i = 0; i < anchors.size(); ++i)
    require(isClose(samples[8 * i], anchors[i], 1e-12),
            "spline: the curve misses an anchor");
}

//------------------------------------------------------------------

void testSmoothIdentity() {
  TEnvelopeDeformer deformer = makeEnvelope(square(), square(), 8);
  require(deformer.isValid(), "smooth identity: not valid");
  require(deformer.isIdentity(0.0), "smooth identity: not identity");

  for (const TPointD &p : samplePoints())
    require(isClose(deformer.deform(p, 0.0), p, 1e-9),
            "smooth identity: deform moved a point");
}

//------------------------------------------------------------------

void testTriangleAffine() {
  // With 3 anchors the envelope is a triangle, or a rounded one when smooth;
  // either way an affine change of the anchors is reproduced exactly
  std::vector<TPointD> rest = {TPointD(-80.0, -60.0), TPointD(90.0, -50.0),
                               TPointD(10.0, 100.0)};
  TAffine aff = TTranslation(12.0, -7.0) * TRotation(30.0) * TScale(1.5, 0.75);
  std::vector<TPointD> deformed = rest;
  for (TPointD &p : deformed) p = aff * p;

  const TPointD points[] = {TPointD(0.0, 0.0),     TPointD(20.0, 10.0),
                            TPointD(-30.0, -20.0), TPointD(150.0, 80.0),
                            TPointD(-120.0, 90.0), TPointD(10.0, -150.0)};

  for (int samples : {1, 8}) {
    TEnvelopeDeformer deformer = makeEnvelope(rest, deformed, samples);
    require(deformer.isValid(), "triangle: not valid");

    for (const TPointD &p : points) {
      TPointD q;
      require(deformer.undeform(aff * p, 0.0, q), "triangle: undeform failed");
      require(isClose(q, p, 1e-6), "triangle: wrong undeform");
      require(isClose(deformer.deform(p, 0.0), aff * p, 1e-4),
              "triangle: wrong deform");
    }
  }
}

//------------------------------------------------------------------

void testSmoothBend() {
  // Moving an anchor moves the curve through it, and bends the plane smoothly
  std::vector<TPointD> rest = square(), deformed = square();
  deformed[2] += TPointD(40.0, 30.0);

  TEnvelopeDeformer deformer = makeEnvelope(rest, deformed, 8);
  require(!deformer.isIdentity(1.0), "smooth bend: identity");
  require(isClose(deformer.deform(rest[2], 0.0), deformed[2], 1e-3),
          "smooth bend: an anchor is not moved to its deformed position");
  require(
      isClose(deformer.deform(TPointD(0.0, 0.0), 0.0), TPointD(0.0, 0.0), 30.0),
      "smooth bend: the center moved too much");
}

//------------------------------------------------------------------

void testVectorImage() {
  TVectorImageP vi(new TVectorImage());
  std::vector<TThickPoint> points = {TThickPoint(0.0, 0.0, 2.0),
                                     TThickPoint(10.0, 20.0, 3.0),
                                     TThickPoint(30.0, 0.0, 4.0)};
  vi->addStroke(new TStroke(points));

  // A translation of the plane translates the control points
  std::vector<TPointD> moved = square();
  for (TPointD &p : moved) p += TPointD(5.0, -3.0);
  TEnvelopeDeformer deformer(square(), moved);

  deformVectorImage(*vi, deformer, 0.0);

  require(vi->getStrokeCount() == 1, "vector: wrong stroke count");
  TStroke *stroke = vi->getStroke(0);
  require(stroke->getControlPointCount() == 3, "vector: wrong point count");
  for (int i = 0; i < 3; ++i) {
    TThickPoint p = stroke->getControlPoint(i);
    require(isClose(TPointD(p.x, p.y),
                    TPointD(points[i].x + 5.0, points[i].y - 3.0), 1e-4),
            "vector: a control point is not translated");
    require(std::fabs(p.thick - points[i].thick) < 1e-9,
            "vector: the thickness changed");
  }
}

//------------------------------------------------------------------
// Shape Aware deformer (TShapeAwareDeformer)

#include "toonz/tshapeawaredeformer.h"

namespace {

typedef TShapeAwareDeformer::Handle SaHandle;

// A pin, a bone and a cage, on their rest geometry
std::vector<SaHandle> saRestHandles() {
  std::vector<SaHandle> handles;
  handles.push_back(
      SaHandle::makePin(TPointD(-40.0, 30.0), TPointD(-40.0, 30.0)));
  handles.push_back(
      SaHandle::makePin(TPointD(60.0, -20.0), TPointD(60.0, -20.0)));
  handles.push_back(
      SaHandle::makeBone(TPointD(-60.0, -50.0), TPointD(20.0, -50.0),
                         TPointD(-60.0, -50.0), TPointD(20.0, -50.0)));
  const TPointD cage[4] = {TPointD(-80.0, -80.0), TPointD(80.0, -80.0),
                           TPointD(80.0, 80.0), TPointD(-80.0, 80.0)};
  for (const TPointD &c : cage)
    handles.push_back(SaHandle::makeCageVertex(c, c));
  return handles;
}

//------------------------------------------------------------------

void testShapeAwareWeights() {
  std::vector<SaHandle> handles = saRestHandles();
  TShapeAwareDeformer deformer(handles);
  require(deformer.isValid(), "shape aware: not valid");
  require(deformer.getHandleCount() == (int)handles.size(),
          "shape aware: wrong handle count");

  // Non-negative, bounded and summing to 1, inside and outside the grid
  const TRectD domain = deformer.getDomain().enlarge(30.0);
  const int n         = 40;
  for (int j = 0; j <= n; ++j)
    for (int i = 0; i <= n; ++i) {
      TPointD p(domain.x0 + domain.getLx() * i / n,
                domain.y0 + domain.getLy() * j / n);
      std::vector<double> w;
      double anchor = deformer.weightsAt(p, w);
      require((int)w.size() == deformer.getHandleCount(),
              "shape aware: wrong weight count");

      double sum = anchor;
      require(anchor >= 0.0 && anchor <= 1.0,
              "shape aware: anchor out of [0, 1]");
      for (double v : w) {
        require(v >= 0.0 && v <= 1.0, "shape aware: weight out of [0, 1]");
        sum += v;
      }
      require(std::fabs(sum - 1.0) < 1e-9,
              "shape aware: weights do not sum to 1");
    }

  // The plane far from the handles is held by the anchor
  std::vector<double> w;
  require(deformer.weightsAt(TPointD(1000.0, 1000.0), w) == 1.0,
          "shape aware: the anchor does not hold the far plane");

  // The handles dominate their own place
  deformer.weightsAt(TPointD(-40.0, 30.0), w);
  require(w[0] > 0.9, "shape aware: a pin has a weak weight at its position");
  deformer.weightsAt(TPointD(60.0, -20.0), w);
  require(w[1] > 0.9, "shape aware: a pin has a weak weight at its position");
  deformer.weightsAt(TPointD(-20.0, -50.0), w);
  require(w[2] > 0.9, "shape aware: a bone has a weak weight on its segment");

  // The weights depend on the rest geometry only, and can be shared
  std::vector<SaHandle> moved = handles;
  moved[0] =
      SaHandle::makePin(TPointD(-40.0, 30.0), TPointD(-30.0, 35.0), 20.0, 1.2);
  require(TShapeAwareDeformer::makeWeightsKey(
              handles, TShapeAwareDeformer::Settings()) ==
              TShapeAwareDeformer::makeWeightsKey(
                  moved, TShapeAwareDeformer::Settings()),
          "shape aware: the weights key depends on the pose");

  TShapeAwareDeformer shared(moved, TShapeAwareDeformer::Settings(), TAffine(),
                             deformer.getWeights());
  TShapeAwareDeformer own(moved);
  for (const TPointD &p : {TPointD(-35.0, 30.0), TPointD(0.0, 0.0),
                           TPointD(50.0, -30.0), TPointD(-70.0, 70.0)})
    require(isClose(shared.deform(p, 0.0), own.deform(p, 0.0), 1e-9),
            "shape aware: shared weights give another deformation");

  // Invalid input
  require(!TShapeAwareDeformer::computeWeights(std::vector<SaHandle>(),
                                               TShapeAwareDeformer::Settings()),
          "shape aware: weights of no handle");
  TShapeAwareDeformer invalid((std::vector<SaHandle>()));
  require(!invalid.isValid() && invalid.isIdentity(0.0) &&
              isClose(invalid.deform(TPointD(3.0, 4.0), 0.0), TPointD(3.0, 4.0),
                      0.0),
          "shape aware: an invalid deformer moves points");
}

//------------------------------------------------------------------

void testShapeAwareIdentity() {
  std::vector<SaHandle> handles = saRestHandles();
  TShapeAwareDeformer deformer(handles);
  require(deformer.isIdentity(1e-9), "shape aware identity: not identity");

  for (const TPointD &p : {TPointD(0.0, 0.0), TPointD(-40.0, 30.0),
                           TPointD(75.0, 75.0), TPointD(-200.0, 10.0)}) {
    TPointD q;
    require(isClose(deformer.deform(p, 0.0), p, 1e-9),
            "shape aware identity: deform moved a point");
    require(deformer.undeform(p, 0.0, q) && isClose(q, p, 1e-6),
            "shape aware identity: undeform moved a point");
  }

  // The raster is unchanged
  const int size = 48;
  TRaster32P in(size, size), out(size, size);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x)
      in->pixels(y)[x] = TPixel32(5 * x, 5 * y, 100, 255);

  TPointD pos(-24.0, -24.0);
  deformer.warpRaster(out, pos, in, pos, 0.0);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x)
      require(out->pixels(y)[x] == in->pixels(y)[x],
              "shape aware identity: the raster changed");
}

//------------------------------------------------------------------

void testShapeAwareMovedPin() {
  // A pin is moved, another one holds the plane at 100 units from it
  const TPointD t(6.0, 2.0);
  std::vector<SaHandle> handles;
  handles.push_back(SaHandle::makePin(TPointD(0.0, 0.0), t));
  handles.push_back(
      SaHandle::makePin(TPointD(100.0, 0.0), TPointD(100.0, 0.0)));
  TShapeAwareDeformer deformer(handles,
                               TShapeAwareDeformer::Settings(48, 30.0));
  require(!deformer.isIdentity(1e-6), "shape aware pin: identity");

  // The pin carries its place
  require(norm(deformer.deform(TPointD(0.0, 0.0), 0.0) - t) < 0.15 * norm(t),
          "shape aware pin: the pin does not reach its position");

  // The nearer to the pin, the more a point moves
  auto moveAt = [&](double x) {
    TPointD p(x, 0.0);
    return norm(deformer.deform(p, 0.0) - p);
  };
  require(moveAt(10.0) > moveAt(40.0),
          "shape aware pin: 10 does not move more than 40");
  require(moveAt(40.0) > moveAt(75.0),
          "shape aware pin: 40 does not move more than 75");
  require(moveAt(10.0) > 0.25 * norm(t),
          "shape aware pin: a near point hardly moves");
  require(moveAt(75.0) < 0.5 * norm(t),
          "shape aware pin: a far point moves much");

  // The held pin stays, and the plane beyond the grid does not move
  require(isClose(deformer.deform(TPointD(100.0, 0.0), 0.0),
                  TPointD(100.0, 0.0), 0.15 * norm(t)),
          "shape aware pin: the held pin moves");
  require(isClose(deformer.deform(TPointD(500.0, 50.0), 0.0),
                  TPointD(500.0, 50.0), 0.0),
          "shape aware pin: a point beyond the grid moves");

  // A moved pin moves the same way whatever the placement
  TAffine placement =
      TTranslation(30.0, -20.0) * TRotation(30.0) * TScale(2.0, 2.0);
  TShapeAwareDeformer placed(handles, TShapeAwareDeformer::Settings(48, 30.0),
                             placement, deformer.getWeights());
  for (const TPointD &p :
       {TPointD(0.0, 0.0), TPointD(20.0, 10.0), TPointD(70.0, -10.0)})
    require(isClose(placed.deform(placement * p, 0.0),
                    placement * deformer.deform(p, 0.0), 1e-9),
            "shape aware pin: wrong placement");

  // deform() and undeform() are inverse
  for (const TPointD &p :
       {TPointD(0.0, 0.0), TPointD(15.0, -5.0), TPointD(-30.0, 20.0),
        TPointD(60.0, 10.0), TPointD(120.0, 0.0)}) {
    TPointD q;
    require(deformer.undeform(deformer.deform(p, 0.0), 0.0, q) &&
                isClose(q, p, 1e-3),
            "shape aware pin: undeform is not the inverse of deform");
  }

  // The bounds hold the deformed points
  TRectD source(-60.0, -60.0, 160.0, 60.0);
  TRectD bounds = deformer.deformedBounds(source, 0.0);
  for (double x = source.x0; x <= source.x1; x += 10.0)
    for (double y = source.y0; y <= source.y1; y += 10.0) {
      TPointD q = deformer.deform(TPointD(x, y), 0.0);
      require(q.x >= bounds.x0 && q.x <= bounds.x1 && q.y >= bounds.y0 &&
                  q.y <= bounds.y1,
              "shape aware pin: bounds too small");
    }

  // The raster follows the pin: pixels beyond the grid are as they were
  const int size = 64;
  TRaster32P in(size, size), out(size, size);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x)
      in->pixels(y)[x] = TPixel32(3 * x, 3 * y, 100, 255);

  std::vector<SaHandle> lone(
      1, SaHandle::makePin(TPointD(0.0, 0.0), TPointD(4.0, 0.0)));
  TShapeAwareDeformer loneDeformer(lone,
                                   TShapeAwareDeformer::Settings(48, 20.0));
  TPointD pos(-32.0, -32.0);
  loneDeformer.warpRaster(out, pos, in, pos, 0.0);
  require(out->pixels(32)[32] != in->pixels(32)[32],
          "shape aware pin: the raster did not move at the pin");
  require(out->pixels(0)[0] == in->pixels(0)[0] &&
              out->pixels(size - 1)[size - 1] == in->pixels(size - 1)[size - 1],
          "shape aware pin: the raster moved beyond the grid");

  // As a layer of the deformer stack
  TDeformerStack stack;
  stack.setScale(200.0);
  stack.addLayer(std::make_shared<TShapeAwareDeformer>(
                     handles, TShapeAwareDeformer::Settings(48, 30.0)),
                 TDeformerWeightMap::makeUniform(0.5));
  TPointD p(10.0, 0.0), q = deformer.deform(p, 0.0);
  require(isClose(stack.deform(p, 0.0), p + (q - p) * 0.5, 1e-9),
          "shape aware pin: wrong stack blend");
}

//------------------------------------------------------------------

void testShapeAwareBoneAndCage() {
  const TPointD t(8.0, 4.0);

  // A bone translated
  std::vector<SaHandle> handles;
  handles.push_back(SaHandle::makeBone(TPointD(-50.0, 0.0), TPointD(50.0, 0.0),
                                       TPointD(-50.0, 0.0) + t,
                                       TPointD(50.0, 0.0) + t));
  TShapeAwareDeformer bone(handles);
  for (double x = -50.0; x <= 50.0; x += 25.0) {
    TPointD p(x, 0.0);
    require(norm(bone.deform(p, 0.0) - (p + t)) < 0.15 * norm(t),
            "shape aware bone: the bone does not carry its segment");
  }
  TPointD above(0.0, 12.0), far(0.0, 90.0);
  require(
      norm(bone.deform(above, 0.0) - above) > norm(bone.deform(far, 0.0) - far),
      "shape aware bone: the influence does not fall with the distance");

  // A bone turned by 90 degrees about its head
  handles[0] = SaHandle::makeBone(TPointD(0.0, 0.0), TPointD(60.0, 0.0),
                                  TPointD(0.0, 0.0), TPointD(0.0, 60.0));
  TShapeAwareDeformer turned(handles);
  require(
      isClose(turned.deform(TPointD(30.0, 0.0), 0.0), TPointD(0.0, 30.0), 3.0),
      "shape aware bone: the turned bone is not turned");

  // A cage translated: its inside and edges follow
  handles.clear();
  const TPointD corners[4] = {TPointD(-50.0, -50.0), TPointD(50.0, -50.0),
                              TPointD(50.0, 50.0), TPointD(-50.0, 50.0)};
  for (const TPointD &c : corners)
    handles.push_back(SaHandle::makeCageVertex(c, c + t));
  TShapeAwareDeformer cage(handles);
  for (const TPointD &p :
       {TPointD(0.0, 0.0), TPointD(20.0, -30.0), TPointD(50.0, 0.0)})
    require(norm(cage.deform(p, 0.0) - (p + t)) < 0.15 * norm(t),
            "shape aware cage: the cage does not carry its inside");
  require(
      isClose(cage.deform(TPointD(400.0, 0.0), 0.0), TPointD(400.0, 0.0), 0.0),
      "shape aware cage: a far point moves");
}

//------------------------------------------------------------------

void testShapeAware() {
  testShapeAwareWeights();
  testShapeAwareIdentity();
  testShapeAwareMovedPin();
  testShapeAwareBoneAndCage();
}

}  // namespace

int main() {
  try {
    testIdentity();
    testDegenerate();
    testAffinePrecision();
    testBendAndRoundTrip();
    testRasterWarp();
    testStackIdentity();
    testStackWeights();
    testWeightMap();
    testStackInverse();
    testFreeFormIdentity();
    testFreeFormInvalid();
    testFreeFormTranslation();
    testFreeFormAffine();
    testFreeFormRoundTrip();
    testFreeFormFold();
    testFreeFormStack();
    testBezierChain();
    testCurveIdentity();
    testCurveTranslation();
    testCurveBend();
    testBoneIdentity();
    testBoneInvalid();
    testBoneTranslation();
    testBoneBend();
    testBoneRoundTrip();
    testSplineSamples();
    testSmoothIdentity();
    testTriangleAffine();
    testSmoothBend();
    testVectorImage();
    testShapeAware();
  } catch (const std::exception &e) {
    std::cerr << "deformer_test failed: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "deformer_test passed" << std::endl;
  return 0;
}
