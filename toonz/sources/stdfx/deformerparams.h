#pragma once

#ifndef DEFORMERPARAMS_H
#define DEFORMERPARAMS_H

// The parameters of the deformer fxs built on the generic deformers
// (toonz/tdeformer.h), and how they make their TDeformer. They are shared by
// the single deformer fxs (Curve, Free-Form, Bone and Shape Aware Deformer)
// and by the layers of the Deformer Stack fx, so that a layer of a type
// deforms exactly as the fx of that type.
//
// Each set of parameters binds its parameters to an fx with a name prefix
// (empty for the single fxs, whose parameter names it keeps), builds its
// deformer at a frame in the space that an affine maps the fx's space to
// (the render's space, which differs from the fx's by a similarity), and
// fills the gadgets of its points.

#include "tfx.h"
#include "tfxparam.h"
#include "tdoubleparam.h"
#include "tnotanimatableparam.h"
#include "tparamset.h"
#include "tparamuiconcept.h"
#include "deformerfxcommon.h"

#include "toonz/tdeformer.h"
#include "toonz/tcurvedeformer.h"
#include "toonz/tfreeformdeformer.h"
#include "toonz/tbonedeformer.h"
#include "toonz/tshapeawaredeformer.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace deformerfx {

//------------------------------------------------------------------------------

//! Returns a point parameter of default p, in fx lengths.
inline TPointParamP makePointParam(const TPointD &p) {
  TPointParamP param(p);
  param->getX()->setMeasureName("fxLength");
  param->getY()->setMeasureName("fxLength");
  return param;
}

//------------------------------------------------------------------------------

//! Returns a non-negative length parameter of the passed default.
inline TDoubleParamP makeLengthParam(double value) {
  TDoubleParamP param(value);
  param->setMeasureName("fxLength");
  param->setValueRange(0.0, 1e6);
  return param;
}

//==============================================================================
//    Envelope
//==============================================================================

//! Builds the envelope deformer of params at frame, in the space that aff
//! maps the fx's space to, and fills rest with the rest envelope in that
//! space. Returns null, with rest empty, when the Source passes through
//! unchanged (see isPassThrough()).
inline std::shared_ptr<const TDeformer> buildEnvelopeDeformer(
    const EnvelopeParams &params, double frame, const TAffine &aff,
    std::vector<TPointD> &rest) {
  std::vector<TPointD> deformed;
  params.getEnvelopes(frame, rest, deformed);
  if (isPassThrough(rest, deformed)) {
    rest.clear();
    return nullptr;
  }

  mapPoints(aff, rest);
  mapPoints(aff, deformed);
  return std::make_shared<TEnvelopeDeformer>(rest, deformed);
}

//==============================================================================
//    Curve
//==============================================================================

inline void mapAnchors(const TAffine &aff,
                       std::vector<TBezierChain::Anchor> &anchors) {
  for (TBezierChain::Anchor &a : anchors)
    a.m_pos = aff * a.m_pos, a.m_in = aff * a.m_in, a.m_out = aff * a.m_out;
}

//------------------------------------------------------------------------------

//! The animatable control points of a curve: the position and the two tangent
//! handles of each anchor. The handles are positions, not offsets.
class CurveParams {
public:
  std::vector<TPointParamP> m_pos, m_in, m_out;

  //! Creates the parameters of anchorCount anchors (at least 2) and binds them
  //! to fx as posName, inName and outName followed by 1 to anchorCount. By
  //! default, the curve is a straight line along the x axis, from -halfLength
  //! to halfLength, with the anchors evenly spaced and the handles a third of
  //! the way to the neighbours.
  void bind(TFx *fx, const std::string &posName, const std::string &inName,
            const std::string &outName, int anchorCount, double halfLength) {
    const double step   = 2.0 * halfLength / (anchorCount - 1);
    const double handle = step / 3.0;

    m_pos.resize(anchorCount);
    m_in.resize(anchorCount);
    m_out.resize(anchorCount);
    for (int i = 0; i < anchorCount; ++i) {
      std::string suffix = std::to_string(i + 1);
      double x           = -halfLength + step * i;

      m_pos[i] = makePointParam(TPointD(x, 0.0));
      m_in[i]  = makePointParam(TPointD(x - handle, 0.0));
      m_out[i] = makePointParam(TPointD(x + handle, 0.0));

      bindParam(fx, posName + suffix, m_pos[i]);
      bindParam(fx, inName + suffix, m_in[i]);
      bindParam(fx, outName + suffix, m_out[i]);
    }
  }

