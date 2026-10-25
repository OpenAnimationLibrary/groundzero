#include "tglbatch.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace tgl {

namespace {

GLenum toGLenum(Primitive primitive) {
  switch (primitive) {
  case Primitive::Points:
    return GL_POINTS;
  case Primitive::Lines:
    return GL_LINES;
  case Primitive::LineStrip:
    return GL_LINE_STRIP;
  case Primitive::LineLoop:
    return GL_LINE_LOOP;
  case Primitive::Triangles:
    return GL_TRIANGLES;
  case Primitive::TriangleStrip:
    return GL_TRIANGLE_STRIP;
  case Primitive::TriangleFan:
    return GL_TRIANGLE_FAN;
  case Primitive::Quads:
    return GL_QUADS;
  case Primitive::Polygon:
    return GL_POLYGON;
  }
  assert(false);
  return GL_POINTS;
}

// Number of vertices of each independent primitive, or 0 when consecutive
// primitives of this kind cannot be merged into a single draw call.
int mergeableSize(Primitive primitive) {
  switch (primitive) {
  case Primitive::Points:
    return 1;
  case Primitive::Lines:
    return 2;
  case Primitive::Triangles:
    return 3;
  case Primitive::Quads:
    return 4;
  default:
    return 0;
  }
}

}  // namespace

//-----------------------------------------------------------------------------

Batch::Batch() : m_inPrimitive(false) {}

//-----------------------------------------------------------------------------

void Batch::clear() {
  assert(!m_inPrimitive);
  m_coords.clear();
  m_colors.clear();
  m_commands.clear();
  m_lastColor32        = TPixel32();
  m_currentVertexColor = TPixel32();
  m_inPrimitive        = false;
}

//-----------------------------------------------------------------------------

void Batch::color(const TPixel32 &color) {
  assert(!m_inPrimitive);
  Command cmd;
  cmd.m_kind          = Command::Color;
  cmd.m_primitive     = Primitive::Points;
  cmd.m_first         = 0;
  cmd.m_count         = 0;
  cmd.m_vertexColors  = false;
  cmd.m_colorIsPixelD = false;
  cmd.m_color32       = color;
  m_commands.push_back(cmd);
  m_lastColor32 = color;
}

//-----------------------------------------------------------------------------

void Batch::color(const TPixelD &color) {
  assert(!m_inPrimitive);
  Command cmd;
  cmd.m_kind          = Command::Color;
  cmd.m_primitive     = Primitive::Points;
  cmd.m_first         = 0;
  cmd.m_count         = 0;
  cmd.m_vertexColors  = false;
  cmd.m_colorIsPixelD = true;
  cmd.m_colorD        = color;
  m_commands.push_back(cmd);
}

//-----------------------------------------------------------------------------

void Batch::begin(Primitive primitive) {
  assert(!m_inPrimitive);
  Command cmd;
  cmd.m_kind          = Command::Draw;
  cmd.m_primitive     = primitive;
  cmd.m_first         = vertexCount();
  cmd.m_count         = 0;
  cmd.m_vertexColors  = false;
  cmd.m_colorIsPixelD = false;
  m_commands.push_back(cmd);
  m_currentVertexColor = m_lastColor32;
  m_inPrimitive        = true;
}

//-----------------------------------------------------------------------------

void Batch::addVertex(double x, double y) {
  assert(m_inPrimitive);
  m_coords.push_back(x);
  m_coords.push_back(y);
  ++m_commands.back().m_count;
}

//-----------------------------------------------------------------------------

void Batch::pushColor(const TPixel32 &color) {
  m_colors.push_back(color.r);
  m_colors.push_back(color.g);
  m_colors.push_back(color.b);
  m_colors.push_back(color.m);
}

//-----------------------------------------------------------------------------

void Batch::vertex(double x, double y) {
  addVertex(x, y);
  if (!m_colors.empty()) pushColor(m_currentVertexColor);
}

//-----------------------------------------------------------------------------

void Batch::vertex(const TPointD &p, const TPixel32 &color) {
  // The color array is allocated on the first colored vertex; the vertices
  // recorded so far get the fallback color (they belong either to primitives
  // drawn without the color array or to the leading uncolored vertices of
  // the current primitive).
  if (m_colors.empty())
    for (int i = 0, count = vertexCount(); i < count; ++i)
      pushColor(m_currentVertexColor);
  addVertex(p.x, p.y);
  pushColor(color);
  m_currentVertexColor             = color;
  m_commands.back().m_vertexColors = true;
}

//-----------------------------------------------------------------------------

void Batch::end() {
  assert(m_inPrimitive);
  m_inPrimitive = false;
  if (m_commands.back().m_count == 0)
    m_commands.pop_back();
  else
    tryMergeLast();
}

//-----------------------------------------------------------------------------

