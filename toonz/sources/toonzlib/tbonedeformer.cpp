#include "toonz/tbonedeformer.h"

#include <algorithm>
#include <cmath>

//****************************************************************************************
//    Local namespace stuff
//****************************************************************************************

namespace {

bool isFinite(const TPointD &p) {
  return std::isfinite(p.x) && std::isfinite(p.y);
}

}  // namespace

//****************************************************************************************
//    TBoneDeformer  implementation
//****************************************************************************************

TBoneDeformer::TBoneDeformer(const std::vector<Bone> &bones,
                             const TAffine &placement)
    : m_scale(1.0), m_valid(false) {
  // The negated test also rejects NaNs
  if (bones.empty() || !(std::fabs(placement.det()) > 1e-18)) return;
  const double k = std::sqrt(std::fabs(placement.det()));

  m_bones.reserve(bones.size());
  m_cache.reserve(bones.size());

  for (size_t i = 0; i < bones.size(); ++i) {
    const Bone &b = bones[i];

    if (b.m_parent < -1 || b.m_parent >= (int)i || !isFinite(b.m_restHead) ||
        !isFinite(b.m_restTail) || !isFinite(b.m_head) || !isFinite(b.m_tail) ||
        !std::isfinite(b.m_inner) || !std::isfinite(b.m_outer)) {
      m_bones.clear();
      m_cache.clear();
      return;
    }

    Bone pb(placement * b.m_restHead, placement * b.m_restTail,
            placement * b.m_head, placement * b.m_tail,
            std::max(0.0, b.m_inner) * k, 0.0, b.m_parent);
    pb.m_outer = std::max(pb.m_inner, b.m_outer * k);
    m_bones.push_back(pb);

    Cache c;
    c.m_a     = pb.m_restHead;
    c.m_ab    = pb.m_restTail - pb.m_restHead;
    c.m_len2  = c.m_ab * c.m_ab;
    c.m_inner = pb.m_inner;
    c.m_outer = pb.m_outer;

    // The rotation from the rest direction to the deformed one; a bone with a
    // null end to end length has no direction, and only translates.
    TPointD v(pb.m_tail - pb.m_head);
    double lu = norm(c.m_ab), lv = norm(v);
    if (lu > 1e-12 && lv > 1e-12) {
      c.m_c = (c.m_ab * v) / (lu * lv);
      c.m_s = cross(c.m_ab, v) / (lu * lv);
    } else
      c.m_c = 1.0, c.m_s = 0.0;

    c.m_t = pb.m_head - TPointD(c.m_c * c.m_a.x - c.m_s * c.m_a.y,
                                c.m_s * c.m_a.x + c.m_c * c.m_a.y);
    m_cache.push_back(c);

    m_scale = std::max(m_scale, std::max(lu, lv));
  }

  m_valid = true;
}

//---------------------------------------------------------------------------------

std::vector<TBoneDeformer::Bone> TBoneDeformer::makeChain(
    const std::vector<TPointD> &restJoints, const std::vector<TPointD> &joints,
    double inner, double outer) {
  std::vector<Bone> bones;
  if (restJoints.size() != joints.size() || joints.size() < 2) return bones;

  for (size_t i = 0; i + 1 < joints.size(); ++i)
    bones.push_back(Bone(restJoints[i], restJoints[i + 1], joints[i],
                         joints[i + 1], inner, outer, (int)i - 1));
  return bones;
}

//---------------------------------------------------------------------------------

bool TBoneDeformer::isIdentity(double tolerance) const {
  if (!m_valid) return false;

  for (const Bone &b : m_bones)
    if (norm(b.m_head - b.m_restHead) > tolerance ||
        norm(b.m_tail - b.m_restTail) > tolerance)
      return false;
  return true;
}

//---------------------------------------------------------------------------------

// The influence of a bone at p: 1 up to the inner radius from the rest
// segment, then a smoothstep down to 0 at the outer radius.
double TBoneDeformer::influence(const Cache &c, const TPointD &p) {
  TPointD ap(p - c.m_a);

  double t = 0.0;
  if (c.m_len2 > 1e-18)
    t = std::min(1.0, std::max(0.0, (ap * c.m_ab) / c.m_len2));
  double d = norm(ap - c.m_ab * t);

  if (d <= c.m_inner) return 1.0;
  if (!(d < c.m_outer)) return 0.0;

  double u = (d - c.m_inner) / (c.m_outer - c.m_inner);
  return 1.0 - u * u * (3.0 - 2.0 * u);
}