  int getAnchorCount() const { return (int)m_pos.size(); }

  //! Returns the anchors at frame, in the fx's space.
  void getAnchors(double frame,
                  std::vector<TBezierChain::Anchor> &anchors) const {
    const int count = getAnchorCount();
    anchors.resize(count);
    for (int i = 0; i < count; ++i)
      anchors[i] = TBezierChain::Anchor(m_pos[i]->getValue(frame),
                                        m_in[i]->getValue(frame),
                                        m_out[i]->getValue(frame));
  }

  //! Fills concepts with the gadgets of the curve, whose labels start with
  //! labelPrefix, and returns how many. The vectors from the anchors to their
  //! handles come first, so that the points are drawn (and picked) over them.
  /*!
    The handles that do not shape the curve have no gadget. No more than
    maxConceptCount() concepts are written.
  */
  int getParamUIs(TParamUIConcept *concepts,
                  const std::string &labelPrefix) const {
    const int anchorCount = getAnchorCount();
    int count             = 0;

    for (int i = 0; i < anchorCount; ++i) {
      if (i > 0) {
        TParamUIConcept &c = concepts[count++];
        c.m_type           = TParamUIConcept::VECTOR;
        c.m_params.push_back(m_pos[i]);
        c.m_params.push_back(m_in[i]);
      }
      if (i < anchorCount - 1) {
        TParamUIConcept &c = concepts[count++];
        c.m_type           = TParamUIConcept::VECTOR;
        c.m_params.push_back(m_pos[i]);
        c.m_params.push_back(m_out[i]);
      }
    }

    for (int i = 0; i < anchorCount; ++i) {
      std::string suffix = std::to_string(i + 1);

      if (i > 0) {
        TParamUIConcept &c = concepts[count++];
        c.m_type           = TParamUIConcept::POINT;
        c.m_label          = labelPrefix + "In " + suffix;
        c.m_params.push_back(m_in[i]);
      }
      if (i < anchorCount - 1) {
        TParamUIConcept &c = concepts[count++];
        c.m_type           = TParamUIConcept::POINT;
        c.m_label          = labelPrefix + "Out " + suffix;
        c.m_params.push_back(m_out[i]);
      }

      TParamUIConcept &c = concepts[count++];
      c.m_type           = TParamUIConcept::POINT;
      c.m_label          = labelPrefix + "Point " + suffix;
      c.m_params.push_back(m_pos[i]);
    }
    return count;
  }

  int maxConceptCount() const { return 5 * getAnchorCount() - 4; }
};

//------------------------------------------------------------------------------

//! The parameters of a curve deformer (see TCurveDeformer): the deformed and
//! the rest curve, and the distances from the rest curve of full influence
//! (Width) and of no influence (Falloff).
class CurveDeformerParams {
public:
  CurveParams m_deformed;  // The curve, deformed
  CurveParams m_rest;      // The curve in rest pose
  TDoubleParamP m_width, m_falloff;

  //! Creates the parameters and binds them to fx as prefix + "point1",
  //! "in1", "out1", ... for the deformed curve, prefix + "restPoint1",
  //! "restIn1", "restOut1", ... for the rest one, then prefix + "width" and
  //! prefix + "falloff". By default both curves are the straight line of
  //! CurveParams::bind().
  void bind(TFx *fx, const std::string &prefix, int anchorCount,
            double halfLength) {
    m_deformed.bind(fx, prefix + "point", prefix + "in", prefix + "out",
                    anchorCount, halfLength);
    m_rest.bind(fx, prefix + "restPoint", prefix + "restIn", prefix + "restOut",
                anchorCount, halfLength);

    m_width = makeLengthParam(100.0);
    bindParam(fx, prefix + "width", m_width);

    m_falloff = makeLengthParam(300.0);
    bindParam(fx, prefix + "falloff", m_falloff);
  }

