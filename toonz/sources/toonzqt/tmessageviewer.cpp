#include "toonzqt/tmessageviewer.h"
#include "toonzqt/dvdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace {
constexpr qint64 kHistoryTextBytes = 16 * 1024 * 1024;
constexpr int kRecordCharacters = 32 * 1024;

QIcon severityIcon(int severity) {
  switch (severity) {
  case DVGui::CRITICAL: return QIcon(":Resources/tmsg_error.svg");
  case DVGui::WARNING: return QIcon(":Resources/tmsg_warning.svg");
  default: return QIcon(":Resources/tmsg_info.svg");
  }
}
}  // namespace

class MySortFilterProxyModel final : public QSortFilterProxyModel {
public:
  explicit MySortFilterProxyModel(QObject *parent)
      : QSortFilterProxyModel(parent) {}
  bool filterAcceptsRow(int row, const QModelIndex &parent) const override {
    const auto *viewer = static_cast<TMessageViewer *>(this->parent());
    const int severity = sourceModel()->index(row, 0, parent)
                             .data(TMessageRepository::SeverityRole).toInt();
    if (severity == DVGui::INFORMATION) return viewer->m_greenCheck->isChecked();
    if (severity == DVGui::WARNING) return viewer->m_yellowCheck->isChecked();
    if (severity == DVGui::CRITICAL) return viewer->m_redCheck->isChecked();
    return true;
  }
};

TMessageRepository::TMessageRepository()
    : m_sim(new QStandardItemModel(this)) {}

TMessageRepository *TMessageRepository::instance() {
  // Application startup creates the repository before worker threads start.
  static TMessageRepository *repository = [] {
    auto *result = new TMessageRepository();
    result->setParent(qApp);
    QObject::connect(TMsgCore::instance(), &TMsgCore::sendMessage, result,
                     &TMessageRepository::messageReceived);
    return result;
  }();
  return repository;
}

void TMessageRepository::clear() {
  if (QThread::currentThread() != thread()) {
    QMetaObject::invokeMethod(this, [this] { clear(); }, Qt::QueuedConnection);
    return;
  }
  emit historyAboutToChange();
  m_sim->clear();
  m_textBytes = 0;
  m_discarded = 0;
  emit historyChanged();
}

void TMessageRepository::trimHistory() {
  int remove = 0;
  while (remove < m_sim->rowCount() &&
         (m_sim->rowCount() - remove > m_historyLimit ||
          m_textBytes > kHistoryTextBytes)) {
    m_textBytes -= m_sim->item(remove)->data(TextBytesRole).toLongLong();
    ++remove;
  }
  if (remove) {
    m_sim->removeRows(0, remove);
    m_discarded += remove;
  }
}

void TMessageRepository::setHistoryLimit(int limit) {
  if (QThread::currentThread() != thread()) {
    QMetaObject::invokeMethod(this, [this, limit] { setHistoryLimit(limit); },
                              Qt::QueuedConnection);
    return;
  }
  limit = std::max(1, std::min(10000, limit));
  if (m_historyLimit == limit) return;
  emit historyAboutToChange();
  m_historyLimit = limit;
  trimHistory();
  emit historyChanged();
}

void TMessageRepository::messageReceived(int type, const QString &message) {
  if (QThread::currentThread() != thread()) {
    QMetaObject::invokeMethod(this, [this, type, message] {
      messageReceived(type, message);
    }, Qt::QueuedConnection);
    return;
  }
  if (type != DVGui::INFORMATION && type != DVGui::WARNING &&
      type != DVGui::CRITICAL) return;

  const QDateTime timestamp = QDateTime::currentDateTimeUtc();
  const quint64 sequence = ++m_nextSequence;
  QString text = message;
  if (text.size() > kRecordCharacters)
    text = text.left(kRecordCharacters) + tr("\n[Message truncated in history]");
  const QString display = timestamp.toLocalTime().toString("hh:mm:ss.zzz") +
                          "  " + text;
  auto *item = new QStandardItem(severityIcon(type), display);
  item->setData(type, SeverityRole);
  item->setData(timestamp, TimestampRole);
  item->setData(QVariant::fromValue(sequence), SequenceRole);
  item->setData(text, MessageRole);
  const QString tooltip = timestamp.toLocalTime().toString(Qt::ISODateWithMs);
  item->setToolTip(tooltip);
  const qint64 bytes = (qint64(text.size()) + display.size() + tooltip.size()) * sizeof(QChar);
  item->setData(bytes, TextBytesRole);

  emit historyAboutToChange();
  m_textBytes += bytes;
  m_sim->appendRow(item);
  trimHistory();
  emit historyChanged();
  emit recordAdded(sequence, timestamp, type, message);

  // Retain existing popup semantics and the complete error message.
  if (type == DVGui::CRITICAL ||
      (type == DVGui::WARNING && !TMessageViewer::isTMsgVisible()))
    DVGui::MsgBoxInPopup(DVGui::MsgType(type), message);
}

