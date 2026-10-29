// Search-field/results-list interaction adapted from the UI template's
// CommandPalette. Native actions and context are supplied by the adapter.

#include "commandpalette.h"

#include <QApplication>
#include <QEvent>
#include <QHeaderView>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QScrollBar>
#include <QSet>
#include <QShowEvent>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {
QString displayText(const QString &text) {
  // Strip menu mnemonics without changing a literal escaped ampersand.
  QString result;
  for (int i = 0; i < text.size(); ++i) {
    if (text[i] != QLatin1Char('&')) {
      result += text[i];
    } else if (i + 1 < text.size() && text[i + 1] == QLatin1Char('&')) {
      result += QLatin1Char('&');
      ++i;
    }
  }
  return result.trimmed();
}

bool usable(const CommandPaletteEntry &entry) {
  return entry.action && entry.available && entry.action->isEnabled() &&
         entry.action->isVisible() && !entry.action->isSeparator() &&
         !entry.action->menu();
}
}  // namespace

CommandPalette::CommandPalette(QWidget *parent)
    : QWidget(parent)
    , m_search(new QLineEdit(this))
    , m_results(new QTreeWidget(this))
    , m_hint(new QLabel(this))
    , m_refreshTimer(new QTimer(this)) {
  setObjectName(QStringLiteral("CommandPalette"));
  m_search->setObjectName(QStringLiteral("commandPaletteSearch"));
  m_results->setObjectName(QStringLiteral("commandPaletteResults"));
  m_hint->setObjectName(QStringLiteral("commandPaletteHint"));
  setFocusProxy(m_search);
  m_search->setClearButtonEnabled(true);
  m_results->setColumnCount(3);
  m_results->setRootIsDecorated(false);
  m_results->setAlternatingRowColors(true);
  m_results->setSelectionMode(QAbstractItemView::SingleSelection);
  m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_results->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_results->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_results->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  m_hint->setWordWrap(true);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->setSpacing(4);
  layout->addWidget(m_search);
  layout->addWidget(m_results, 1);
  layout->addWidget(m_hint);

  m_search->installEventFilter(this);
  m_results->installEventFilter(this);
  connect(m_search, &QLineEdit::textChanged, this,
          [this]() { rebuildResults(true); });
  connect(m_results, &QTreeWidget::itemActivated, this,
          [this](QTreeWidgetItem *, int) { invokeCurrent(); });
  // A registry can acquire new room/custom-panel commands after construction.
  // Refresh while visible, preserving the query, selection and scroll position.
  m_refreshTimer->setInterval(250);
  connect(m_refreshTimer, &QTimer::timeout, this,
          &CommandPalette::refreshCommands);
  retranslate();
}

void CommandPalette::setSource(Source source) {
  m_source = std::move(source);
  refreshCommands();
}

void CommandPalette::setInvoker(Invoker invoker) {
  m_invoker = std::move(invoker);
}

void CommandPalette::beginSearch() {
  refreshCommands();
  m_search->setFocus(Qt::ShortcutFocusReason);
  m_search->selectAll();
}

void CommandPalette::refreshCommands() {
  if (m_invoking) return;
  QVector<CommandPaletteEntry> entries = m_source ? m_source()
                                                 : QVector<CommandPaletteEntry>();
  QSet<QAction *> seenActions;
  QSet<QString> seenIds;
  m_entries.clear();
  for (const CommandPaletteEntry &entry : entries) {
    QAction *action = entry.action.data();
    if (!action || entry.id.isEmpty() || !action->isVisible() ||
        action->isSeparator() || action->menu() ||
        seenActions.contains(action) || seenIds.contains(entry.id))
      continue;
    seenActions.insert(action);
    seenIds.insert(entry.id);
    m_entries.push_back(entry);
  }
  std::sort(m_entries.begin(), m_entries.end(),
            [](const CommandPaletteEntry &a, const CommandPaletteEntry &b) {
              const int order = QString::localeAwareCompare(
                  displayText(a.action->text()), displayText(b.action->text()));
              return order ? order < 0 : a.id < b.id;
            });

  QStringList signature;
  for (const CommandPaletteEntry &entry : m_entries) {
    QAction *action = entry.action.data();
    signature << entry.id << entry.category << action->text()
              << action->shortcut().toString(QKeySequence::NativeText)
              << QString::number(usable(entry))
              << QString::number(action->isCheckable())
              << QString::number(action->isChecked())
              << QString::number(action->icon().cacheKey());
  }
  // Qt 5's QList equality uses an MSVC iterator extension removed in newer STL.
  if (signature.size() == m_signature.size() &&
      std::equal(signature.cbegin(), signature.cend(), m_signature.cbegin()))
    return;
  m_signature = signature;
  rebuildResults(false);
}