  //! Builds the curve deformer at frame, in the space that aff maps the fx's
  //! space to, and fills rest with the control points of the rest curve in
  //! that space. Returns null, with rest empty, when the Source passes
  //! through unchanged: the curve is not moved from its rest pose, the rest
  //! curve is degenerate, or nothing is within the falloff.
  std::shared_ptr<const TDeformer> buildDeformer(
      double frame, const TAffine &aff, std::vector<TPointD> &rest) const {
    rest.clear();

    std::vector<TBezierChain::Anchor> restAnchors, deformedAnchors;
    m_rest.getAnchors(frame, restAnchors);
    m_deformed.getAnchors(frame, deformedAnchors);

    // The distances follow the scale of the affine, a similarity
    double radiusScale = std::sqrt(std::fabs(aff.det()));
    double width       = m_width->getValue(frame) * radiusScale;
    double falloff     = m_falloff->getValue(frame) * radiusScale;
    if (!(std::max(width, falloff) > 0.0)) return nullptr;

    mapAnchors(aff, restAnchors);
    mapAnchors(aff, deformedAnchors);

    std::shared_ptr<TCurveDeformer> deformer = std::make_shared<TCurveDeformer>(
        restAnchors, deformedAnchors, width, falloff);
    if (!deformer->isValid() || deformer->isIdentity(1e-6 * radiusScale))
      return nullptr;

    for (const TBezierChain::Anchor &a : restAnchors)
      rest.push_back(a.m_pos), rest.push_back(a.m_in), rest.push_back(a.m_out);
    return deformer;
  }

  //! Fills concepts with the gadgets of both curves, whose labels start with
  //! labelPrefix, and returns how many (at most maxConceptCount()). The rest
  //! curve comes first: the viewer picks the gadget drawn last, so the
  //! deformed points are dragged where they lie on the rest ones.
  int getParamUIs(TParamUIConcept *concepts,
                  const std::string &labelPrefix) const {
    int count = m_rest.getParamUIs(concepts, labelPrefix + "Rest ");
    count += m_deformed.getParamUIs(concepts + count, labelPrefix);
    return count;
  }

  int maxConceptCount() const {
    return m_rest.maxConceptCount() + m_deformed.maxConceptCount();
  }
};

//==============================================================================
//    Free-Form
//==============================================================================

//! The parameters of a free-form lattice (see TFreeFormDeformer): a rest
//! rectangle, given by its center, width and height, over which a regular
//! lattice of (cells + 1) x (cells + 1) control points is laid out, and the
//! positions the control points are moved to, numbered row by row from the
//! bottom-left one.
class FreeFormParams {
  int m_cells;

public:
  TPointParamP m_center;  // Rest rectangle
  TDoubleParamP m_width, m_height;
  std::vector<TPointParamP> m_point;  // The lattice, deformed

  FreeFormParams() : m_cells(1) {}

  //! Creates the parameters of a lattice of cells x cells cells (at least 1)
  //! and binds them to fx as prefix + "center", "width", "height" and
  //! "point1" to "pointN". By default, the rest rectangle is a square of side
  //! size around the origin and the points are on the rest lattice.
  void bind(TFx *fx, const std::string &prefix, int cells, double size) {
    m_cells = std::max(cells, 1);

    m_center = makePointParam(TPointD(0.0, 0.0));
    bindParam(fx, prefix + "center", m_center);

    m_width = makeLengthParam(size);
    bindParam(fx, prefix + "width", m_width);

    m_height = makeLengthParam(size);
    bindParam(fx, prefix + "height", m_height);

    m_point.resize((m_cells + 1) * (m_cells + 1));
    for (int j = 0; j <= m_cells; ++j)
      for (int i = 0; i <= m_cells; ++i) {
        int n = j * (m_cells + 1) + i;

        m_point[n] = makePointParam(restPoint(TPointD(), size, size, i, j));
        bindParam(fx, prefix + "point" + std::to_string(n + 1), m_point[n]);
      }
  }

  int getCells() const { return m_cells; }
  int getPointCount() const { return (int)m_point.size(); }

  //! Returns the rest position, in the fx's space, of the control point
  //! (i, j) of the lattice over the rect of the passed center and size.
  TPointD restPoint(const TPointD &center, double width, double height, int i,
                    int j) const {
    return TPointD(center.x - 0.5 * width + width * i / m_cells,
                   center.y - 0.5 * height + height * j / m_cells);
  }

