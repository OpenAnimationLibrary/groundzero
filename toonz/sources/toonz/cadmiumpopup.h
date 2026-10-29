#pragma once

#include "cadmiumconsensus.h"
#include "cadmiumregeneration.h"
#include "cadmiumsession.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/imagemanager.h"
#include <QDialog>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QTimer>
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
class QThread;
class CadmiumReviewView;

class CadmiumPopup final : public QDialog {
  Q_OBJECT
public:
  static void open(TXshSimpleLevel *, const std::set<TFrameId> &, QWidget *);
protected:
  void closeEvent(QCloseEvent *) override;
private:
  struct Drawing {
    TFrameId fid;
    QVector<TFrameId> referenceIds;
    Cadmium::Frame target;
    QByteArray labelsFingerprint;
    QVector<Cadmium::Frame> references;
    ImageBuilderP targetBuilder;
    QVector<ImageBuilderP> referenceBuilders;
    ImageBuilderP pendingTargetBuilder;
    QVector<ImageBuilderP> pendingReferenceBuilders;
    int pendingRenumberCount = -1;
    QVector<QImage> referencePreviews;
    int renumberCount = -1;
    QVector<Cadmium::RegionProposal> proposals;
    QSet<int> selectedRegions;
    QSet<int> requestedRegions;
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
  void finishDrawing();
  void prepareDrawing(int index, bool restoring);
  void analysisFinished(int index, Cadmium::Frame target,
                        QVector<Cadmium::Frame> references,
                        QVector<QImage> previews, QString error,
                        bool restoring, quint64 epoch);
  void refresh();
  void refreshPreview();
  void inspectDrawing();
  void updateProposalPreview();
  void updateReferencePreview();
  void showRegionMenu(int region, const QPoint &globalPosition);
  void setRegionDecision(int region, int candidate);
  void setRegionProtected(int region, bool protect);
  void setRegionLocked(int region, bool lock);
  void setRegionsSkipped(const QSet<int> &regions);
  void setRegionsProtected(const QSet<int> &regions, bool protect);
  void addPaletteDecision(int region, int style);
  void setSelectedRegions(const QSet<int> &regions);
  void apply(bool all);
  void cancel();
  void assignReferences();
  void optionsChanged();
  bool currentScene() const;
  TImageP composeProposal(const Drawing &, QString &error) const;
  static QImage imagePreview(const TImageP &, const Cadmium::Frame &);
  Cadmium::SessionOptions sessionOptions() const;
  Cadmium::Session makeSession() const;
  void saveSession(bool explicitSave = false);
  void restoreSession();
  void clearSession();
  void autosave();
  QString sessionCacheRoot() const;
  QString sceneIdentity() const;
  void restoreNext();
  void flushSession();

  TXshSimpleLevelP m_level;
  ToonzScene *m_scene = nullptr;
  std::vector<Drawing> m_drawings;
  std::vector<TFrameId> m_references;
  QVector<int> m_jobs;
  int m_jobPosition = -1;
  int m_running = -1;
  int m_runningReference = 0;
  int m_selectionRenumberCount = -1;
  Cadmium::RegenerationMode m_generation = Cadmium::RegenerationMode::All;
  bool m_cancelled = false;
  bool m_generating = false;
  bool m_restoring = false;
  bool m_inspecting = false;
  bool m_refreshing = false;
  quint64 m_snapshotEpoch = 0;
  int m_backgroundTasks = 0;
  int m_sessionWrites = 0;
  QThread *m_sessionWriter = nullptr;
  quint64 m_sessionRevision = 0;
  bool m_sessionDirty = false;
  QString m_sessionScene;
  QByteArray m_sessionSourceKey;
  QString m_pendingScene;
  Cadmium::Session m_pendingSession;
  Cadmium::Session m_savedSession;
  QMap<TFrameId, Cadmium::SessionDrawing> m_recovery;
  QTimer m_saveTimer;
  QListWidget *m_list;
  QListWidget *m_referenceList;
  QLabel *m_status;
  CadmiumReviewView *m_view;
  QComboBox *m_previewReference, *m_generationMode, *m_rasterSource, *m_lineLevel;
  QCheckBox *m_repaint, *m_extraction;
  QDoubleSpinBox *m_paperThreshold, *m_lineThreshold;
  QTableWidget *m_regions;
  QPushButton *m_generate, *m_apply, *m_applyAll, *m_cancel, *m_review;
  QPushButton *m_saveSession, *m_restoreSession, *m_clearSession;
};
