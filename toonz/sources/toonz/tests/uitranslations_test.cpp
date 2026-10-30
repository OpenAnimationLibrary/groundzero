#include "toonzqt/uitranslations.h"
#include "toonzqt/filefield.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QStandardItemModel>
#include <QTabBar>
#include <QTextCursor>
#include <QTextEdit>
#include <QTranslator>
#include <iostream>

namespace {
class Catalog final : public QTranslator {
public:
  int language = 0;
  bool isEmpty() const override { return false; }
  QString translate(const char *context, const char *source,
                    const char *comment = nullptr, int n = -1) const override {
    QString value = QString::fromUtf8(source);
    if (language && QByteArray(source) == "Open")
      value = QString("%1:%2").arg(QString::fromUtf8(context)).arg(language);
    if (language && QByteArray(source) == "Shot %1")
      value = QString("Scene%1: %2").arg(language).arg("%1");
    return UiTranslations::record(context, source, comment, n, value);
  }
};
class DeletePeerWindow final : public QWidget {
public:
  QPointer<QWidget> peer;
protected:
  void changeEvent(QEvent *event) override {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) delete peer.data();
  }
};
bool check(bool condition, const char *message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}
QString translated(const char *source, const char *context = "Fixture") {
  return QCoreApplication::translate(context, source);
}
}

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  Catalog catalog;
  app.installTranslator(&catalog);
  UiTranslations::initialize();
  QWidget window;
  QLabel first(translated("Open", "First"), &window);
  QLabel second(translated("Open", "Second"), &window);
  QLabel userText(QString::fromUtf8("Open"), &window);
  QLineEdit editable(translated("Open"), &window);
  editable.setPlaceholderText(translated("Open", "Placeholder"));
  editable.setSelection(1, 2);
  const int editCursor = editable.cursorPosition();
  int edits = 0;
  QObject::connect(&editable, &QLineEdit::textChanged, [&]() { ++edits; });
  QLineEdit literalPlaceholder(&window);
  literalPlaceholder.setPlaceholderText(QStringLiteral("Open"));
  QLineEdit storyboardCaption(&window);
  storyboardCaption.setPlaceholderText(QStringLiteral("panelCaption"));
  QTextEdit notes(&window);
  notes.setPlainText(QStringLiteral("Unsaved notes"));
  notes.setPlaceholderText(translated("Open", "NotesPlaceholder"));
  QTextCursor notesCursor = notes.textCursor();
  notesCursor.setPosition(3);
  notesCursor.setPosition(7, QTextCursor::KeepAnchor);
  notes.setTextCursor(notesCursor);
  int noteEdits = 0;
  QObject::connect(&notes, &QTextEdit::textChanged, [&]() { ++noteEdits; });
  QLabel replaced(translated("Open"), &window);
  QLabel equalReplacement(translated("Open"), &window);
  QLabel equalSourceChange(translated("Open", "First"), &window);
  QAction action(translated("Open"), &window);
  action.setCheckable(true);
  action.setChecked(true);
  action.setShortcut(QKeySequence("Ctrl+J"));
  int triggered = 0;
  QObject::connect(&action, &QAction::triggered, [&]() { ++triggered; });
  window.addAction(&action);
  QLabel formatted(&window);
  QLabel joined(UiTranslations::joined({translated("Open", "First"), " / ",
      translated("Open", "Second"), " / ", QStringLiteral("Open")}), &window);
  const QString descriptionToken = translated("Open");
  DVGui::FileField description(&window, descriptionToken, true, true);
  DVGui::FileField realPath(&window, descriptionToken, true, true);
  realPath.setPath(QStringLiteral("Open"));
  int pathChanges = 0;
  QObject::connect(&description, &DVGui::FileField::pathChanged,
                   [&]() { ++pathChanges; });
  {
    QByteArray argument("kept");
    formatted.setText(UiTranslations::text(translated("Shot %1"))
                          .arg(argument.data()));
    argument.fill('x');
  }
  QComboBox captions(&window);
  captions.addItem(translated("Open"), 12);
  captions.addItem(QStringLiteral("Other"), 13);
  captions.setCurrentIndex(0);
  UiTranslations::watchCombo(&captions);
  int changed = 0;
  QObject::connect(&captions, QOverload<int>::of(&QComboBox::currentIndexChanged),
                   [&]() { ++changed; });
  int modelChanges = 0;
  QObject::connect(captions.model(), &QAbstractItemModel::dataChanged,
                   [&]() { ++modelChanges; });

  // Provenance alone is not permission to mutate a data-backed model.
  QStandardItemModel model;
  model.appendRow(new QStandardItem(translated("Open")));
  QComboBox dataSelector(&window);
  dataSelector.setModel(&model);
  UiTranslations::watchCombo(&dataSelector);
  int dataWrites = 0;
  QObject::connect(&model, &QAbstractItemModel::dataChanged,
                   [&]() { ++dataWrites; });
  QTabBar tabs(&window), documentTabs(&window);
  tabs.addTab(translated("Open"));
  tabs.addTab(QStringLiteral("Other"));
  tabs.setCurrentIndex(1);
  UiTranslations::watchTabBar(&tabs);
  documentTabs.addTab(translated("Open"));
  int tabChanges = 0;
  QObject::connect(&tabs, &QTabBar::currentChanged, [&]() { ++tabChanges; });
  UiTranslations::captureAll();
  replaced.setText(QStringLiteral("User caption"));
  // QLabel discards equal-text assignments, retaining the old QString buffer.
  // Ownership must follow the explicit producer, not that retained storage.
  UiTranslations::setLiteral(&equalReplacement, "text", QStringLiteral("Open"));
  UiTranslations::setCaption(&equalSourceChange, "text", translated("Open", "Second"));
  bool ok = true;
  for (int language : {1, 2, 0, 2, 0}) {
    UiTranslations::captureAll();
    catalog.language = language;
    UiTranslations::refresh();
    ok &= check(first.text() == (language ? QString("First:%1").arg(language)
                                         : QString("Open")), "First context lost");
    ok &= check(second.text() == (language ? QString("Second:%1").arg(language)
                                          : QString("Open")), "Second context lost");
    ok &= check(formatted.text() == (language
        ? QString("Scene%1: kept").arg(language) : QString("Shot kept")),
        "Formatted caption or owned argument failed on repeated switch");
    ok &= check(joined.text() == (language
        ? QString("First:%1 / Second:%1 / Open").arg(language)
        : QString("Open / Open / Open")), "Composed caption changed user text or lost source");
    ok &= check(description.getField()->text() == (language
        ? QString("Fixture:%1").arg(language) : QString("Open")) &&
        description.getPath() == descriptionToken && pathChanges == 0 &&
        realPath.getField()->text() == "Open" && realPath.getPath() == "Open",
        "Description changed a semantic token, path or user input");
    ok &= check(userText.text() == "Open" && editable.text() == "Open" &&
                replaced.text() == "User caption" && equalReplacement.text() == "Open",
                "User text changed, including an equal-text replacement");
    ok &= check(editable.placeholderText() == (language
        ? QString("Placeholder:%1").arg(language) : QString("Open")) &&
        editable.cursorPosition() == editCursor && editable.selectionStart() == 1 &&
        editable.selectedText() == "pe" && edits == 0 &&
        literalPlaceholder.placeholderText() == "Open" &&
        storyboardCaption.placeholderText() == "panelCaption",
        "Placeholder translation altered a value, selection or literal placeholder");
    ok &= check(notes.placeholderText() == (language
        ? QString("NotesPlaceholder:%1").arg(language) : QString("Open")) &&
        notes.toPlainText() == "Unsaved notes" && notes.textCursor().position() == 7 &&
        notes.textCursor().anchor() == 3 && noteEdits == 0,
        "Text-edit placeholder translation altered document text or cursor");
    ok &= check(equalSourceChange.text() == (language
        ? QString("Second:%1").arg(language) : QString("Open")),
        "Equal caption replacement retained the wrong source context");
    ok &= check(action.isChecked() && action.shortcut() == QKeySequence("Ctrl+J")
                && triggered == 0, "Action state/dispatch changed");
    ok &= check(captions.currentData().toInt() == 12 && changed == 0 &&
                modelChanges == 0, "Caption update edited choice/model");
    ok &= check(model.item(0)->text() == "Open" && dataWrites == 0,
                "Data-backed selector was mutated");
    ok &= check(tabs.tabText(0) == (language
        ? QString("Fixture:%1").arg(language) : QString("Open")) &&
        tabs.currentIndex() == 1 && tabChanges == 0 &&
        documentTabs.tabText(0) == "Open", "Caption tabs changed tab state or document names");
  }
  UiTranslations::setCaption(&equalReplacement, "text", translated("Open"));
  realPath.setPath(QStringLiteral("kept.png"));
  UiTranslations::captureAll();
  catalog.language = 1;
  UiTranslations::refresh();
  ok &= check(equalReplacement.text() == "Fixture:1",
              "Equal-text native caption did not regain translation ownership");
  QAction controller(&window);
  QLabel duringRefresh(&window);
  UiTranslations::setCaption(&controller, "text", translated("Open"));
  UiTranslations::setCaption(&duringRefresh, "text", translated("Open"));
  const QString beforeOwnershipChange = duringRefresh.text();
  QObject::connect(&controller, &QAction::changed, &duringRefresh, [&]() {
    UiTranslations::setLiteral(&duringRefresh, "text", duringRefresh.text());
  });
  catalog.language = 2;
  UiTranslations::refresh();
  ok &= check(duringRefresh.text() == beforeOwnershipChange,
              "A stale refresh snapshot overrode newly assigned literal data");
  ok &= check(realPath.getPath() == "kept.png" &&
              realPath.getField()->text() == "kept.png", "Selected path changed");
  realPath.setPath(descriptionToken);
  ok &= check(realPath.getPath() == descriptionToken &&
              realPath.getField()->text() == "Fixture:2",
              "Restoring description did not preserve its semantic token");
  UiTranslations::captureAll();
  catalog.language = 1;
  UiTranslations::refresh();
  ok &= check(realPath.getPath() == descriptionToken &&
              realPath.getField()->text() == "Fixture:1",
              "Restored description stopped translating");
  const std::size_t before = UiTranslations::retainedCaptionCount();
  for (int i = 0; i < 20000; ++i) {
    QString discarded = UiTranslations::text(translated("Shot %1")).arg(i);
    (void)discarded;
  }
  ok &= check(UiTranslations::retainedCaptionCount() < before + 256,
              "Discarded formatted captions retain unbounded metadata");
  bool deadCallback = false;
  QObject *owner = new QObject;
  UiTranslations::onLanguageChanged(owner, [&]() { deadCallback = true; });
  delete owner;
  UiTranslations::captureAll();
  UiTranslations::refresh();
  ok &= check(!deadCallback, "Destroyed translation owner was called");
  // Presentation observers may synchronously destroy an action being updated.
  QPointer<QAction> ephemeral = new QAction(translated("Open"), &window);
  window.addAction(ephemeral);
  UiTranslations::captureAll();
  QObject::connect(ephemeral, &QAction::changed, [&]() { delete ephemeral.data(); });
  catalog.language = 2;
  UiTranslations::refresh();
  ok &= check(ephemeral.isNull(), "Deleted presentation target was retained");
  DeletePeerWindow deletePeer;
  deletePeer.peer = new QWidget;
  UiTranslations::refresh();
  ok &= check(deletePeer.peer.isNull(), "LanguageChange did not dispose its peer safely");

  // Capture cost follows the widgets that changed, not the whole interface.
  // captureWork() counts the properties and retained entries examined, so
  // these bounds do not depend on the speed of the machine.
  QWidget large;
  for (int i = 0; i < 1500; ++i) {
    auto *label = new QLabel(translated("Open", "Large"), &large);
    label->setToolTip(translated("Open", "LargeTip"));
  }
  UiTranslations::captureAll();
  std::size_t work = UiTranslations::captureWork();
  UiTranslations::captureAll();
  work = UiTranslations::captureWork() - work;
  ok &= check(work <= 32 * std::size_t(QApplication::allWidgets().size()),
              "Caption capture compared each property with every binding");
  large.show();
  QCoreApplication::processEvents();
  QWidget dialog;
  QLabel shown(translated("Open", "Shown"), &dialog);
  work = UiTranslations::captureWork();
  dialog.show();
  QCoreApplication::processEvents();
  work = UiTranslations::captureWork() - work;
  ok &= check(work > 0 && work <= 100,
              "Showing one window captured every widget of the interface");
  catalog.language = 1;
  UiTranslations::refresh();
  ok &= check(shown.text() == "Shown:1" &&
                  large.findChild<QLabel *>()->text() == "Large:1",
              "A shown window's captions were not captured");
  return ok ? 0 : 1;
}
