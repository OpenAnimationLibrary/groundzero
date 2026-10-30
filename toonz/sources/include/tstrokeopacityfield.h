#pragma once
#include "tstroke.h"
#include "tcurves.h"
#include <limits>
#include <numeric>
#include <stdexcept>

namespace StrokeOpacity {
// Spatial index for the arc-length coordinate of the nearest centerline point.
// Adaptive flattening tolerance is expressed in drawing coordinates; callers
// derive it from their output pixel scale. Pattern overhang uses the same
// nearest-centerline convention as the stroke body.
class Field {
  struct Segment { TPointD a, b; double first, last; TRectD box; };
  struct Node { TRectD box; size_t begin, end; int left = -1, right = -1; };
  std::vector<Segment> m_segments;
  std::vector<Node> m_nodes;
  const TStroke &m_stroke;
  double m_total;
  static double distanceSquared(const TRectD &box, const TPointD &p) {
    double x = std::max({box.x0 - p.x, 0., p.x - box.x1});
    double y = std::max({box.y0 - p.y, 0., p.y - box.y1});
    return x * x + y * y;
  }
  void flatten(int chunk, double first, double last, double tolerance, int depth) {
    const auto *q = m_stroke.getChunk(chunk);
    const TPointD a = q->getPoint(first), b = q->getPoint(last);
    const double middle = (first + last) * .5;
    const TPointD midpoint = q->getPoint(middle);
    if (depth < 20 && norm(midpoint - (a + b) * .5) > tolerance) {
      flatten(chunk, first, middle, tolerance, depth + 1);
      flatten(chunk, middle, last, tolerance, depth + 1);
    } else {
      // Bound transient work independently of the on-disk knot limit. Reject
      // pathological geometry explicitly instead of exhausting renderer memory.
      if (m_segments.size() >= 1048576)
        throw std::runtime_error("Stroke opacity geometry exceeds the render sampling limit");
      TRectD box(std::min(a.x,b.x), std::min(a.y,b.y), std::max(a.x,b.x), std::max(a.y,b.y));
      m_segments.push_back({a, b, m_total > 0 ? m_stroke.getLength(chunk, first) / m_total : 0.,
          m_total > 0 ? m_stroke.getLength(chunk, last) / m_total : 0., box});
    }
  }
  int build(size_t begin, size_t end) {
    const int index = int(m_nodes.size());
    TRectD box = m_segments[begin].box;
    for (size_t i = begin + 1; i < end; ++i) {
      const auto &other = m_segments[i].box;
      box.x0 = std::min(box.x0, other.x0); box.y0 = std::min(box.y0, other.y0);
      box.x1 = std::max(box.x1, other.x1); box.y1 = std::max(box.y1, other.y1);
    }
    m_nodes.push_back({box, begin, end});
    if (end - begin > 8) {
      const bool horizontal = box.getLx() >= box.getLy();
      const size_t middle = begin + (end - begin) / 2;
      std::nth_element(m_segments.begin() + begin, m_segments.begin() + middle,
          m_segments.begin() + end, [horizontal](const Segment &a, const Segment &b) {
            return horizontal ? a.a.x + a.b.x < b.a.x + b.b.x : a.a.y + a.b.y < b.a.y + b.b.y;
          });
      const int left = build(begin, middle), right = build(middle, end);
      m_nodes[index].left = left; m_nodes[index].right = right;
    }
    return index;
  }
  void nearest(int index, const TPointD &point, double &distance, double &position) const {
    const auto &node = m_nodes[index];
    if (distanceSquared(node.box, point) > distance) return;
    if (node.left >= 0) {
      int first = node.left, last = node.right;
      if (distanceSquared(m_nodes[first].box, point) > distanceSquared(m_nodes[last].box, point)) std::swap(first, last);
      nearest(first, point, distance, position); nearest(last, point, distance, position);
      return;
    }
    for (size_t i = node.begin; i < node.end; ++i) {
      const auto &segment = m_segments[i];
      TPointD delta = segment.b - segment.a;
      const double length2 = delta.x * delta.x + delta.y * delta.y;
      TPointD offset = point - segment.a;
      const double t = length2 > 0. ? std::clamp((offset.x * delta.x + offset.y * delta.y) / length2, 0., 1.) : 0.;
      const TPointD d = point - (segment.a + delta * t);
      const double candidate = d.x * d.x + d.y * d.y;
      const double at = segment.first * (1. - t) + segment.last * t;
      // A stable first-along-the-stroke tie break handles crossings and loops.
      if (candidate < distance || (candidate == distance && at < position)) distance = candidate, position = at;
    }
  }
public:
  explicit Field(const TStroke &stroke, double tolerance = .05)
      : m_stroke(stroke), m_total(stroke.getLength()) {
    for (int chunk = 0; chunk < stroke.getChunkCount(); ++chunk) flatten(chunk, 0., 1., std::max(tolerance, 1e-6), 0);
    if (!m_segments.empty()) build(0, m_segments.size());
  }
  double position(const TPointD &point) const {
    double distance = std::numeric_limits<double>::infinity(), result = 0.;
    if (!m_nodes.empty()) nearest(0, point, distance, result);
    return result;
  }
  double opacity(const TPointD &point) const { return evaluate(m_stroke.getOpacityProfile(), position(point)); }
};
} // namespace StrokeOpacity
