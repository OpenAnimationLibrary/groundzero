#pragma once

#ifndef MASTERCONTROLLERSPANE_H
#define MASTERCONTROLLERSPANE_H

#include "toonz/tstageobjectid.h"

#include <QFrame>

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
class QVBoxLayout;

//=============================================================================
// MasterControllersPane
//-----------------------------------------------------------------------------
/*! Lists the master controllers of the current xsheet and lets the user
    animate them with a slider each.

    A master controller is an ordinary pegbar whose name starts with "ctrl_".
    Its value is the pegbar's Rotation channel, which is unit-free in
    expressions, and other channels are driven by it through ordinary
    expressions such as "peg3.rot*30". Everything is stored as pegbars and
    expressions, so no new scene data is needed.                         */

class MasterControllersPane final : public QFrame {
  Q_OBJECT

  struct Row {
    TStageObjectId m_id;
    QSlider *m_slider;
    QDoubleSpinBox *m_field;
    QPushButton *m_keyButton;
  };

  typedef std::vector<std::pair<TStageObjectId, std::string>> Controllers;

  QVBoxLayout *m_listLayout;
  QLabel *m_emptyLabel;
  std::vector<Row> m_rows;
  Controllers m_controllers;

  // Slider drag state: the keyframe setter is created on the first move,
  // so that clicking the handle without moving it does not set a key.
  TStageObjectId m_dragId;
  bool m_dragging;
  KeyframeSetter *m_dragSetter;

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
  void beginDrag(const TStageObjectId &id);
  void dragTo(double value);
  void endDrag();

protected:
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

protected slots:
  void refresh();
  void updateValues();
  void onNewController();
  void onBindChannel();
};

#endif  // MASTERCONTROLLERSPANE_H
