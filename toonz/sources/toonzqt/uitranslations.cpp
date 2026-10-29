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
#include <QPointer>
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
struct State {
  std::map<const QChar *, Caption> captions;
  QHash<QString, const QChar *> literals;
  std::vector<Binding> bindings;
  std::vector<Callback> callbacks;
  std::vector<ItemBinding> items;
  std::vector<TabBinding> tabs;
  bool capturing = false;
  bool refreshing = false;
  unsigned allocations = 0;
};
State &state() {
  static State instance;
  return instance;
}
bool guiThread() {
  return qApp && QThread::currentThread() == qApp->thread();
}

void collect() {
  auto &s = state();
  if (++s.allocations % 128 != 0) return;
  s.bindings.erase(std::remove_if(s.bindings.begin(), s.bindings.end(),
      [](const Binding &binding) {
        return !binding.object ||
            binding.object->property(binding.property.constData()).toString()
                .constData() != binding.assigned.constData();
      }), s.bindings.end());
  s.callbacks.erase(std::remove_if(s.callbacks.begin(), s.callbacks.end(),
      [](const Callback &callback) { return callback.owner.isNull(); }),
      s.callbacks.end());
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
}

void captureProperty(QObject *object, const char *property) {
  if (!object || object->metaObject()->indexOfProperty(property) < 0) return;
  QString value = object->property(property).toString();
  Recipe translate = recipe(value);
  auto &bindings = state().bindings;
  auto existing = std::find_if(bindings.begin(), bindings.end(),
      [&](const Binding &binding) {
        return binding.object == object && binding.property == property;
      });
  if (translate) {
    Binding binding{object, property, value, std::move(translate)};
    if (existing == bindings.end()) bindings.push_back(std::move(binding));
    else *existing = std::move(binding);
  } else if (existing != bindings.end() &&
             existing->assigned.constData() != value.constData()) {
    // A runtime label now represents user data or another unbound value.
    bindings.erase(existing);
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
                               "accessibleDescription"})
    captureProperty(widget, property);
  if (qobject_cast<QLabel *>(widget) || qobject_cast<QAbstractButton *>(widget))
    captureProperty(widget, "text");
  if (qobject_cast<QGroupBox *>(widget) || qobject_cast<QMenu *>(widget))
    captureProperty(widget, "title");
  if (auto *tabs = qobject_cast<QTabBar *>(widget)) {
    if (tabs->property("_ot_caption_tabs").toBool())
      for (int i = 0; i < tabs->count(); ++i) {
        QString value = tabs->tabText(i);
        Recipe translate = recipe(value);
        if (translate)
          state().tabs.push_back({tabs, i, value, std::move(translate)});
      }
  }
  // Editable values (line edits, text edits, editable combos and item views)
  // are deliberately outside automatic caption capture.
  QComboBox *combo = qobject_cast<QComboBox *>(widget);
  if (!combo || combo->isEditable() ||
      !combo->property("_ot_caption_items").toBool()) return;
  auto *model = qobject_cast<QStandardItemModel *>(combo->model());
  if (!model || model->parent() != combo) return;
  for (int i = 0; i < combo->count(); ++i) {
    QString value = combo->itemText(i);
    Recipe translate = recipe(value);
    if (translate)
      state().items.push_back({combo, i, combo->itemData(i), value,
                               std::move(translate)});
  }
}

class CaptureEvents final : public QObject {
  bool m_pending = false;
public:
  explicit CaptureEvents(QObject *parent) : QObject(parent) {}
  bool eventFilter(QObject *, QEvent *event) override {
    if (!m_pending && (event->type() == QEvent::Polish ||
                       event->type() == QEvent::Show)) {
      m_pending = true;
      QTimer::singleShot(0, this, [this]() {
        m_pending = false;
        captureAll();
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
  object->setProperty(property, value);
  if (!guard) return;
  auto &bindings = state().bindings;
  auto existing = std::find_if(bindings.begin(), bindings.end(),
      [&](const Binding &binding) {
        return binding.object == object && binding.property == property;
      });
  Binding binding{object, property, object->property(property).toString(),
                  std::move(translate)};
  if (existing == bindings.end()) bindings.push_back(std::move(binding));
  else *existing = std::move(binding);
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

void captureAll() {
  if (!guiThread() || state().capturing || state().refreshing) return;
  auto &s = state();
  s.capturing = true;
  s.items.clear();
  s.tabs.clear();
  s.bindings.erase(std::remove_if(s.bindings.begin(), s.bindings.end(),
      [](const Binding &binding) { return binding.object.isNull(); }),
      s.bindings.end());
  const auto widgets = QApplication::allWidgets();
  for (QWidget *widget : widgets) {
    captureObject(widget);
    for (QAction *action : widget->actions()) captureObject(action);
    for (QAction *action : widget->findChildren<QAction *>(QString(),
                                   Qt::FindDirectChildrenOnly))
      captureObject(action);
  }
  s.capturing = false;
}

void refresh() {
  if (!guiThread() || state().refreshing) return;
  auto &s = state();
  s.refreshing = true;
  const auto bindings = s.bindings;
  for (const Binding &binding : bindings) {
    if (!binding.object ||
        binding.object->property(binding.property.constData()).toString()
            .constData() != binding.assigned.constData())
      continue;
    // Presentation changes must not execute model/scene editing callbacks.
    // QAction::changed is presentation notification used by menus and command
    // browsers. It must remain enabled; none of these setters emits triggered.
    QString value = remember(binding.translate(), binding.translate);
    binding.object->setProperty(binding.property.constData(), value);
    for (Binding &live : s.bindings)
      if (live.object && live.object == binding.object &&
          live.property == binding.property) {
        live.assigned = live.object->property(live.property.constData()).toString();
        break;
      }
  }
  const auto items = s.items;
  for (const ItemBinding &item : items) {
    if (!item.combo || item.index >= item.combo->count() ||
        item.combo->itemText(item.index).constData() != item.assigned.constData() ||
        item.combo->itemData(item.index) != item.identity)
      continue;
    QSignalBlocker block(item.combo);
    QSignalBlocker modelBlock(item.combo->model());
    item.combo->setItemText(item.index,
                           remember(item.translate(), item.translate));
  }
  const auto tabs = s.tabs;
  for (const TabBinding &tab : tabs) {
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
