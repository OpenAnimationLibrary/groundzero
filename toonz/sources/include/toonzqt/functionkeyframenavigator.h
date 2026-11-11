#pragma once

#ifndef FUNCTION_KEYFRAME_NAVIGATOR_INCLUDED
#define FUNCTION_KEYFRAME_NAVIGATOR_INCLUDED

#include "toonzqt/keyframenavigator.h"
#include "tdoubleparam.h"

#include <QToolBar>
class FunctionPanel;
class FunctionSelection;
class TFrameHandle;
class FrameNavigator;

class DVAPI FunctionKeyframeNavigator final : public KeyframeNavigator {
  Q_OBJECT
  TDoubleParamP m_curve;
  FunctionSelection *m_selection = nullptr;

public:
  FunctionKeyframeNavigator(QWidget *parent);

  void setCurve(TDoubleParam *curve);
  void setSelection(FunctionSelection *selection) { m_selection = selection; }

protected:
  bool hasNext() const override;
  bool hasPrev() const override;
  bool hasKeyframes() const override;
  bool isKeyframe() const override;
  bool isFullKeyframe() const override { return isKeyframe(); }
  void toggle() override;
  void goNext() override;
  void goPrev() override;

  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

public slots:
  void onFrameSwitched() { update(); }
};

#endif
