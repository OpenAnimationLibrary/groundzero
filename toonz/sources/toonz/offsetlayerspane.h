#pragma once

#ifndef OFFSETLAYERSPANE_H
#define OFFSETLAYERSPANE_H

#include "toonz/tstageobjectid.h"

#include <QFrame>

#include <string>
#include <utility>
#include <vector>

class TStageObject;
class QCheckBox;
class QLabel;
class QLineEdit;
class QSlider;
class QVBoxLayout;

//=============================================================================
// OffsetLayersPane
//-----------------------------------------------------------------------------
/*! Lists the offset layers of the current stage object (see
    TStageObject::OffsetLayer): their enabling, name and weight, and creates,
    deletes and bakes them.

    The curves of the layers are edited in the Function Editor, where they are
    listed under the stage object as "<layer>/<channel>".               */

class OffsetLayersPane final : public QFrame {
  Q_OBJECT

  struct Row {
    QCheckBox *m_enabled;
    QLineEdit *m_name;
    QSlider *m_slider;
    QLabel *m_value;
  };

  typedef std::vector<std::pair<std::wstring, bool>> Signature;

  QLabel *m_objectLabel;
  QLabel *m_emptyLabel;
  QVBoxLayout *m_listLayout;
  std::vector<Row> m_rows;

  // What the rows have been built for
  TStageObjectId m_objectId;
  Signature m_signature;

public:
  OffsetLayersPane(QWidget *parent       = 0,
                   Qt::WindowFlags flags = Qt::WindowFlags());

private:
  TStageObject *getCurrentObject() const;
  void rebuild();
  void notifyLayersChanged();

  void setWeight(int index, int percent);
  void rename(int index, const QString &name);

protected:
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

protected slots:
  void refresh();
  void updateValues();
  void onNewLayer();
  void onTrainFromKeys();
  void onDeleteLayer(int index);
  void onBakeLayer(int index);
};

#endif  // OFFSETLAYERSPANE_H
