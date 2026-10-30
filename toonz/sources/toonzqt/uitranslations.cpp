#include "toonzqt/uitranslations.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QGroupBox>
#include <QHash>
#include <QLabel>
#include <QMenu>
#include <QMetaProperty>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QTabBar>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <algorithm>
#include <map>
#include <vector>

namespace UiTranslations {
namespace {
struct Caption {
  QString value;  // Own the storage used as the identity key.
  Recipe translate;
  QString canonicalKey;
};
struct Binding {
  QPointer<QObject> object;
  QByteArray property;
  QString assigned;
  Recipe translate;
  bool explicitSource = false;
  unsigned long long revision = 0;
  // Refresh applies bindings in this order: a property keeps its place when
  // it is captured again and moves to the end when it is assigned anew.
  unsigned long long order = 0;
};
struct LiteralProperty {
  QPointer<QObject> object;
  QByteArray property;
  QString assigned;
};
struct Callback {
  QPointer<QObject> owner;
  std::function<void()> invoke;
};
struct ItemBinding {
  QPointer<QComboBox> combo;
  int index;
  QVariant identity;
  QString assigned;
  Recipe translate;
};
struct TabBinding {
  QPointer<QTabBar> tabs;
  int index;
  QString assigned;
  Recipe translate;
};
// One owner per (object, property). An entry whose object is gone is stale:
// its address may already belong to a new object, so lookups compare the
// guarded pointer as well as the key.
using PropertyKey = QPair<const QObject *, QByteArray>;
struct State {
  std::map<const QChar *, Caption> captions;
  QHash<QString, const QChar *> literals;
  QHash<PropertyKey, Binding> bindings;
  QHash<PropertyKey, LiteralProperty> literalProperties;
  std::vector<Callback> callbacks;
  QHash<const QObject *, std::vector<ItemBinding>> items;
  QHash<const QObject *, std::vector<TabBinding>> tabs;
  bool capturing = false;
  bool refreshing = false;
  std::size_t allocations        = 0;
  std::size_t nextCollection     = 128;
  unsigned long long nextBinding = 0;
  std::size_t captureWork        = 0;
};
State &state() {
  static State instance;
  return instance;
}
bool guiThread() {
  return qApp && QThread::currentThread() == qApp->thread();
}

PropertyKey propertyKey(const QObject *object, const char *property) {
  return PropertyKey(object, QByteArray(property));
}

// The binding owning a live object's property, or end() when there is none.
QHash<PropertyKey, Binding>::iterator findBinding(const PropertyKey &key) {
  auto &s = state();
  ++s.captureWork;
  auto found = s.bindings.find(key);
  if (found != s.bindings.end() && found->object != key.first) {
    s.bindings.erase(found);  // Stale: the address now names another object.
    return s.bindings.end();
  }
  return found;
}

void erasePropertyOwners(QObject *object, const char *property) {
  auto &s = state();
  const PropertyKey key = propertyKey(object, property);
  s.bindings.remove(key);
  s.literalProperties.remove(key);
}

template <typename Entries, typename Stale>
void eraseIf(Entries &entries, Stale stale) {
  for (auto it = entries.begin(); it != entries.end();) {
    if (stale(it.value()))
      it = entries.erase(it);
    else
      ++it;
  }
}

template <typename Entry>
bool reassigned(const Entry &entry) {
  return !entry.object || entry.object->property(entry.property.constData())
                                  .toString()
                                  .constData() != entry.assigned.constData();
}

void collectLiteralProperties() {
  eraseIf(state().literalProperties,
          [](const LiteralProperty &entry) { return reassigned(entry); });
}

std::size_t retainedEntries() {
  const auto &s = state();
  return s.captions.size() + s.bindings.size() + s.literalProperties.size() +
         s.callbacks.size() + s.items.size() + s.tabs.size();
}

void collect() {
  auto &s = state();
  // Sweep after as many new identities as were retained after the last
  // sweep: each record() then costs constant amortized time however large the
  // interface is, and retained metadata stays within twice the live amount.
  if (++s.allocations < s.nextCollection) return;
  s.allocations = 0;
  collectLiteralProperties();
  eraseIf(s.bindings,
          [](const Binding &binding) { return reassigned(binding); });
  s.callbacks.erase(std::remove_if(s.callbacks.begin(), s.callbacks.end(),
      [](const Callback &callback) { return callback.owner.isNull(); }),
      s.callbacks.end());
  eraseIf(s.items, [](const std::vector<ItemBinding> &items) {
    return items.empty() || !items.front().combo;
  });
  eraseIf(s.tabs, [](const std::vector<TabBinding> &tabs) {
    return tabs.empty() || !tabs.front().tabs;
  });
  // QString's implicit sharing is also our lifetime token. Sole ownership
  // means no widget, application value or binding can still use this identity.
  // Unlike a text->source dictionary this retains no discarded label history.
  for (auto it = s.captions.begin(); it != s.captions.end();) {
    if (it->second.value.isDetached()) {
      if (!it->second.canonicalKey.isEmpty())
        s.literals.remove(it->second.canonicalKey);
      it = s.captions.erase(it);
    } else ++it;
  }
  s.nextCollection = std::max<std::size_t>(128, retainedEntries());
}

void captureProperty(QObject *object, const char *property) {
  auto &s = state();
  ++s.captureWork;
  if (!object || object->metaObject()->indexOfProperty(property) < 0) return;
  QString value = object->property(property).toString();
  const PropertyKey key = propertyKey(object, property);
  auto existing         = findBinding(key);
  Recipe translate = recipe(value);
  if (!translate) {
    // Most properties hold neither a caption nor a binding: nothing to do.
    // A runtime label that replaced a bound caption is user data or another
    // unbound value now.
    if (existing != s.bindings.end() &&
        existing->assigned.constData() != value.constData())
      s.bindings.erase(existing);
    return;
  }
  ++s.captureWork;
  const auto literal = s.literalProperties.constFind(key);
  if (literal != s.literalProperties.constEnd() && literal->object == object &&
      literal->assigned.constData() == value.constData())
    return;
  if (existing != s.bindings.end() && existing->explicitSource &&
      existing->assigned.constData() == value.constData())
    return;
  Binding binding{object, property, value, std::move(translate)};
  binding.revision = ++s.nextBinding;
  if (existing == s.bindings.end()) {
    binding.order = binding.revision;
    s.bindings.insert(key, std::move(binding));
  } else {
    binding.order = existing->order;
    *existing     = std::move(binding);
  }
}

void captureObject(QObject *object) {
  if (QAction *action = qobject_cast<QAction *>(object)) {
    for (const char *property : {"text", "iconText", "toolTip", "statusTip",
                                 "whatsThis"})
      captureProperty(action, property);
  }
  QWidget *widget = qobject_cast<QWidget *>(object);
  if (!widget) return;
  for (const char *property : {"windowTitle", "toolTip", "statusTip",
                               "whatsThis", "accessibleName",
                               "accessibleDescription", "placeholderText"})
    captureProperty(widget, property);
  if (qobject_cast<QLabel *>(widget) || qobject_cast<QAbstractButton *>(widget))
    captureProperty(widget, "text");
  if (qobject_cast<QGroupBox *>(widget) || qobject_cast<QMenu *>(widget))
    captureProperty(widget, "title");
  if (auto *tabs = qobject_cast<QTabBar *>(widget)) {
    state().tabs.remove(tabs);
    if (tabs->property("_ot_caption_tabs").toBool()) {
      std::vector<TabBinding> captions;
      for (int i = 0; i < tabs->count(); ++i) {
        QString value = tabs->tabText(i);
        Recipe translate = recipe(value);
        if (translate)
          captions.push_back({tabs, i, value, std::move(translate)});
      }
      if (!captions.empty()) state().tabs.insert(tabs, std::move(captions));
    }
  }
  // Editable values (line edits, text edits, editable combos and item views)
  // are deliberately outside automatic caption capture.
  QComboBox *combo = qobject_cast<QComboBox *>(widget);
  if (!combo) return;
  state().items.remove(combo);
  if (combo->isEditable() || !combo->property("_ot_caption_items").toBool())
    return;
  auto *model = qobject_cast<QStandardItemModel *>(combo->model());
  if (!model || model->parent() != combo) return;
  std::vector<ItemBinding> captions;
  for (int i = 0; i < combo->count(); ++i) {
    QString value = combo->itemText(i);
    Recipe translate = recipe(value);
    if (translate)
      captions.push_back(
          {combo, i, combo->itemData(i), value, std::move(translate)});
  }
  if (!captions.empty()) state().items.insert(combo, std::move(captions));
}

// A widget, its actions and the actions it owns. Menus and toolbars share
// actions, so each object is captured once per pass.
void captureWidget(QWidget *widget, QSet<QObject *> &seen) {
  captureObject(widget);
  auto captureAction = [&seen](QAction *action) {
    if (seen.contains(action)) return;
    seen.insert(action);
    captureObject(action);
  };
  for (QAction *action : widget->actions()) captureAction(action);
  for (QAction *action :
       widget->findChildren<QAction *>(QString(), Qt::FindDirectChildrenOnly))
    captureAction(action);
}

// Captures the widgets shown or polished since the last pass. A widget shown
// with its window is captured with the window's subtree, not on its own; the
// rest of the interface is unchanged by these events and is not walked again.
void captureSubtrees(const QList<QWidget *> &roots) {
  auto &s = state();
  if (!guiThread() || s.capturing || s.refreshing) return;
  s.capturing = true;
  QSet<QWidget *> pending;
  for (QWidget *root : roots) pending.insert(root);
  QSet<QObject *> seen;
  for (QWidget *root : roots) {
    bool covered = false;
    for (QWidget *parent = root->parentWidget(); parent && !covered;
         parent          = parent->parentWidget())
      covered = pending.contains(parent);
    if (covered) continue;
    captureWidget(root, seen);
    for (QWidget *child : root->findChildren<QWidget *>())
      captureWidget(child, seen);
  }
  s.capturing = false;
}

class CaptureEvents final : public QObject {
  QHash<QWidget *, QPointer<QWidget>> m_pending;
  bool m_scheduled = false;

public:
  explicit CaptureEvents(QObject *parent) : QObject(parent) {}
  bool eventFilter(QObject *object, QEvent *event) override {
    if ((event->type() != QEvent::Polish && event->type() != QEvent::Show) ||
        !object->isWidgetType())
      return false;
    QWidget *widget = static_cast<QWidget *>(object);
    auto found      = m_pending.find(widget);
    if (found != m_pending.end() && *found) return false;
    m_pending.insert(widget, widget);
    if (!m_scheduled) {
      m_scheduled = true;
      QTimer::singleShot(0, this, [this]() {
        m_scheduled = false;
        QList<QWidget *> roots;
        for (const QPointer<QWidget> &widget : m_pending)
          if (widget) roots.append(widget);
        m_pending.clear();
        captureSubtrees(roots);
      });
    }
    return false;
  }
};
}  // namespace

QString record(const char *context, const char *source,
               const char *disambiguation, int plural,
               const QString &translated) {
  if (!guiThread() || !source) return translated;
  if (translated.isEmpty()) return translated;
  collect();
  QByteArray ctx(context ? context : "");
  QByteArray src(source);
  QByteArray comment(disambiguation ? disambiguation : "");
  QString key = QString::fromUtf8(ctx) + QChar(0) + QString::fromUtf8(src) +
                QChar(0) + QString::fromUtf8(comment) + QChar(0) +
                QString::number(plural) + QChar(0) + translated;
  auto &s = state();
  auto found = s.literals.constFind(key);
  if (found != s.literals.constEnd()) {
    auto caption = s.captions.find(found.value());
    if (caption != s.captions.end()) return caption->second.value;
  }
  QString value = translated;
  value.detach();  // Separate equal translations with different source keys.
  Recipe translate = [ctx, src, comment, plural]() {
    return QCoreApplication::translate(ctx.constData(), src.constData(),
        comment.isEmpty() ? nullptr : comment.constData(), plural);
  };
  s.captions[value.constData()] = {value, std::move(translate), key};
  s.literals.insert(key, value.constData());
  return value;
}

Recipe recipe(const QString &value) {
  if (!guiThread() || value.isEmpty()) return {};
  auto &captions = state().captions;
  auto found = captions.find(value.constData());
  return found == captions.end() ? Recipe() : found->second.translate;
}

QString remember(const QString &text, Recipe translate) {
  if (!guiThread() || !translate || text.isEmpty()) return text;
  if (recipe(text)) return text;
  collect();
  QString value = text;
  value.detach();
  state().captions[value.constData()] = {value, std::move(translate), QString()};
  return value;
}

QString joined(std::initializer_list<QString> parts) {
  std::vector<std::pair<QString, Recipe>> pieces;
  QString value;
  bool translated = false;
  for (const QString &part : parts) {
    Recipe source = recipe(part);
    translated |= bool(source);
    value += part;
    pieces.emplace_back(source ? QString() : part, std::move(source));
  }
  if (!translated) return value;
  return remember(value, [pieces]() {
    QString result;
    for (const auto &piece : pieces)
      result += piece.second ? piece.second() : piece.first;
    return result;
  });
}

void setLiteral(QObject *object, const char *property, const QString &text) {
  if (!object || !property) return;
  const QPointer<QObject> guard(object);
  erasePropertyOwners(object, property);
  object->setProperty(property, text);
  if (guard)
    state().literalProperties.insert(
        propertyKey(object, property),
        {object, property, object->property(property).toString()});
}

void setCaption(QObject *object, const char *property, const QString &text) {
  if (!object || !property) return;
  Recipe translate = recipe(text);
  if (!translate) {
    setLiteral(object, property, text);
    return;
  }
  const QPointer<QObject> guard(object);
  erasePropertyOwners(object, property);
  object->setProperty(property, text);
  if (!guard) return;
  const unsigned long long revision = ++state().nextBinding;
  state().bindings.insert(
      propertyKey(object, property),
      {object, property, object->property(property).toString(),
       std::move(translate), true, revision, revision});
}

void bind(QObject *object, const char *property, const char *context,
          const QString &source, const QString &disambiguation, int plural) {
  if (!object || !property) return;
  QByteArray ctx(context ? context : ""), src = source.toUtf8();
  QByteArray comment = disambiguation.toUtf8();
  Recipe translate = [ctx, src, comment, plural]() {
    return QCoreApplication::translate(ctx.constData(), src.constData(),
        comment.isEmpty() ? nullptr : comment.constData(), plural);
  };
  QPointer<QObject> guard(object);
  QString value = translate();
  erasePropertyOwners(object, property);
  object->setProperty(property, value);
  if (!guard) return;
  const PropertyKey key = propertyKey(object, property);
  auto existing         = findBinding(key);
  Binding binding{object, property, object->property(property).toString(),
                  std::move(translate), true, ++state().nextBinding};
  binding.order =
      existing == state().bindings.end() ? binding.revision : existing->order;
  state().bindings.insert(key, std::move(binding));
}

void onLanguageChanged(QObject *owner, std::function<void()> callback) {
  auto &callbacks = state().callbacks;
  callbacks.erase(std::remove_if(callbacks.begin(), callbacks.end(),
      [](const Callback &entry) { return entry.owner.isNull(); }), callbacks.end());
  if (owner && callback) callbacks.push_back({owner, std::move(callback)});
}

void watchCombo(QComboBox *combo) {
  if (combo) combo->setProperty("_ot_caption_items", true);
}

void watchTabBar(QTabBar *tabs) {
  if (tabs) tabs->setProperty("_ot_caption_tabs", true);
}

std::size_t retainedCaptionCount() { return state().captions.size(); }

std::size_t captureWork() { return state().captureWork; }

void captureAll() {
  if (!guiThread() || state().capturing || state().refreshing) return;
  auto &s = state();
  s.capturing = true;
  collectLiteralProperties();
  s.items.clear();
  s.tabs.clear();
  eraseIf(s.bindings,
          [](const Binding &binding) { return binding.object.isNull(); });
  QSet<QObject *> seen;
  const auto widgets = QApplication::allWidgets();
  for (QWidget *widget : widgets) captureWidget(widget, seen);
  s.capturing = false;
}

void refresh() {
  if (!guiThread() || state().refreshing) return;
  auto &s = state();
  s.refreshing = true;
  std::vector<Binding> bindings;
  bindings.reserve(s.bindings.size());
  for (const Binding &binding : s.bindings) bindings.push_back(binding);
  std::sort(
      bindings.begin(), bindings.end(),
      [](const Binding &a, const Binding &b) { return a.order < b.order; });
  for (const Binding &binding : bindings) {
    if (!binding.object) continue;
    // A presentation observer may have assigned literal data or a new source
    // to a later target while this snapshot was being applied.
    const PropertyKey key(binding.object.data(), binding.property);
    auto live = findBinding(key);
    if (live == s.bindings.end() || live->revision != binding.revision ||
        binding.object->property(binding.property.constData())
                .toString()
                .constData() != binding.assigned.constData())
      continue;
    // Presentation changes must not execute model/scene editing callbacks.
    // QAction::changed is presentation notification used by menus and command
    // browsers. It must remain enabled; none of these setters emits triggered.
    QString value = remember(binding.translate(), binding.translate);
    binding.object->setProperty(binding.property.constData(), value);
    if (!binding.object) continue;
    live = findBinding(key);
    if (live != s.bindings.end() && live->revision == binding.revision)
      live->assigned =
          binding.object->property(binding.property.constData()).toString();
  }
  const auto items = s.items;
  for (const std::vector<ItemBinding> &captions : items)
    for (const ItemBinding &item : captions) {
      if (!item.combo || item.index >= item.combo->count() ||
          item.combo->itemText(item.index).constData() !=
              item.assigned.constData() ||
          item.combo->itemData(item.index) != item.identity)
        continue;
      QSignalBlocker block(item.combo);
      QSignalBlocker modelBlock(item.combo->model());
      item.combo->setItemText(item.index,
                              remember(item.translate(), item.translate));
    }
  const auto tabs = s.tabs;
  for (const std::vector<TabBinding> &captions : tabs)
    for (const TabBinding &tab : captions) {
      if (!tab.tabs || tab.index >= tab.tabs->count() ||
          tab.tabs->tabText(tab.index).constData() != tab.assigned.constData())
        continue;
      const QSignalBlocker blocker(tab.tabs);
      tab.tabs->setTabText(tab.index, remember(tab.translate(), tab.translate));
    }
  s.callbacks.erase(std::remove_if(s.callbacks.begin(), s.callbacks.end(),
      [](const Callback &callback) { return callback.owner.isNull(); }),
      s.callbacks.end());
  const auto callbacks = s.callbacks;
  for (const Callback &callback : callbacks)
    if (callback.owner) callback.invoke();
  s.refreshing = false;
  captureAll();
  std::vector<QPointer<QWidget>> windows;
  for (QWidget *widget : QApplication::topLevelWidgets()) windows.push_back(widget);
  for (const QPointer<QWidget> &widget : windows) {
    if (!widget) continue;
    QEvent event(QEvent::LanguageChange);
    QCoreApplication::sendEvent(widget, &event);
    if (widget) widget->update();
  }
}

void initialize() {
  static QPointer<CaptureEvents> events;
  if (!qApp || events) return;
  events = new CaptureEvents(qApp);
  qApp->installEventFilter(events);
}
}  // namespace UiTranslations