void Batch::tryMergeLast() {
  int n = (int)m_commands.size();
  if (n < 2) return;
  Command &prev      = m_commands[n - 2];
  const Command &cur = m_commands[n - 1];
  if (prev.m_kind != Command::Draw || prev.m_primitive != cur.m_primitive ||
      prev.m_vertexColors != cur.m_vertexColors)
    return;
  int size = mergeableSize(cur.m_primitive);
  // An incomplete trailing primitive is discarded by GL: merging after it
  // would shift the following vertices.
  if (size == 0 || prev.m_count % size != 0) return;
  assert(prev.m_first + prev.m_count == cur.m_first);
  prev.m_count += cur.m_count;
  m_commands.pop_back();
}

//-----------------------------------------------------------------------------

void Batch::point(const TPointD &p) {
  begin(Primitive::Points);
  vertex(p);
  end();
}

//-----------------------------------------------------------------------------

void Batch::line(const TPointD &p0, const TPointD &p1) {
  begin(Primitive::Lines);
  vertex(p0);
  vertex(p1);
  end();
}

//-----------------------------------------------------------------------------

void Batch::lineStrip(std::initializer_list<TPointD> points) {
  begin(Primitive::LineStrip);
  for (const TPointD &p : points) vertex(p);
  end();
}

//-----------------------------------------------------------------------------

void Batch::lineStrip(const std::vector<TPointD> &points) {
  begin(Primitive::LineStrip);
  for (const TPointD &p : points) vertex(p);
  end();
}

//-----------------------------------------------------------------------------

void Batch::lineLoop(std::initializer_list<TPointD> points) {
  begin(Primitive::LineLoop);
  for (const TPointD &p : points) vertex(p);
  end();
}

//-----------------------------------------------------------------------------

void Batch::lineLoop(const std::vector<TPointD> &points) {
  begin(Primitive::LineLoop);
  for (const TPointD &p : points) vertex(p);
  end();
}

//-----------------------------------------------------------------------------

void Batch::triangle(const TPointD &p0, const TPointD &p1, const TPointD &p2) {
  begin(Primitive::Triangles);
  vertex(p0);
  vertex(p1);
  vertex(p2);
  end();
}

//-----------------------------------------------------------------------------

void Batch::quad(const TPointD &p0, const TPointD &p1, const TPointD &p2,
                 const TPointD &p3) {
  begin(Primitive::Quads);
  vertex(p0);
  vertex(p1);
  vertex(p2);
  vertex(p3);
  end();
}

//-----------------------------------------------------------------------------

void Batch::polygon(std::initializer_list<TPointD> points) {
  begin(Primitive::Polygon);
  for (const TPointD &p : points) vertex(p);
  end();
}

//-----------------------------------------------------------------------------

void Batch::rect(const TRectD &rect) {
  lineLoop({rect.getP00(), rect.getP10(), rect.getP11(), rect.getP01()});
}

//-----------------------------------------------------------------------------

void Batch::fillRect(const TRectD &rect) {
  polygon({rect.getP00(), rect.getP10(), rect.getP11(), rect.getP01()});
}

//-----------------------------------------------------------------------------

void Batch::circle(const TPointD &center, double radius, int slices) {
  if (radius <= 0) return;
  slices = std::max(3, slices);

  double step = M_2PI / (double)slices;
  double c = std::cos(step), s = std::sin(step);

  begin(Primitive::LineStrip);
  double x = radius, y = 0.0;
  vertex(center.x + x, center.y + y);
  for (int i = slices - 1; i; --i) {
    double xx = x;
    x         = c * xx - s * y;
    y         = s * xx + c * y;
    vertex(center.x + x, center.y + y);
  }
  vertex(center.x + radius, center.y + 0.0);
  end();
}

//-----------------------------------------------------------------------------

void Batch::draw() const {
  assert(!m_inPrimitive);
  if (m_commands.empty()) return;

  // Restores the caller's array state (enables, pointers) on exit.
  glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
  glDisableClientState(GL_NORMAL_ARRAY);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_INDEX_ARRAY);
  glDisableClientState(GL_EDGE_FLAG_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);
  glEnableClientState(GL_VERTEX_ARRAY);
  if (!m_coords.empty()) glVertexPointer(2, GL_DOUBLE, 0, m_coords.data());
  if (!m_colors.empty())
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, m_colors.data());

  bool colorArray = false;
  for (const Command &cmd : m_commands) {
    if (cmd.m_kind == Command::Color) {
      if (cmd.m_colorIsPixelD)
        tglColor(cmd.m_colorD);
      else
        tglColor(cmd.m_color32);
      continue;
    }

    if (cmd.m_vertexColors != colorArray) {
      colorArray = cmd.m_vertexColors;
      if (colorArray)
        glEnableClientState(GL_COLOR_ARRAY);
      else
        glDisableClientState(GL_COLOR_ARRAY);
    }

    glDrawArrays(toGLenum(cmd.m_primitive), cmd.m_first, cmd.m_count);

    // The current color is undefined after drawing with a color array: set
    // it to the last vertex color, as glColor() inside glBegin()/glEnd()
    // would have left it.
    if (colorArray) glColor4ubv(&m_colors[4 * (cmd.m_first + cmd.m_count - 1)]);
  }

  glPopClientAttrib();
}

}  // namespace tgl
