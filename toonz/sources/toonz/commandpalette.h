#pragma once

#include <QAction>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

// The native adapter supplies existing actions, not copies of their handlers.
// QPointer also makes results safe when an action disappears while searching.
struct CommandPaletteEntry {
  QString id;
  QString category;
  QPointer<QAction> action;
  bool available = true;
};

class CommandPalette final : public QWidget {
  Q_OBJECT

public:
  using Source = std::function<QVector<CommandPaletteEntry>()>;
  using Invoker = std::function<bool(const CommandPaletteEntry &)>;

  explicit CommandPalette(QWidget *parent = nullptr);
  void setSource(Source source);
  void setInvoker(Invoker invoker);
  void beginSearch();
  void refreshCommands();

signals:
  void cancelled();
  void commandInvoked(const QString &id);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;
  void showEvent(QShowEvent *event) override;
  void hideEvent(QHideEvent *event) override;
  void changeEvent(QEvent *event) override;

private:
  void retranslate();
  void rebuildResults(bool resetSelection);
  void moveCurrent(int direction);
  void invokeCurrent();
  void cancelSearch();

  Source m_source;
  Invoker m_invoker;
  QVector<CommandPaletteEntry> m_entries;
  QLineEdit *m_search;
  QTreeWidget *m_results;
  QLabel *m_hint;
  QTimer *m_refreshTimer;
  QStringList m_signature;
  bool m_invoking = false;
};

// Application adapter; tests can exercise the widget without native globals.
CommandPalette *createNativeCommandPalette(QWidget *parent);
