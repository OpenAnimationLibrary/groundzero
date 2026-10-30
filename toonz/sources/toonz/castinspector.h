#pragma once

#include "toonz/txshlevel.h"
#include <QWidget>
#include <memory>
#include <vector>

// A persistent child pane of Scene Cast. Its retained levels are pruned against
// the active scene before every action; changing selection stops audition.
class CastInspector final : public QWidget {
  struct State;
  std::unique_ptr<State> m;
  void refreshRows();
  void selectRow();
  void preview();
  void stopAudio();
  void setCover(bool present);
protected:
  void hideEvent(QHideEvent *event) override;
public:
  explicit CastInspector(QWidget *parent);
  ~CastInspector();
  void setLevels(const std::vector<TXshLevel *> &levels);
};