  //! Returns the rest rectangle and the deformed points at frame, in the fx's
  //! space.
  void getLattice(double frame, TRectD &rect,
                  std::vector<TPointD> &points) const {
    TPointD center(m_center->getValue(frame));
    double width = m_width->getValue(frame), height = m_height->getValue(frame);

    points.resize(m_point.size());
    for (int n = 0; n < (int)m_point.size(); ++n)
      points[n] = m_point[n]->getValue(frame);

    rect = TRectD(center.x - 0.5 * width, center.y - 0.5 * height,
                  center.x + 0.5 * width, center.y + 0.5 * height);
  }

  //! Builds the lattice deformer at frame, in the space that aff maps the
  //! fx's space to, and fills rest with the rest rectangle's corners in that
  //! space. Returns null, with rest empty, when the Source passes through
  //! unchanged: the points are on the rest lattice, or the rest rectangle is
  //! degenerate.
  std::shared_ptr<const TDeformer> buildDeformer(
      double frame, const TAffine &aff, std::vector<TPointD> &rest) const {
    rest.clear();

    TRectD rect;
    std::vector<TPointD> points;
    getLattice(frame, rect, points);

    // Whether it moves anything is told in the fx's space
    TFreeFormDeformer unplaced(rect, m_cells, m_cells, points);
    if (!unplaced.isValid() || unplaced.isIdentity(1e-6)) return nullptr;

    rest.push_back(aff * TPointD(rect.x0, rect.y0));
    rest.push_back(aff * TPointD(rect.x1, rect.y0));
    rest.push_back(aff * TPointD(rect.x1, rect.y1));
    rest.push_back(aff * TPointD(rect.x0, rect.y1));

    // The rest lattice and the points are in the fx's space: the deformer maps
    // them to the space of aff
    return std::make_shared<TFreeFormDeformer>(rect, m_cells, m_cells, points,
                                               aff);
  }

  //! Fills the conceptCount() concepts from the passed one with the gadgets of
  //! the lattice, whose labels start with labelPrefix. The rest rectangle
  //! comes first: the viewer picks the gadget drawn last, so the points are
  //! dragged where they lie on its sides.
  int getParamUIs(TParamUIConcept *concepts,
                  const std::string &labelPrefix) const {
    concepts[0].m_type  = TParamUIConcept::RECT;
    concepts[0].m_label = labelPrefix + "Rest";
    concepts[0].m_params.push_back(m_width);
    concepts[0].m_params.push_back(m_height);
    concepts[0].m_params.push_back(m_center);

    for (int n = 0; n < (int)m_point.size(); ++n) {
      TParamUIConcept &point = concepts[1 + n];
      point.m_type           = TParamUIConcept::POINT;
      point.m_label          = labelPrefix + "Point " + std::to_string(n + 1);
      point.m_params.push_back(m_point[n]);
    }
    return conceptCount();
  }

  int conceptCount() const { return 1 + (int)m_point.size(); }
};

//==============================================================================
//    Bone
//==============================================================================

//! The parameters of a chain of bones (see TBoneDeformer): the joints in rest
//! pose and moved, and the radii of the region of influence of the bones.
/*!
  The bone i runs from joint i to joint i + 1.
*/
class BoneChainParams {
public:
  std::vector<TPointParamP> m_point;  // The joints, deformed
  std::vector<TPointParamP> m_rest;   // The joints in rest pose
  TDoubleParamP m_inner, m_outer;

  //! Creates the parameters of jointCount joints (at least 2) and binds them
  //! to fx as prefix + "point1" to "pointN", prefix + "rest1" to "restN",
  //! then prefix + "inner" and prefix + "outer". By default, the joints are
  //! evenly spaced along a horizontal line from -halfLength to halfLength,
  //! and the moved ones are on the rest ones.
  void bind(TFx *fx, const std::string &prefix, int jointCount,
            double halfLength) {
    const double step = 2.0 * halfLength / (jointCount - 1);

    m_point.resize(jointCount);
    for (int i = 0; i < jointCount; ++i) {
      m_point[i] = makePointParam(TPointD(-halfLength + step * i, 0.0));
      bindParam(fx, prefix + "point" + std::to_string(i + 1), m_point[i]);
    }

    m_rest.resize(jointCount);
    for (int i = 0; i < jointCount; ++i) {
      m_rest[i] = makePointParam(TPointD(-halfLength + step * i, 0.0));
      bindParam(fx, prefix + "rest" + std::to_string(i + 1), m_rest[i]);
    }

    m_inner = makeLengthParam(30.0);
    bindParam(fx, prefix + "inner", m_inner);

    m_outer = makeLengthParam(90.0);
    bindParam(fx, prefix + "outer", m_outer);
  }

