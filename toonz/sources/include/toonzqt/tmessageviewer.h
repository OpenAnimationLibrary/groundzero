#pragma once

#include <QFrame>
#include <QModelIndex>
#include <QListView>
#include <QDateTime>
#include <QPointer>
#include <vector>

#include "tcommon.h"
#include "tmsgcore.h"

#undef DVAPI
#undef DVVAR
#ifdef TOONZQT_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

class MySortFilterProxyModel;
class QCheckBox;
class QStandardItemModel;
class QLabel;
class QPushButton;

class DVAPI TMessageRepository final : public QObject {
  QStandardItemModel *m_sim;
  int m_historyLimit = 1000;
  qint64 m_textBytes = 0;
  quint64 m_nextSequence = 0;
  quint64 m_discarded = 0;
  void trimHistory();

  Q_OBJECT
public:
  enum ItemRole {
    SeverityRole = Qt::UserRole + 1,
    TimestampRole,
    SequenceRole,
    MessageRole,
    TextBytesRole
  };
  static TMessageRepository *instance();
  TMessageRepository();
  QStandardItemModel *getModel() const { return m_sim; }
  void clear();
  void setHistoryLimit(int limit);
  int historyLimit() const { return m_historyLimit; }
  quint64 discardedCount() const { return m_discarded; }
  qint64 retainedTextBytes() const { return m_textBytes; }

public slots:
  void messageReceived(int, const QString &);

signals:
  void historyAboutToChange();
  void historyChanged();
  // Emitted exactly once on ingestion, independently of the number of views.
  void recordAdded(quint64 sequence, const QDateTime &timestamp, int severity,
                   const QString &text);
  void openMessageCenter();  // TMessageRepository emits this signal to indicate
                             // that the TMessageViewer should be made visible
};

//---------------------------------------------------------------------------------------

class DVAPI TMessageViewer final : public QFrame {
  Q_OBJECT

protected:
  static std::vector<QPointer<TMessageViewer>> m_tmsgViewers;
  MySortFilterProxyModel *m_proxyModel;
  QListView *m_list;
  QLabel *m_displayLabel, *m_historyLabel;
  QPushButton *m_clearButton;
  bool m_follow = true;
  bool m_restorePending = false;
  quint64 m_anchorSequence = 0;
  int m_anchorOffset = 0;
  void rememberPosition();
  void restorePosition();
  void retranslate();
  void changeEvent(QEvent *) override;

public:
  QCheckBox *m_redCheck, *m_greenCheck, *m_yellowCheck;
  TMessageViewer(QWidget *);
  ~TMessageViewer() override;
  static bool isTMsgVisible();
public slots:
  void onClicked(bool);
  void refreshFilter(int);
};
