#pragma once

#ifndef CAST_VIEWER_INCLUDED
#define CAST_VIEWER_INCLUDED

#include <memory>

#include <QSplitter>
#include <QTreeWidget>
#include <QJsonObject>

#include "dvitemview.h"
#include "tfilepath.h"
#include "toonz/txshlevel.h"
#include <set>
#include <map>
#include <vector>

class QLabel;
class TLevelSet;
class CastItems;
class QLineEdit;
class QComboBox;
class QTimer;
class QPushButton;
class CastInspector;

//-----------------------------------------------------------------------------

class CastTreeViewer final : public QTreeWidget, public TSelection {
  Q_OBJECT
  QTreeWidgetItem *m_dropTargetItem;
  TFilePath m_dropFilePath;

  void populateFolder(QTreeWidgetItem *folder);

public:
  CastTreeViewer(QWidget *parent = 0);
  QSize sizeHint() const override;

  TFilePath getCurrentFolder() const;
  static TLevelSet *getLevelSet();

  // da TSelection
  bool isEmpty() const override { return false; }
  void selectNone() override {}
  void enableCommands() override;

protected:
  void paintEvent(QPaintEvent *) override;
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dragMoveEvent(QDragMoveEvent *event) override;
  void dropEvent(QDropEvent *event) override;
  void dragLeaveEvent(QDragLeaveEvent *event) override;
  void resizeEvent(QResizeEvent *) override;

public slots:
  void onItemChanged(QTreeWidgetItem *item, int column);
  void onFolderChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous);
  void onSceneNameChanged();
  void onCastFolderAdded(const TFilePath &path);

  void rebuildCastTree();
  void folderUp();
  void newFolder();
  void deleteFolder();

  void resizeToConts(void);

signals:
  void itemMovedToFolder();
};

//-----------------------------------------------------------------------------

//! La classe si occupa della visualizzazione dello scene cast.
/*!E' figlia di \b QSplitter e contiene un albero che visualizza i folder \b
   m_treeView
   e un widget che consente di visualizzare i file \b m_sceneCastView.
   I suoi widget sono settati tramite un modello del tipo \b SceneCastModel.*/
class CastBrowser final : public QSplitter, public DvItemListModel {
  Q_OBJECT

  CastTreeViewer *m_treeViewer;
  QLabel *m_folderName;
  DvItemViewer *m_itemViewer;

  std::unique_ptr<CastItems> m_castItems;
  QLineEdit *m_search;
  QComboBox *m_scope, *m_type, *m_status;
  QComboBox *m_collection;
  QPushButton *m_collectionActions;
  CastInspector *m_inspector = nullptr;
  QLabel *m_resultCount;
  QTimer *m_filterTimer;
  QPushButton *m_actions;
  std::vector<TXshLevelP> m_rememberedSelection;
  std::vector<TXshLevelP> m_displayedLevels;
  std::map<TXshLevel *, QString> m_assetStatus;
  std::map<TXshLevel *, QChar> m_statusBadge;
  QJsonObject m_refreshMeasurements;
  bool m_rebuilding = false;
  void rememberSelection();
  void restoreSelection();
  std::vector<TXshLevel *> selectedLevels() const;
  void showActions();
  void showCollectionActions();
  void refreshCollections();

public:
  CastBrowser(QWidget *parent = 0, Qt::WindowFlags flags = Qt::WindowFlags());
  ~CastBrowser();

  CastItems const &getCastItems() const { return *m_castItems; }

  // Cumulative completed refresh work across native Cast instances. CI records
  // differences around real edit signals; these are not paint-latency timings.
  static QJsonObject refreshMeasurements();
  QJsonObject instanceRefreshMeasurements() const { return m_refreshMeasurements; }

  void sortByDataModel(DataType dataType, bool isDiscendent) override;

  int getItemCount() const override;
  void refreshData() override;
  QVariant getItemData(int index, DataType dataType,
                       bool isSelected = false) override;
  QMenu *getContextMenu(QWidget *parent, int index) override;
  void startDragDrop() override;
  bool acceptDrop(const QMimeData *data) const override;
  bool drop(const QMimeData *data) override;

  void expose();
  void edit();
  void showFolderContents();
  void viewFile();
  void viewFileInfo();
  void showInspector(const std::vector<TXshLevel *> &levels);

protected:
  void showEvent(QShowEvent *event) override;
  bool dropMimeData(QTreeWidgetItem *parent, int index, const QMimeData *data,
                    Qt::DropAction action);
  Qt::DropActions supportedDropActions() const;

  // void showEvent(QShowEvent*);
  // void hideEvent(QHideEvent*);

protected slots:
  void folderChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous);
  void refresh();
};

//-----------------------------------------------------------------------------

#endif
