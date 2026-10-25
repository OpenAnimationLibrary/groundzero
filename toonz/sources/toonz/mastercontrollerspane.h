#pragma once

#ifndef MASTERCONTROLLERSPANE_H
#define MASTERCONTROLLERSPANE_H

#include "toonz/tstageobjectid.h"

#include <QFrame>

#include <map>
#include <string>
#include <utility>
#include <vector>

class TStageObject;
class TDoubleParam;
class KeyframeSetter;
class QLabel;
class QSlider;
class QDoubleSpinBox;
class QPushButton;
class QComboBox;
class QVBoxLayout;
class QWidget;
class MasterControllerPad;

//=============================================================================
// MasterControllersPane
//-----------------------------------------------------------------------------
/*! Lists the master controllers of the current xsheet and lets the user
    animate them with a slider each.

    A master controller is an ordinary pegbar whose name starts with "ctrl_".
    Its value is the pegbar's Rotation channel, which is unit-free in
    expressions, and other channels are driven by it through ordinary
    expressions such as "peg3.rot*30". Everything is stored as pegbars and
    expressions, so no new scene data is needed.

    Each row can show its controller as a slider, as a 2D pad (which sets two
    controllers at once) or as a pose blend (a slider whose controller drives
    the weights of two offset layers made from two poses, see onBindPoses()).
    The widget mode is a property of the panel, not of the scene.         */

class MasterControllersPane final : public QFrame {
  Q_OBJECT

  enum WidgetMode { SliderMode, PadMode, PoseBlendMode };

  struct Row {
    TStageObjectId m_id;
    QComboBox *m_modeCombo;
    QSlider *m_slider;
    MasterControllerPad *m_pad;
    QWidget *m_padYBox;  //!< The Y controller chooser of the pad
    QComboBox *m_padYCombo;
    QPushButton *m_bindPosesButton;
    QDoubleSpinBox *m_field;
    QPushButton *m_keyButton;
  };

  typedef std::vector<std::pair<TStageObjectId, std::string>> Controllers;

  QVBoxLayout *m_listLayout;
  QLabel *m_emptyLabel;
  std::vector<Row> m_rows;
  Controllers m_controllers;

  // The widget mode of the controllers, and the controller giving the Y of
  // their pad. The ones not listed get the defaults.
  std::map<TStageObjectId, int> m_modes;
  std::map<TStageObjectId, TStageObjectId> m_padY;

  // Slider or pad drag state: the keyframe setters are created on the first
  // move, so that clicking the handle without moving it does not set a key.
  // A pad drag has a second setter, for its Y controller.
  TStageObjectId m_dragId;
  TStageObjectId m_dragYId;
  bool m_dragging;
  bool m_dragHasY;
  KeyframeSetter *m_dragSetter;
  KeyframeSetter *m_dragYSetter;

public:
  MasterControllersPane(QWidget *parent       = 0,
                        Qt::WindowFlags flags = Qt::WindowFlags());
  ~MasterControllersPane();

  static bool isMasterController(TStageObject *obj);

private:
  Controllers getControllers() const;
  TDoubleParam *getControllerCurve(const TStageObjectId &id) const;
  void rebuild();

  void setValue(const TStageObjectId &id, double value);
  void toggleKey(const TStageObjectId &id);
  void beginDrag(const TStageObjectId &id,
                 const TStageObjectId &yId = TStageObjectId::NoneId);
  void dragTo(double value, double yValue = 0.0);
  void endDrag();

  int getMode(const TStageObjectId &id) const;
  TStageObjectId getPadY(const TStageObjectId &id) const;
  void applyMode(const Row &row, int mode) const;
  bool hasPoseBlendLayers(const TStageObjectId &id) const;
  void bindPoses(const TStageObjectId &preselected);

protected:
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

protected slots:
  void refresh();
  void onSceneSwitched();
  void updateValues();
  void onNewController();
  void onBindChannel();
  void onBindPoses();
  void onBakeDrawingSwap();
};

#endif  // MASTERCONTROLLERSPANE_H
