#pragma once

#ifndef TGLBATCH_INCLUDED
#define TGLBATCH_INCLUDED

#include "tgl.h"

#include <initializer_list>
#include <vector>

#undef DVAPI
#undef DVVAR

#ifdef TGL_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

namespace tgl {

//=============================================================================

//! Primitive kinds a Batch can record. They mirror the OpenGL immediate-mode
//! primitives so that glBegin()/glEnd() sites can be ported one to one.
//! Quads and Polygon (convex) are not available in a core profile: a later
//! implementation is expected to triangulate them inside Batch::draw().
enum class Primitive {
  Points,
  Lines,
  LineStrip,
  LineLoop,
  Triangles,
  TriangleStrip,
  TriangleFan,
  Quads,
  Polygon
};

//=============================================================================

//! Records 2D primitives on the CPU and submits them to OpenGL in one go.
/*!
  A Batch replaces glBegin()/glVertex()/glEnd() sequences:

  \code
  tgl::Batch b;
  b.color(TPixel32::Red);
  b.line(p0, p1);
  b.circle(center, radius, 32);
  b.draw();
  \endcode

  Primitives are drawn by draw() in the order they were recorded. Colors set
  with color() are recorded too and applied, in order, between primitives,
  exactly like glColor() calls placed between glBegin()/glEnd() blocks. When
  no color() has been recorded, primitives use the GL current color, so a
  caller can keep setting the color with glColor()/tglColor() before draw().
  Vertices may also carry their own color (vertex(p, color)).

  Consecutive independent primitives of the same kind (points, lines,
  triangles, quads) are merged into a single draw call; strips, loops, fans
  and polygons are always submitted separately.

  Batch::draw() honors the current GL state: modelview and projection
  matrices, line width, line stipple, smoothing, blending, name stack, etc.
  A batch must be drawn before the caller changes any of that state for the
  next primitives.

  Implementation notes:
  \li The current implementation uses client-side vertex arrays
      (glVertexPointer/glColorPointer + glDrawArrays) in the compatibility
      context. It assumes no buffer is bound to GL_ARRAY_BUFFER.
  \li Vertex generation (everything except draw()) needs no GL context.
  \li The interface is meant to stay unchanged when draw() is reimplemented
      with a VBO and a small shader. That step will need the modelview and
      projection matrices to be passed explicitly instead of being read from
      the fixed-function matrix stack.
*/
class DVAPI Batch {
public:
  //! A recorded command, exposed for tests and debugging.
  struct Command {
    enum Kind { Draw, Color } m_kind;
    Primitive m_primitive;  //!< Draw only
    int m_first, m_count;   //!< Draw only: vertex range
    bool m_vertexColors;    //!< Draw only: uses per-vertex colors
    bool m_colorIsPixelD;   //!< Color only: which of the two colors is set
    TPixel32 m_color32;     //!< Color only
    TPixelD m_colorD;       //!< Color only
  };

public:
  Batch();

  //! Removes every recorded command and vertex.
  void clear();
  bool empty() const { return m_commands.empty(); }

  //! Sets the color of the following primitives (as tglColor()).
  void color(const TPixel32 &color);
  void color(const TPixelD &color);

  //! Starts a primitive, as glBegin(). Primitives cannot be nested.
  void begin(Primitive primitive);
  //! Adds a vertex to the primitive started by begin(), as glVertex2d().
  void vertex(double x, double y);
  void vertex(const TPointD &p) { vertex(p.x, p.y); }
  //! Adds a vertex with its own color. A primitive containing such a vertex
  //! is drawn with per-vertex colors: its vertices added without a color take
  //! the color of the previous vertex of the primitive, or, before the first
  //! colored one, the last color(const TPixel32 &) (opaque black if none).
  //! After drawing it, the GL current color is its last vertex color.
  void vertex(const TPointD &p, const TPixel32 &color);
  //! Ends the primitive started by begin(), as glEnd().
  void end();

  void point(const TPointD &p);
  void line(const TPointD &p0, const TPointD &p1);
  void lineStrip(std::initializer_list<TPointD> points);
  void lineStrip(const std::vector<TPointD> &points);
  void lineLoop(std::initializer_list<TPointD> points);
  void lineLoop(const std::vector<TPointD> &points);
  void triangle(const TPointD &p0, const TPointD &p1, const TPointD &p2);
  void quad(const TPointD &p0, const TPointD &p1, const TPointD &p2,
            const TPointD &p3);
  //! Fills a convex polygon.
  void polygon(std::initializer_list<TPointD> points);

  //! Outlines a rect with a line loop P00, P10, P11, P01 (as tglDrawRect()).
  void rect(const TRectD &rect);
  //! Fills a rect with a polygon P00, P10, P11, P01 (as tglFillRect()).
  void fillRect(const TRectD &rect);

  //! Outlines a circle with a closed line strip of slices + 1 vertices,
  //! starting and ending at center + (radius, 0) and going
  //! counterclockwise. Nothing is recorded for radius <= 0; slices is clamped
  //! to at least 3.
  void circle(const TPointD &center, double radius, int slices);

  //! Submits the recorded primitives to the current GL context. The batch is
  //! not cleared, so it can be drawn again.
  void draw() const;

  //! Recorded data, for tests and debugging.
  int vertexCount() const { return (int)m_coords.size() / 2; }
  TPointD vertexAt(int i) const {
    return TPointD(m_coords[2 * i], m_coords[2 * i + 1]);
  }
  const std::vector<Command> &commands() const { return m_commands; }

private:
  void addVertex(double x, double y);
  void pushColor(const TPixel32 &color);
  void tryMergeLast();

private:
  std::vector<GLdouble> m_coords;  //!< x, y pairs
  std::vector<GLubyte> m_colors;   //!< r, g, b, a per vertex, when needed
  std::vector<Command> m_commands;
  TPixel32 m_lastColor32;         //!< Last color(const TPixel32 &)
  TPixel32 m_currentVertexColor;  //!< Color of uncolored vertices
  bool m_inPrimitive;
};

}  // namespace tgl

#endif
