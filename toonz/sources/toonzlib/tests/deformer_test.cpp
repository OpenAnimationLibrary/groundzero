// Regression test of the generic deformer layer (TDeformer) through the
// Envelope deformer: the mean value mapping, its inverse, and the raster warp;
// of the deformer stack (TDeformerStack) built on it; and of the free-form
// lattice deformer (TFreeFormDeformer).

#include "toonz/tdeformer.h"
#include "toonz/tdeformerstack.h"
#include "toonz/tfreeformdeformer.h"
#include "traster.h"

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
  } catch (const std::exception &e) {
    std::cerr << "deformer_test failed: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "deformer_test passed" << std::endl;
  return 0;
}
