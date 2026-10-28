#pragma once

#include "cadmiumtypes.h"
#include "toonz/txshsimplelevel.h"
#include <QDialog>
#include <QJsonObject>
#include "tfilepath.h"
#include <set>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTableWidget;
class ToonzScene;

class CadmiumPopup final : public QDialog {
  Q_OBJECT
public:
  static void open(TXshSimpleLevel *, const std::set<TFrameId> &, QWidget *);

protected:
  void closeEvent(QCloseEvent *) override;

private:
  struct Drawing {
    TFrameId fid, referenceId;
    Cadmium::Frame target, reference;
    QVector<int> styles;
    QVector<QColor> colors;
    bool ready = false;
    bool applied = false;
    bool reviewed = false;
    QString error;
  };
  explicit CadmiumPopup(QWidget *);
  void initialize(TXshSimpleLevel *, const std::set<TFrameId> &);
  bool snapshot(const TFrameId &, Cadmium::Frame &, QString &, bool captureOnly = false);
  void generate();
  void next();
  void scheduleNext();
  void submit();
  void received(const QJsonObject &);
  void failed(const QString &);
  void refresh();
  void refreshPreview();
  void updateProposalPreview();
  void apply(bool all);
  void cancel();
  void assignReference();
  bool currentScene() const;

  TXshSimpleLevelP m_level;
  ToonzScene *m_scene = nullptr;
  std::vector<Drawing> m_drawings;
  std::vector<TFrameId> m_references;
  int m_running = -1;
  bool m_cancelled = false;
  bool m_generating = false;
  bool m_inspecting = false;
  quint64 m_snapshotEpoch = 0;
  int m_backgroundTasks = 0;
  QListWidget *m_list;
  QLabel *m_status, *m_before, *m_after;
  QComboBox *m_reference, *m_rasterSource, *m_lineLevel;
  QCheckBox *m_repaint, *m_extraction;
  QDoubleSpinBox *m_paperThreshold, *m_lineThreshold;
  QTableWidget *m_regions;
  QPushButton *m_generate, *m_apply, *m_applyAll, *m_cancel;
  QPushButton *m_review;
};
