#include "commandpalette.h"

#include <QActionGroup>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QTreeWidget>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void key(QWidget *widget, int code, const QString &text = QString(),
         Qt::KeyboardModifiers modifiers = Qt::NoModifier,
         bool repeat = false) {
  QKeyEvent press(QEvent::KeyPress, code, modifiers, text, repeat);
  QApplication::sendEvent(widget, &press);
  QKeyEvent release(QEvent::KeyRelease, code, modifiers, text, repeat);
  QApplication::sendEvent(widget, &release);
}

QString currentId(QTreeWidget *tree) {
  return tree->currentItem()
             ? tree->currentItem()->data(0, Qt::UserRole).toString()
             : QString();
}

void activateItem(QTreeWidget *tree) {
  QMetaObject::invokeMethod(tree, "itemActivated", Qt::DirectConnection,
                           Q_ARG(QTreeWidgetItem *, tree->currentItem()),
                           Q_ARG(int, 0));
}
}  // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  QApplication app(argc, argv);
  try {
    CommandPalette palette;
    palette.resize(700, 420);
    auto *search = palette.findChild<QLineEdit *>("commandPaletteSearch");
    auto *results = palette.findChild<QTreeWidget *>("commandPaletteResults");
    require(search && results, "palette controls missing");

    QAction library(QStringLiteral("&Library"), &palette);
    QAction edit(QStringLiteral("Convert Selection to Symbol..."), &palette);
    QAction disabled(QStringLiteral("Unavailable Edit"), &palette);
    QAction hidden(QStringLiteral("Hidden Command"), &palette);
    QAction ampersand(QStringLiteral("Save && Export"), &palette);
    QAction duplicateLabel(QStringLiteral("&Library"), &palette);
    QAction toggle(QStringLiteral("Toggle View"), &palette);
    QAction menuAction(QStringLiteral("Submenu"), &palette);
    QMenu menu;
    menuAction.setMenu(&menu);
    QAction separator(&palette);
    separator.setSeparator(true);
    disabled.setEnabled(false);
    hidden.setVisible(false);
    toggle.setCheckable(true);
    toggle.setChecked(true);
    library.setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    bool editable = true;
    QVector<CommandPaletteEntry> entries = {
        {QStringLiteral("MI_OpenRigLibrary"), QStringLiteral("Windows"), &library},
        {QStringLiteral("MI_LibraryAlias"), QStringLiteral("Windows"), &library},
        {QStringLiteral("MI_OtherLibrary"), QStringLiteral("Other"), &duplicateLabel},
        {QStringLiteral("MI_ConvertSelectionToSymbol"), QStringLiteral("Edit"), &edit},
        {QStringLiteral("MI_Disabled"), QStringLiteral("Edit"), &disabled},
        {QStringLiteral("MI_Hidden"), QStringLiteral("Other"), &hidden},
        {QStringLiteral("MI_Ampersand"), QStringLiteral("File"), &ampersand},
        {QStringLiteral("MI_Toggle"), QStringLiteral("View"), &toggle},
        {QStringLiteral("MI_Submenu"), QStringLiteral("Other"), &menuAction},
        {QStringLiteral("MI_Separator"), QStringLiteral("Other"), &separator}};
    palette.setSource([&]() {
      QVector<CommandPaletteEntry> current = entries;
      for (CommandPaletteEntry &entry : current)
        if (entry.action == &edit) entry.available = editable;
      return current;
    });
    palette.show();
    palette.beginSearch();
    app.processEvents();
    require(results->topLevelItemCount() == 6,
            "hidden, duplicate, submenu or separator leaked into results");

    search->setText(QStringLiteral("library"));
    require(results->topLevelItemCount() == 2,
            "same labels from different actions were incorrectly deduplicated");
    require(results->topLevelItem(0)->text(0) == QStringLiteral("Library"),
            "menu mnemonic was displayed as command text");
    search->setText(QStringLiteral("MI_OpenRigLibrary"));
    require(results->topLevelItemCount() == 1 &&
                currentId(results) == QStringLiteral("MI_OpenRigLibrary"),
            "native renamed command lost its stable identity");
    require(results->currentItem()->text(1) == library.shortcut().toString(QKeySequence::NativeText),
            "current shortcut was not displayed");
    library.setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+L")));
    palette.refreshCommands();
    require(results->currentItem()->text(1) == library.shortcut().toString(QKeySequence::NativeText),
            "changed shortcut was not reflected");

    search->setText(QStringLiteral("save & export"));
    require(results->topLevelItemCount() == 1 &&
                results->currentItem()->text(0) == QStringLiteral("Save & Export"),
            "escaped ampersand was lost during filtering");
    search->setText(QStringLiteral("not a real command"));
    require(results->topLevelItemCount() == 0, "no-match query retained results");

    int edits = 0;
    QObject::connect(&edit, &QAction::triggered, [&]() { ++edits; });
    search->setText(QStringLiteral("convert selection"));
    editable = false;  // Target changed, without changing QAction::enabled.
    key(search, Qt::Key_Return);
    require(edits == 0, "stale target eligibility permitted execution");
    editable = true;
    palette.refreshCommands();
    edit.setEnabled(false);  // QAction changed after a result was selected.
    key(search, Qt::Key_Return);
    require(edits == 0, "disabled action executed from a stale result");
    edit.setEnabled(true);
    palette.refreshCommands();
    key(search, Qt::Key_Return);
    require(edits == 1, "Enter did not activate exactly once");
    require(search->text().isEmpty(), "successful activation did not clear query");

    search->setText(QStringLiteral("convert selection"));
    key(search, Qt::Key_Return, QString(), Qt::NoModifier, true);
    require(edits == 1, "auto-repeat executed an edit again");
    activateItem(results);
    require(edits == 2, "item activation did not dispatch once");

    search->setText(QStringLiteral("toggle view"));
    key(search, Qt::Key_Return);
    require(!toggle.isChecked(), "checkable action could not toggle off");
    search->setText(QStringLiteral("toggle view"));
    key(search, Qt::Key_Return);
    require(toggle.isChecked(), "checkable action could not toggle on");

    QActionGroup tools(&palette);
    tools.setExclusive(true);
    QAction first(QStringLiteral("First Tool"), &tools);
    QAction second(QStringLiteral("Second Tool"), &tools);
    first.setCheckable(true);
    second.setCheckable(true);
    first.setChecked(true);
    entries.push_back({QStringLiteral("T_First"), QStringLiteral("Tools"), &first});
    entries.push_back({QStringLiteral("T_Second"), QStringLiteral("Tools"), &second});
    palette.refreshCommands();
    search->setText(QStringLiteral("second tool"));
    key(search, Qt::Key_Return);
    require(second.isChecked() && !first.isChecked(),
            "native exclusive action group was bypassed");

    auto *transient = new QAction(QStringLiteral("Transient Command"), &palette);
    entries.push_back({QStringLiteral("MI_Transient"), QStringLiteral("Other"), transient});
    palette.refreshCommands();
    search->setText(QStringLiteral("transient"));
    require(results->topLevelItemCount() == 1, "new action was not discovered");
    delete transient;
    key(search, Qt::Key_Return);
    require(results->topLevelItemCount() == 0, "destroyed action remained executable");

    library.setText(QString::fromUtf8("Biblioth\xC3\xA8que"));
    palette.refreshCommands();
    search->setText(QStringLiteral("biblioth"));
    require(currentId(results) == QStringLiteral("MI_OpenRigLibrary"),
            "translated label lost the native action");

    search->clear();
    palette.beginSearch();
    QKeyEvent overrideEvent(QEvent::ShortcutOverride, Qt::Key_B, Qt::NoModifier,
                           QStringLiteral("b"));
    overrideEvent.ignore();
    QApplication::sendEvent(search, &overrideEvent);
    require(overrideEvent.isAccepted(), "search did not reserve a drawing shortcut");
    key(search, Qt::Key_B, QStringLiteral("b"));
    require(search->text() == QStringLiteral("b"), "typing did not edit the search field");
    key(search, Qt::Key_A, QStringLiteral("a"), Qt::ControlModifier);
    key(search, Qt::Key_X, QStringLiteral("x"));
    require(search->text() == QStringLiteral("x"), "text selection shortcut was intercepted");

    search->clear();
    const QString initial = currentId(results);
    key(search, Qt::Key_Down);
    require(!currentId(results).isEmpty() && currentId(results) != initial,
            "keyboard navigation did not move between results");
    int cancelled = 0;
    QObject::connect(&palette, &CommandPalette::cancelled, [&]() { ++cancelled; });
    key(search, Qt::Key_Escape);
    require(cancelled == 1 && search->text().isEmpty(), "Escape did not cancel cleanly");

    // Dispatch can synchronously reenter the UI through a native modal dialog.
    palette.setInvoker([&](const CommandPaletteEntry &entry) {
      activateItem(results);
      entry.action->trigger();
      return true;
    });
    search->setText(QStringLiteral("convert selection"));
    key(search, Qt::Key_Return);
    require(edits == 3, "reentrant activation executed a command twice");

    // Room/panel commands may destroy the calling widget during dispatch.
    auto *closing = new CommandPalette;
    QPointer<CommandPalette> closingGuard(closing);
    QAction closeAction(QStringLiteral("Close Owner"), &app);
    closing->setSource([&]() {
      return QVector<CommandPaletteEntry>{{QStringLiteral("MI_CloseOwner"),
                                           QStringLiteral("Windows"), &closeAction}};
    });
    QObject::connect(&closeAction, &QAction::triggered, [&]() { delete closing; });
    auto *closingSearch = closing->findChild<QLineEdit *>("commandPaletteSearch");
    QKeyEvent closePress(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(closingSearch, &closePress);
    require(!closingGuard, "owner destruction during command was not handled");

    std::cout << "Command palette behavior passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