std::vector<QPointer<TMessageViewer>> TMessageViewer::m_tmsgViewers;

bool TMessageViewer::isTMsgVisible() {
  for (const auto &viewer : m_tmsgViewers)
    if (viewer && viewer->isVisible()) return true;
  return false;
}

TMessageViewer::TMessageViewer(QWidget *parent) : QFrame(parent) {
  m_tmsgViewers.push_back(this);
  setFrameStyle(QFrame::StyledPanel);
  setObjectName("OnePixelMarginFrame");
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  auto *controls = new QHBoxLayout();
  controls->setContentsMargins(4, 2, 4, 2);
  m_displayLabel = new QLabel(this);
  controls->addWidget(m_displayLabel);
  m_redCheck = new QCheckBox(this);
  m_yellowCheck = new QCheckBox(this);
  m_greenCheck = new QCheckBox(this);
  for (auto *check : {m_redCheck, m_yellowCheck, m_greenCheck}) {
    check->setChecked(true);
    controls->addWidget(check);
    connect(check, &QCheckBox::stateChanged, this, &TMessageViewer::refreshFilter);
  }
  controls->addStretch();
  m_clearButton = new QPushButton(this);
  controls->addWidget(m_clearButton);
  connect(m_clearButton, &QPushButton::clicked, this, &TMessageViewer::onClicked);
  layout->addLayout(controls);

  m_list = new QListView(this);
  m_list->setObjectName("MessageHistory");
  m_list->setAlternatingRowColors(true);
  m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  m_proxyModel = new MySortFilterProxyModel(this);
  m_proxyModel->setDynamicSortFilter(true);
  auto *repository = TMessageRepository::instance();
  m_proxyModel->setSourceModel(repository->getModel());
  m_list->setModel(m_proxyModel);
  layout->addWidget(m_list);
  m_historyLabel = new QLabel(this);
  m_historyLabel->setWordWrap(true);
  layout->addWidget(m_historyLabel);
  connect(repository, &TMessageRepository::historyAboutToChange, this,
          &TMessageViewer::rememberPosition);
  connect(repository, &TMessageRepository::historyChanged, this, [this] {
    if (m_restorePending) return;
    m_restorePending = true;
    // A burst of worker messages should cause one layout, not one expensive
    // full-history layout per event.
    QTimer::singleShot(0, this, [this] {
      m_restorePending = false;
      restorePosition();
    });
  });
  retranslate();
}

TMessageViewer::~TMessageViewer() {
  m_tmsgViewers.erase(std::remove_if(m_tmsgViewers.begin(), m_tmsgViewers.end(),
                                   [this](const QPointer<TMessageViewer> &view) {
                                     return !view || view.data() == this;
                                   }), m_tmsgViewers.end());
}

void TMessageViewer::onClicked(bool) { TMessageRepository::instance()->clear(); }

void TMessageViewer::refreshFilter(int) { m_proxyModel->invalidate(); }

void TMessageViewer::rememberPosition() {
  if (m_restorePending) return;
  const auto *bar = m_list->verticalScrollBar();
  m_follow = bar->value() >= bar->maximum();
  const QModelIndex top = m_list->indexAt(QPoint(1, 1));
  m_anchorSequence = top.data(TMessageRepository::SequenceRole).toULongLong();
  m_anchorOffset = top.isValid() ? m_list->visualRect(top).top() : 0;
}

void TMessageViewer::restorePosition() {
  if (!isVisible()) {
    retranslate();
    return;
  }
  m_list->doItemsLayout();
  if (m_follow) {
    m_list->scrollToBottom();
  } else if (m_anchorSequence) {
    for (int row = 0; row < m_proxyModel->rowCount(); ++row) {
      const QModelIndex index = m_proxyModel->index(row, 0);
      if (index.data(TMessageRepository::SequenceRole).toULongLong() ==
          m_anchorSequence) {
        m_list->scrollTo(index, QAbstractItemView::PositionAtTop);
        auto *bar = m_list->verticalScrollBar();
        bar->setValue(bar->value() - m_anchorOffset);
        break;
      }
    }
  }
  retranslate();
}

void TMessageViewer::retranslate() {
  m_displayLabel->setText(tr("Display:"));
  m_redCheck->setText(tr("Errors"));
  m_yellowCheck->setText(tr("Warnings"));
  m_greenCheck->setText(tr("Info"));
  m_clearButton->setText(tr("Clear"));
  const auto *repository = TMessageRepository::instance();
  m_historyLabel->setText(tr("Recent history: %1 messages. Older messages discarded: %2.")
                             .arg(repository->getModel()->rowCount())
                             .arg(repository->discardedCount()));
}

void TMessageViewer::changeEvent(QEvent *event) {
  QFrame::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) retranslate();
}
