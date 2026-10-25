// Regression test of the generic deformer layer (TDeformer) through the
// Envelope deformer: the mean value mapping, its inverse, and the raster warp.

#include "toonz/tdeformer.h"
#include "traster.h"

#include <iostream>
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

}  // namespace

//------------------------------------------------------------------

int main() {
  try {
    testIdentity();
    testDegenerate();
    testAffinePrecision();
    testBendAndRoundTrip();
    testRasterWarp();
  } catch (const std::exception &e) {
    std::cerr << "deformer_test failed: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "deformer_test passed" << std::endl;
  return 0;
}
