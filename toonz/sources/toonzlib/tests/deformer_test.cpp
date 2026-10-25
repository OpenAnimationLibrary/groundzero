// Regression test of the generic deformer layer (TDeformer) through the
// Envelope deformer: the mean value mapping, its inverse, and the raster warp;
// and of the deformer stack (TDeformerStack) built on it.

#include "toonz/tdeformer.h"
#include "toonz/tdeformerstack.h"
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
  } catch (const std::exception &e) {
    std::cerr << "deformer_test failed: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "deformer_test passed" << std::endl;
  return 0;
}