  int getJointCount() const { return (int)m_point.size(); }

  //! Builds the bone deformer at frame, in the space that aff maps the fx's
  //! space to, and fills rest with the rest joints in that space. Returns
  //! null, with rest empty, when the Source passes through unchanged: the
  //! joints are on the rest ones.
  std::shared_ptr<const TDeformer> buildDeformer(
      double frame, const TAffine &aff, std::vector<TPointD> &rest) const {
    const int jointCount = getJointCount();

    std::vector<TPointD> joints(jointCount);
    rest.resize(jointCount);
    for (int i = 0; i < jointCount; ++i) {
      rest[i]   = m_rest[i]->getValue(frame);
      joints[i] = m_point[i]->getValue(frame);
    }

    std::vector<TBoneDeformer::Bone> bones = TBoneDeformer::makeChain(
        rest, joints, m_inner->getValue(frame), m_outer->getValue(frame));

    // Whether it moves anything is told in the fx's space
    TBoneDeformer unplaced(bones);
    if (!unplaced.isValid() || unplaced.isIdentity(1e-6)) {
      rest.clear();
      return nullptr;
    }

    // The bones are in the fx's space: the deformer maps them to the space of
    // aff
    mapPoints(aff, rest);
    return std::make_shared<TBoneDeformer>(bones, aff);
  }

  //! Fills the conceptCount() concepts from the passed one with the gadgets of
  //! the joints, whose labels start with labelPrefix. The rest joints come
  //! first: the viewer picks the gadget drawn last, so the deformed joints are
  //! dragged where they lie on the rest ones.
  int getParamUIs(TParamUIConcept *concepts,
                  const std::string &labelPrefix) const {
    const int jointCount = getJointCount();

    for (int i = 0; i < jointCount; ++i) {
      std::string suffix = std::to_string(i + 1);

      TParamUIConcept &rest = concepts[i];
      rest.m_type           = TParamUIConcept::POINT;
      rest.m_label          = labelPrefix + "Rest Joint " + suffix;
      rest.m_params.push_back(m_rest[i]);

      TParamUIConcept &offset = concepts[jointCount + i];
      offset.m_type           = TParamUIConcept::VECTOR;
      offset.m_params.push_back(m_rest[i]);
      offset.m_params.push_back(m_point[i]);

      TParamUIConcept &joint = concepts[2 * jointCount + i];
      joint.m_type           = TParamUIConcept::POINT;
      joint.m_label          = labelPrefix + "Joint " + suffix;
      joint.m_params.push_back(m_point[i]);
    }
    return conceptCount();
  }

  int conceptCount() const { return 3 * getJointCount(); }
};

//==============================================================================
//    Shape Aware
//==============================================================================

//! The parameters of a shape aware deformer (see TShapeAwareDeformer): pins,
//! bones and the corners of a cage, each with its rest geometry and the
//! geometry it is moved to, and the settings of the weights.
/*!
  The weights depend on the rest geometry only: the last ones computed are
  kept, so that the tiles and the frames of an animation share them.
*/
class ShapeAwareParams {
public:
  typedef TShapeAwareDeformer::Handle Handle;

  TIntParamP m_resolution;
  TDoubleParamP m_margin;
  TBoolParamP m_stretch;

  std::vector<TBoolParamP> m_pinOn;
  std::vector<TPointParamP> m_pin;      // Moved to
  std::vector<TPointParamP> m_pinRest;  // Rest position
  std::vector<TDoubleParamP> m_pinAngle, m_pinScale;

  std::vector<TBoolParamP> m_boneOn;
  std::vector<TPointParamP> m_boneHead, m_boneTail;
  std::vector<TPointParamP> m_boneRestHead, m_boneRestTail;

  TBoolParamP m_cageOn;
  std::vector<TPointParamP> m_cage;      // Moved to
  std::vector<TPointParamP> m_cageRest;  // Rest

private:
  // The last weights computed, for the rest geometry in m_cacheKey
  mutable std::mutex m_cacheMutex;
  mutable std::vector<double> m_cacheKey;
  mutable std::shared_ptr<const TShapeAwareDeformer::Weights> m_cache;

