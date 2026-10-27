#pragma once

#ifndef PARAMCMD_INCLUDED
#define PARAMCMD_INCLUDED

#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include <set>

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

class KeyframesUndo;
class TSceneHandle;

class DVAPI KeyframeSetter {
  TDoubleParamP m_param;
  int m_kIndex;
  std::set<int> m_indices;
  int m_extraDFrame;  // used by moveKeyframes
  bool m_enableUndo;
  TDoubleKeyframe m_keyframe;
  KeyframesUndo *m_undo;
  bool m_changed;
  double m_pixelRatio;  // frame pixel size / value pixel size

  double getNorm(const TPointD &p) const {
    double y = p.y * m_pixelRatio;
    return sqrt(p.x * p.x + y * y);
  }
  void getRotatingSpeedHandles(
      std::vector<std::pair<double, int>> &rotatingSpeeds, TDoubleParam *param,
      int kIndex) const;

public:
  KeyframeSetter(TDoubleParam *param, int kIndex = -1, bool enableUndo = true);
  ~KeyframeSetter();

  TDoubleParam *getCurve() const { return m_param.getPointer(); }

  // pixel ratio refers to graph panel. It is necessary to move along a circular
  // arc
  void setPixelRatio(double pixelRatio) { m_pixelRatio = pixelRatio; }
  double getPixelRatio() const { return m_pixelRatio; }

  void selectKeyframe(int kIndex);

  // create a new keyframe, select it and returns its k-index
  // (if a keyframe already exists at frame then it is equivalent to
  // selectKeyframe)
  // note: call createKeyframe() when no other keyframes are selected
  int createKeyframe(double frame);

  bool isSelected(int index) const { return m_indices.count(index) > 0; }

  void moveKeyframes(int dFrame, double dValue);

  bool isSpeedInOut(int segmentIndex) const;
  bool isEaseInOut(int segmentIndex) const;  // true also if EaseInOutPercentage

  // the following methods apply if only a single keyframe has been selected
  void setType(TDoubleKeyframe::Type type);
  void setType(int kIndex, TDoubleKeyframe::Type type);
  void setStep(int step);
  void setExpression(std::string expression);
  void setSimilarShape(std::string expression, double offset);
  void setFile(const TDoubleKeyframe::FileParams &params);
  void setUnitName(std::string unitName);

  void setValue(double value);

  void linkHandles();
  void unlinkHandles();

  void setSpeedIn(const TPointD &speedIn);
  void setSpeedOut(const TPointD &speedOut);
  void setEaseIn(double easeIn);
  void setEaseOut(double easeOut);

  // set the curve params adaptively by clicking apply button
  void setAllParams(int step, TDoubleKeyframe::Type comboType,
                    const TPointD &speedIn, const TPointD &speedOut,
                    std::string expressionText, std::string unitName,
                    const TDoubleKeyframe::FileParams &fileParam,
                    double similarShapeOffset);

  // addUndo is called automatically (if needed) in the dtor.
  // it is also possible to call it explicitly.
  void addUndo();

  static void setValue(TDoubleParam *curve, double frame, double value) {
    KeyframeSetter setter(curve);
    setter.createKeyframe(frame);
    setter.setValue(value);
  }
  static void removeKeyframeAt(TDoubleParam *curve, double frame);

  // Sets a key at frame whose value is favour of the way from the previous
  // key's value to the next one's (0 = previous, 1 = next; values outside
  // [0, 1] give an anticipation or an overshoot). Returns false, changing
  // nothing, if the curve has a key at frame, has no key before or after it,
  // or the segment there is an Expression, Similar Shape or File one.
  static bool insertBreakdown(TDoubleParam *curve, int frame, double favour);

  // Sets the segment starting at the key kIndex to Ease In / Ease Out (%)
  // interpolation. easeOut leaves the first key and easeIn reaches the next
  // one, as percentages of the segment length; their sum is cropped to 100.
  static void setSegmentEase(TDoubleParam *curve, int kIndex, double easeOut,
                             double easeIn);

  static void enableCycle(TDoubleParam *curve, bool enabled,
                          TSceneHandle *sceneHandle = nullptr);
};

#endif