void CommandPalette::rebuildResults(bool resetSelection) {
  const QString selected = !resetSelection && m_results->currentItem()
                               ? m_results->currentItem()->data(0, Qt::UserRole).toString()
                               : QString();
  const int scroll = resetSelection ? 0 : m_results->verticalScrollBar()->value();
  m_results->clear();
  QTreeWidgetItem *preferred = nullptr;
  QTreeWidgetItem *first = nullptr;
  const QString query = m_search->text().trimmed();
  for (const CommandPaletteEntry &entry : m_entries) {
    QAction *action = entry.action.data();
    if (!action || !action->isVisible()) continue;
    const QString text = displayText(action->text());
    if (!query.isEmpty() && !text.contains(query, Qt::CaseInsensitive) &&
        !entry.id.contains(query, Qt::CaseInsensitive))
      continue;
    const bool enabled = usable(entry);
    auto *item = new QTreeWidgetItem(m_results);
    item->setText(0, action->isCheckable()
                         ? (action->isChecked() ? QString::fromUtf8("\xE2\x9C\x93 ")
                                               : QStringLiteral("   ")) + text
                         : text);
    item->setIcon(0, action->icon());
    item->setText(1, action->shortcut().toString(QKeySequence::NativeText));
    item->setText(2, entry.category);
    item->setData(0, Qt::UserRole, entry.id);
    item->setToolTip(0, enabled ? action->toolTip()
                              : tr("Unavailable in the current context."));
    item->setFlags(Qt::ItemIsSelectable |
                   (enabled ? Qt::ItemIsEnabled : Qt::NoItemFlags));
    if (enabled && !first) first = item;
    if (enabled && selected == entry.id) preferred = item;
  }
  m_results->setCurrentItem(preferred ? preferred : first);
  m_results->verticalScrollBar()->setValue(scroll);
  m_hint->setText(m_results->topLevelItemCount()
                     ? tr("Enter runs the selected command. Esc returns to your work.")
                     : tr("No matching commands."));
}

void CommandPalette::moveCurrent(int direction) {
  const int count = m_results->topLevelItemCount();
  int index = m_results->indexOfTopLevelItem(m_results->currentItem());
  if (index < 0) index = direction > 0 ? -1 : count;
  for (int i = index + direction; i >= 0 && i < count; i += direction) {
    QTreeWidgetItem *item = m_results->topLevelItem(i);
    if (!(item->flags() & Qt::ItemIsEnabled)) continue;
    m_results->setCurrentItem(item);
    m_results->scrollToItem(item);
    break;
  }
}

void CommandPalette::invokeCurrent() {
  if (m_invoking || !m_results->currentItem()) return;
  const QString id = m_results->currentItem()->data(0, Qt::UserRole).toString();
  // Requery the provider: target eligibility can change without QAction state
  // changing (for example a picked region or the originating panel).
  const QVector<CommandPaletteEntry> entries =
      m_source ? m_source() : QVector<CommandPaletteEntry>();
  for (const CommandPaletteEntry &entry : entries) {
    if (entry.id != id || !usable(entry)) continue;
    m_invoking = true;
    const QPointer<CommandPalette> guard(this);
    const bool invoked = m_invoker ? m_invoker(entry)
                                  : (entry.action->trigger(), true);
    // Quit, room changes and panel commands may destroy the palette.
    if (!guard) return;
    m_invoking = false;
    if (invoked) {
      m_search->clear();
      emit commandInvoked(id);
    }
    if (guard) refreshCommands();
    return;
  }
  refreshCommands();
}

void CommandPalette::cancelSearch() {
  m_search->clear();
  emit cancelled();
}

bool CommandPalette::eventFilter(QObject *object, QEvent *event) {
  if (object == m_search || object == m_results) {
    if (event->type() == QEvent::ShortcutOverride) {
      // Commands use ApplicationShortcut, including single-letter tool keys.
      // While searching, let the input/list handle these keys locally.
      event->accept();
      return true;
    }
    if (event->type() == QEvent::KeyPress) {
      auto *key = static_cast<QKeyEvent *>(event);
      if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
        if (!key->isAutoRepeat()) invokeCurrent();
        return true;
      }
      if (key->key() == Qt::Key_Escape) {
        cancelSearch();
        return true;
      }
      if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
        moveCurrent(key->key() == Qt::Key_Down ? 1 : -1);
        return true;
      }
      if (object == m_results && !key->text().isEmpty() &&
          !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier |
                               Qt::MetaModifier))) {
        m_search->setFocus(Qt::OtherFocusReason);
        QApplication::sendEvent(m_search, event);
        return true;
      }
    }
  }
  return QWidget::eventFilter(object, event);
}

void CommandPalette::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  refreshCommands();
  m_refreshTimer->start();
}

void CommandPalette::hideEvent(QHideEvent *event) {
  m_refreshTimer->stop();
  QWidget::hideEvent(event);
}

void CommandPalette::changeEvent(QEvent *event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) {
    retranslate();
    m_signature.clear();
    refreshCommands();
  }
}

void CommandPalette::retranslate() {
  m_search->setPlaceholderText(tr("Search commands..."));
  m_search->setAccessibleName(tr("Search commands"));
  m_results->setHeaderLabels({tr("Command"), tr("Shortcut"), tr("Category")});
  m_results->setAccessibleName(tr("Matching commands"));
  rebuildResults(false);
}