  // The weights of the rest geometries used last, by every fx: the render
  // clones the fx for each frame, and a clone starts with an empty m_cache,
  // so without this the weights would be solved again for every frame
  static std::shared_ptr<const TShapeAwareDeformer::Weights> sharedWeights(
      const std::vector<double> &key, const std::vector<Handle> &handles,
      const TShapeAwareDeformer::Settings &settings) {
    typedef std::pair<std::vector<double>,
                      std::shared_ptr<const TShapeAwareDeformer::Weights>>
        Entry;
    static std::mutex mutex;
    static std::vector<Entry> recent;  // most recent first
    const std::size_t maxEntries = 8;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (auto it = recent.begin(); it != recent.end(); ++it)
        if (it->first == key) {
          std::rotate(recent.begin(), it, it + 1);
          return recent.front().second;
        }
    }
    // Solved without the lock: other fxs keep using their weights meanwhile
    std::shared_ptr<const TShapeAwareDeformer::Weights> weights =
        TShapeAwareDeformer::computeWeights(handles, settings);
    std::lock_guard<std::mutex> lock(mutex);
    recent.insert(recent.begin(), Entry(key, weights));
    if (recent.size() > maxEntries) recent.resize(maxEntries);
    return weights;
  }

  // Default positions, scaled, so that the gadgets of the handles do not lie
  // on top of each other
  static TPointD defaultPin(int i, double scale) {
    const TPointD corners[4] = {TPointD(-100.0, 100.0), TPointD(100.0, 100.0),
                                TPointD(-100.0, -100.0),
                                TPointD(100.0, -100.0)};
    const TPointD &p         = corners[i % 4];
    return TPointD(p.x * scale, p.y * scale);
  }
  static TPointD defaultBoneHead(int i, double scale) {
    return TPointD((-120.0 + 120.0 * i) * scale, 0.0);
  }
  static TPointD defaultBoneTail(int i, double scale) {
    return TPointD(120.0 * i * scale, 0.0);
  }
  static TPointD defaultCage(int i, double scale) {
    const TPointD corners[4] = {TPointD(-150.0, -150.0), TPointD(150.0, -150.0),
                                TPointD(150.0, 150.0), TPointD(-150.0, 150.0)};
    const TPointD &p         = corners[i % 4];
    return TPointD(p.x * scale, p.y * scale);
  }

