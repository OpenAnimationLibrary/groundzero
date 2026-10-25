// Exercises the CPU side of tgl::Batch (vertex generation and command
// recording). No OpenGL context is created: Batch::draw() is never called.

#include "tglbatch.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    ++failures;
  }
}

// TPointD::operator== has a tolerance: compare exactly here.
bool same(const TPointD &a, const TPointD &b) {
  return a.x == b.x && a.y == b.y;
}

bool isNear(const TPointD &a, const TPointD &b, double eps = 1e-9) {
  return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps;
}

typedef tgl::Batch::Command Command;

bool isDraw(const Command &cmd, tgl::Primitive primitive, int first,
            int count) {
  return cmd.m_kind == Command::Draw && cmd.m_primitive == primitive &&
         cmd.m_first == first && cmd.m_count == count;
}

void testCircle() {
  const TPointD center(1.0, 2.0);
  const double radius = 3.0;
  const int slices    = 8;

  tgl::Batch b;
  b.circle(center, radius, slices);
  check(b.vertexCount() == slices + 1, "circle vertex count is slices + 1");
  check(b.commands().size() == 1 &&
            isDraw(b.commands()[0], tgl::Primitive::LineStrip, 0, slices + 1),
        "circle is a single line strip");
  // The strip is closed on the exact starting point.
  check(same(b.vertexAt(0), center + TPointD(radius, 0.0)),
        "circle starts at center + (radius, 0)");
  check(same(b.vertexAt(slices), center + TPointD(radius, 0.0)),
        "circle ends exactly at its starting point");
  for (int i = 0; i <= slices; ++i) {
    double angle = M_2PI * i / slices;
    TPointD expected =
        center + radius * TPointD(std::cos(angle), std::sin(angle));
    check(isNear(b.vertexAt(i), expected), "circle vertex position");
  }

  // Same recurrence as the former immediate-mode tglDrawCircle().
  tgl::Batch origin;
  origin.circle(TPointD(), radius, slices);
  double step = M_2PI / (double)slices;
  double c = std::cos(step), s = std::sin(step);
  double x = radius, y = 0.0;
  check(same(origin.vertexAt(0), TPointD(x, y)), "origin circle first vertex");
  for (int i = 1; i < slices; ++i) {
    double xx = x;
    x         = c * xx - s * y;
    y         = s * xx + c * y;
    check(isNear(origin.vertexAt(i), TPointD(x, y), 1e-12),
          "origin circle matches the tglDrawCircle recurrence");
  }

  tgl::Batch degenerate;
  degenerate.circle(center, 0.0, 16);
  degenerate.circle(center, -1.0, 16);
  check(degenerate.empty() && degenerate.vertexCount() == 0,
        "circle with radius <= 0 records nothing");

  tgl::Batch clamped;
  clamped.circle(center, radius, 1);
  check(clamped.vertexCount() == 4, "circle slices are clamped to 3");
}

void testMerging() {
  tgl::Batch b;
  b.line(TPointD(0, 0), TPointD(1, 0));
  b.line(TPointD(0, 1), TPointD(1, 1));
  check(b.commands().size() == 1 &&
            isDraw(b.commands()[0], tgl::Primitive::Lines, 0, 4),
        "consecutive lines are merged");

  b.color(TPixel32::Red);
  b.line(TPointD(0, 2), TPointD(1, 2));
  check(b.commands().size() == 3 && b.commands()[1].m_kind == Command::Color &&
            isDraw(b.commands()[2], tgl::Primitive::Lines, 4, 2),
        "a color change splits merged lines");

  b.lineStrip({TPointD(0, 0), TPointD(1, 1)});
  b.lineStrip({TPointD(2, 2), TPointD(3, 3)});
  check(b.commands().size() == 5 &&
            isDraw(b.commands()[3], tgl::Primitive::LineStrip, 6, 2) &&
            isDraw(b.commands()[4], tgl::Primitive::LineStrip, 8, 2),
        "line strips are never merged");

  tgl::Batch q;
  q.begin(tgl::Primitive::Quads);
  q.vertex(0, 0);
  q.vertex(1, 0);
  q.vertex(1, 1);
  q.end();
  q.quad(TPointD(0, 0), TPointD(1, 0), TPointD(1, 1), TPointD(0, 1));
  check(q.commands().size() == 2,
        "nothing is merged after an incomplete primitive");

  tgl::Batch e;
  e.begin(tgl::Primitive::Triangles);
  e.end();
  check(e.empty(), "empty primitives are dropped");

  tgl::Batch t;
  t.triangle(TPointD(0, 0), TPointD(1, 0), TPointD(0, 1));
  t.begin(tgl::Primitive::Triangles);
  t.vertex(TPointD(0, 0), TPixel32::Red);
  t.vertex(TPointD(1, 0), TPixel32::Green);
  t.vertex(TPointD(0, 1));
  t.end();
  check(t.commands().size() == 2 && !t.commands()[0].m_vertexColors &&
            t.commands()[1].m_vertexColors,
        "per-vertex colored primitives are not merged with uniform ones");
}

void testRects() {
  const TRectD rect(1.0, 2.0, 3.0, 4.0);

  tgl::Batch outline;
  outline.rect(rect);
  check(outline.commands().size() == 1 &&
            isDraw(outline.commands()[0], tgl::Primitive::LineLoop, 0, 4),
        "rect is a line loop");
  check(same(outline.vertexAt(0), rect.getP00()) &&
            same(outline.vertexAt(1), rect.getP10()) &&
            same(outline.vertexAt(2), rect.getP11()) &&
            same(outline.vertexAt(3), rect.getP01()),
        "rect vertex order is P00, P10, P11, P01");

  tgl::Batch fill;
  fill.fillRect(rect);
  check(fill.commands().size() == 1 &&
            isDraw(fill.commands()[0], tgl::Primitive::Polygon, 0, 4),
        "fillRect is a polygon");

  fill.clear();
  check(fill.empty() && fill.vertexCount() == 0, "clear empties the batch");
}

}  // namespace

int main() {
  testCircle();
  testMerging();
  testRects();
  if (failures) {
    std::cerr << failures << " check(s) failed" << std::endl;
    return EXIT_FAILURE;
  }
  std::cout << "tglbatch: all checks passed" << std::endl;
  return EXIT_SUCCESS;
}
