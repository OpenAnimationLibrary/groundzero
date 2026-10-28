#pragma once

#ifndef SYMBOLSPANE_H
#define SYMBOLSPANE_H

#include <QWidget>
#include <QPixmap>

class QListWidget;
class QListWidgetItem;
class QPushButton;
class TXshChildLevel;

//=============================================================================
// SymbolsPane
//-----------------------------------------------------------------------------
//! The "Symbols" panel lists the symbols of the scene, i.e. its sub-xsheet
//! levels (see symbolcommand.h). A symbol can be inserted many times in the
//! current xsheet (with the Insert button or by dragging it to the cells);
//! all the instances show the same sub-xsheet.

class SymbolsPane final : public QWidget {
  Q_OBJECT

  QListWidget *m_list;
  QPushButton *m_insertButton, *m_editButton, *m_duplicateButton,
      *m_renameButton;
  QPixmap m_emptyIcon;

  TXshChildLevel *getSymbol(QListWidgetItem *item) const;
  QPixmap getIcon(TXshChildLevel *symbol, bool render) const;

public:
  SymbolsPane(QWidget *parent = 0, Qt::WindowFlags flags = Qt::WindowFlags());

  TXshChildLevel *getCurrentSymbol() const;

  //! The current symbol of the Symbols panel used last, if it is visible,
  //! otherwise 0 (see LibraryCmd::getCommandSymbol()).
  static TXshChildLevel *getPanelSymbol();

protected:
  void showEvent(QShowEvent *) override;

protected slots:
  void refresh();
  void onXsheetSwitched();
  void updateIcons();
  void updateButtons();
  void onCurrentSymbolChanged();
  void onContextMenu(const QPoint &pos);

  void insertInstance();
  void editSymbol();
  void duplicateSymbol();
  void renameSymbol();
  void saveToLibrary();
};

#endif  // SYMBOLSPANE_H