public:
  //! Creates the parameters of pinCount pins, boneCount bones and a cage of
  //! cageCount corners, all off by default, and binds them to fx as prefix +
  //! "resolution", "margin", "stretch", then for each pin "pinOn", "pin",
  //! "pinRest", "pinAngle" and "pinScale", for each bone "boneOn", "boneHead",
  //! "boneTail", "boneRestHead" and "boneRestTail", and "cageOn", then for
  //! each corner "cage" and "cageRest", each followed by its number from 1.
  //! The default positions are scaled by scale.
  void bind(TFx *fx, const std::string &prefix, int pinCount, int boneCount,
            int cageCount, double scale) {
    m_resolution = TIntParamP(48);
    m_resolution->setValueRange(16, 128);
    bindParam(fx, prefix + "resolution", m_resolution);

    m_margin = makeLengthParam(0.0);
    bindParam(fx, prefix + "margin", m_margin);

    m_stretch = TBoolParamP(false);
    bindParam(fx, prefix + "stretch", m_stretch);

    m_pinOn.resize(pinCount);
    m_pin.resize(pinCount);
    m_pinRest.resize(pinCount);
    m_pinAngle.resize(pinCount);
    m_pinScale.resize(pinCount);
    for (int i = 0; i < pinCount; ++i) {
      std::string suffix = std::to_string(i + 1);

      m_pinOn[i] = TBoolParamP(false);
      bindParam(fx, prefix + "pinOn" + suffix, m_pinOn[i]);

      m_pin[i] = makePointParam(defaultPin(i, scale));
      bindParam(fx, prefix + "pin" + suffix, m_pin[i]);

      m_pinRest[i] = makePointParam(defaultPin(i, scale));
      bindParam(fx, prefix + "pinRest" + suffix, m_pinRest[i]);

      m_pinAngle[i] = TDoubleParamP(0.0);
      m_pinAngle[i]->setMeasureName("angle");
      bindParam(fx, prefix + "pinAngle" + suffix, m_pinAngle[i]);

      m_pinScale[i] = TDoubleParamP(1.0);
      m_pinScale[i]->setValueRange(0.01, 100.0);
      bindParam(fx, prefix + "pinScale" + suffix, m_pinScale[i]);
    }

    m_boneOn.resize(boneCount);
    m_boneHead.resize(boneCount);
    m_boneTail.resize(boneCount);
    m_boneRestHead.resize(boneCount);
    m_boneRestTail.resize(boneCount);
    for (int i = 0; i < boneCount; ++i) {
      std::string suffix = std::to_string(i + 1);

      m_boneOn[i] = TBoolParamP(false);
      bindParam(fx, prefix + "boneOn" + suffix, m_boneOn[i]);

      m_boneHead[i] = makePointParam(defaultBoneHead(i, scale));
      bindParam(fx, prefix + "boneHead" + suffix, m_boneHead[i]);

      m_boneTail[i] = makePointParam(defaultBoneTail(i, scale));
      bindParam(fx, prefix + "boneTail" + suffix, m_boneTail[i]);

      m_boneRestHead[i] = makePointParam(defaultBoneHead(i, scale));
      bindParam(fx, prefix + "boneRestHead" + suffix, m_boneRestHead[i]);

      m_boneRestTail[i] = makePointParam(defaultBoneTail(i, scale));
      bindParam(fx, prefix + "boneRestTail" + suffix, m_boneRestTail[i]);
    }

    m_cageOn = TBoolParamP(false);
    bindParam(fx, prefix + "cageOn", m_cageOn);

    m_cage.resize(cageCount);
    m_cageRest.resize(cageCount);
    for (int i = 0; i < cageCount; ++i) {
      std::string suffix = std::to_string(i + 1);

      m_cage[i] = makePointParam(defaultCage(i, scale));
      bindParam(fx, prefix + "cage" + suffix, m_cage[i]);

      m_cageRest[i] = makePointParam(defaultCage(i, scale));
      bindParam(fx, prefix + "cageRest" + suffix, m_cageRest[i]);
    }
  }

  //! Returns the handles that are on at frame, in the fx's space, and their
  //! rest geometry in restPoints.
  void getHandles(double frame, std::vector<Handle> &handles,
                  std::vector<TPointD> *restPoints = 0) const {
    handles.clear();
    if (restPoints) restPoints->clear();

    // Pins first, then bones and the cage: the first handles keep the nodes
    // of the weight grid that several of them claim
    for (int i = 0; i < (int)m_pin.size(); ++i) {
      if (!m_pinOn[i]->getValue()) continue;

      TPointD rest = m_pinRest[i]->getValue(frame);
      handles.push_back(Handle::makePin(rest, m_pin[i]->getValue(frame),
                                        m_pinAngle[i]->getValue(frame),
                                        m_pinScale[i]->getValue(frame)));
      if (restPoints) restPoints->push_back(rest);
    }

    const bool stretch = m_stretch->getValue();
    for (int i = 0; i < (int)m_boneHead.size(); ++i) {
      if (!m_boneOn[i]->getValue()) continue;

      TPointD restHead = m_boneRestHead[i]->getValue(frame);
      TPointD restTail = m_boneRestTail[i]->getValue(frame);
      handles.push_back(
          Handle::makeBone(restHead, restTail, m_boneHead[i]->getValue(frame),
                           m_boneTail[i]->getValue(frame), stretch));
      if (restPoints) {
        restPoints->push_back(restHead);
        restPoints->push_back(restTail);
      }
    }

    if (m_cageOn->getValue())
      for (int i = 0; i < (int)m_cage.size(); ++i) {
        TPointD rest = m_cageRest[i]->getValue(frame);
        handles.push_back(
            Handle::makeCageVertex(rest, m_cage[i]->getValue(frame)));
        if (restPoints) restPoints->push_back(rest);
      }
  }

  //! Builds the shape aware deformer at frame, placed by aff, and fills rest
  //! with the rest geometry of the handles in the space that aff maps the
  //! fx's space to. Returns null, with rest empty, when the Source passes
  //! through unchanged: there is no handle, or all of them are on their rest
  //! geometry.
  std::shared_ptr<const TDeformer> buildDeformer(
      double frame, const TAffine &aff, std::vector<TPointD> &rest) const {
    std::vector<Handle> handles;
    getHandles(frame, handles, &rest);

    bool identity = true;
    for (const Handle &h : handles)
      if (!h.isIdentity(1e-6)) identity = false;
    if (identity) {
      rest.clear();
      return nullptr;
    }

    TShapeAwareDeformer::Settings settings(m_resolution->getValue(),
                                           m_margin->getValue(frame));

    // The weights depend on the rest geometry only: keep them from one call
    // to the next, as the tiles and the frames of an animation share them
    std::shared_ptr<const TShapeAwareDeformer::Weights> weights;
    {
      std::vector<double> key =
          TShapeAwareDeformer::makeWeightsKey(handles, settings);

      std::lock_guard<std::mutex> lock(m_cacheMutex);
      if (!m_cache || key != m_cacheKey) {
        m_cache    = sharedWeights(key, handles, settings);
        m_cacheKey = key;
      }
      weights = m_cache;
    }

    // The handles are in the fx's space: the deformer places them in the
    // space of aff
    mapPoints(aff, rest);
    return std::make_shared<TShapeAwareDeformer>(handles, settings, aff,
                                                 weights);
  }

  //! Fills the conceptCount() concepts from the passed one with the gadgets of
  //! the handles, whose labels start with labelPrefix. For each kind of
  //! handle the rest gadgets come first: the viewer picks the gadget drawn
  //! last, so the moved handles are dragged where they lie on the rest ones.
  int getParamUIs(TParamUIConcept *concepts,
                  const std::string &labelPrefix) const {
    int n = 0;

    for (int i = 0; i < (int)m_pin.size(); ++i) {
      std::string suffix = std::to_string(i + 1);

      TParamUIConcept &rest = concepts[n++];
      rest.m_type           = TParamUIConcept::POINT;
      rest.m_label          = labelPrefix + "Rest Pin " + suffix;
      rest.m_params.push_back(m_pinRest[i]);

      TParamUIConcept &offset = concepts[n++];
      offset.m_type           = TParamUIConcept::VECTOR;
      offset.m_params.push_back(m_pinRest[i]);
      offset.m_params.push_back(m_pin[i]);

      TParamUIConcept &pin = concepts[n++];
      pin.m_type           = TParamUIConcept::POINT;
      pin.m_label          = labelPrefix + "Pin " + suffix;
      pin.m_params.push_back(m_pin[i]);
    }

    for (int i = 0; i < (int)m_boneHead.size(); ++i) {
      std::string suffix = std::to_string(i + 1);

      TParamUIConcept &restHead = concepts[n++];
      restHead.m_type           = TParamUIConcept::POINT;
      restHead.m_label          = labelPrefix + "Rest Bone Head " + suffix;
      restHead.m_params.push_back(m_boneRestHead[i]);

      TParamUIConcept &restTail = concepts[n++];
      restTail.m_type           = TParamUIConcept::POINT;
      restTail.m_label          = labelPrefix + "Rest Bone Tail " + suffix;
      restTail.m_params.push_back(m_boneRestTail[i]);

      TParamUIConcept &head = concepts[n++];
      head.m_type           = TParamUIConcept::POINT;
      head.m_label          = labelPrefix + "Bone Head " + suffix;
      head.m_params.push_back(m_boneHead[i]);

      TParamUIConcept &tail = concepts[n++];
      tail.m_type           = TParamUIConcept::POINT;
      tail.m_label          = labelPrefix + "Bone Tail " + suffix;
      tail.m_params.push_back(m_boneTail[i]);
    }

    for (int i = 0; i < (int)m_cage.size(); ++i) {
      std::string suffix = std::to_string(i + 1);

      TParamUIConcept &rest = concepts[n++];
      rest.m_type           = TParamUIConcept::POINT;
      rest.m_label          = labelPrefix + "Rest Cage " + suffix;
      rest.m_params.push_back(m_cageRest[i]);

      TParamUIConcept &offset = concepts[n++];
      offset.m_type           = TParamUIConcept::VECTOR;
      offset.m_params.push_back(m_cageRest[i]);
      offset.m_params.push_back(m_cage[i]);

      TParamUIConcept &cage = concepts[n++];
      cage.m_type           = TParamUIConcept::POINT;
      cage.m_label          = labelPrefix + "Cage " + suffix;
      cage.m_params.push_back(m_cage[i]);
    }
    return n;
  }

  int conceptCount() const {
    return 3 * (int)m_pin.size() + 4 * (int)m_boneHead.size() +
           3 * (int)m_cage.size();
  }
};

}  // namespace deformerfx

#endif  // DEFORMERPARAMS_H