//---------------------------------------------------------------------------------

TPointD TBoneDeformer::apply(const Cache &c, const TPointD &p) {
  return TPointD(c.m_c * p.x - c.m_s * p.y + c.m_t.x,
                 c.m_s * p.x + c.m_c * p.y + c.m_t.y);
}

//---------------------------------------------------------------------------------

TPointD TBoneDeformer::applyInverse(const Cache &c, const TPointD &p) {
  TPointD q(p - c.m_t);
  return TPointD(c.m_c * q.x + c.m_s * q.y, -c.m_s * q.x + c.m_c * q.y);
}

//---------------------------------------------------------------------------------

double TBoneDeformer::weightsAt(const TPointD &p,
                                std::vector<double> &weights) const {
  weights.assign(m_cache.size(), 0.0);
  if (!m_valid) return 1.0;

  double total = 0.0;
  for (size_t i = 0; i < m_cache.size(); ++i)
    total += (weights[i] = influence(m_cache[i], p));

  if (total <= 0.0) return 1.0;

  double denominator = std::max(total, 1.0);
  for (double &w : weights) w /= denominator;
  return std::max(0.0, 1.0 - total) / denominator;
}

//---------------------------------------------------------------------------------

TPointD TBoneDeformer::deform(const TPointD &p, double frame) const {
  if (!m_valid) return p;

  TPointD sum;
  double total = 0.0;
  for (const Cache &c : m_cache) {
    double w = influence(c, p);
    if (w <= 0.0) continue;

    sum += apply(c, p) * w;
    total += w;
  }

  if (total <= 0.0) return p;
  if (total < 1.0) return sum + p * (1.0 - total);
  return sum * (1.0 / total);
}

//---------------------------------------------------------------------------------

bool TBoneDeformer::undeform(const TPointD &p, double frame,
                             TPointD &result) const {
  if (!m_valid) {
    result = p;
    return true;
  }

  // Start from the inverse transforms of the bones, blended with the weights
  // at the target: exact where a single bone acts.
  TPointD g(p);
  {
    TPointD sum;
    double total = 0.0;
    for (const Cache &c : m_cache) {
      double w = influence(c, p);
      if (w <= 0.0) continue;

      sum += applyInverse(c, p) * w;
      total += w;
    }

    if (total >= 1.0)
      g = sum * (1.0 / total);
    else if (total > 0.0)
      g = sum + p * (1.0 - total);
  }

  // Newton's method on deform(g) = p, with the Jacobian taken by forward
  // differences, as the weights are not smooth enough to be worth deriving.
  const double tolerance = 1e-8 * m_scale, h = 1e-4 * m_scale;

  TPointD fg(deform(g, frame));
  TPointD f(fg - p);
  double err = norm(f);

  for (int it = 0; it < 24 && err > tolerance; ++it) {
    TPointD dx((deform(g + TPointD(h, 0.0), frame) - fg) * (1.0 / h));
    TPointD dy((deform(g + TPointD(0.0, h), frame) - fg) * (1.0 / h));

    // The columns of the Jacobian are dx and dy
    double det = dx.x * dy.y - dy.x * dx.y;
    if (!(std::fabs(det) > 1e-12)) break;  // Folded there

    TPointD step(dy.y * f.x - dy.x * f.y, dx.x * f.y - dx.y * f.x);
    step = step * (1.0 / det);

    // Damp the step until it reduces the error
    bool improved = false;
    double lambda = 1.0;
    for (int k = 0; k < 5; ++k, lambda *= 0.5) {
      TPointD gNew(g - step * lambda);
      TPointD fgNew(deform(gNew, frame));
      TPointD fNew(fgNew - p);
      double errNew = norm(fNew);
      if (errNew < err) {
        g = gNew, fg = fgNew, f = fNew, err = errNew;
        improved = true;
        break;
      }
    }
    if (!improved) break;  // No progress, e.g. near a fold
  }

  result = g;
  return true;
}
