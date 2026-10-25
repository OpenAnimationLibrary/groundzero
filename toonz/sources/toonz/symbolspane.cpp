#include "symbolspane.h"

// Tnz6 includes
#include "tapp.h"
#include "menubarcommandids.h"
#include "castselection.h"
#include "symbolcommand.h"

// TnzQt includes
#include "toonzqt/menubarcommand.h"
#include "toonzqt/icongenerator.h"
#include "toonzqt/dvdialog.h"
#include "toonzqt/gutil.h"

// TnzLib includes
#include "toonz/toonzscene.h"
#include "toonz/levelset.h"
#include "toonz/txsheet.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txsheethandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/preferences.h"

// Qt includes
#include <QListWidget>
#include <QPushButton>
#include <QToolButton>
#include <QBoxLayout>
#include <QMenu>
#include <QDrag>
#include <QAction>
#include <QIcon>

// STD includes
#include <set>
#include <vector>

//*****************************************************************************
//    Local namespace
//*****************************************************************************

namespace {

const QSize SymbolIconSize(80, 60);

//-----------------------------------------------------------------------------

TXshChildLevel *findSymbol(const QString &name) {
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  if (!scene || name.isEmpty()) return 0;
  TXshLevel *level = scene->getLevelSet()->getLevel(name.toStdWString());
  return level ? level->getChildLevel() : 0;
}

//=============================================================================
// SymbolCastItem
//-----------------------------------------------------------------------------
// Dragging a symbol to the xsheet cells inserts an instance of it there (see
// DataDragTool).

class SymbolCastItem final : public CastItem {
  TXshChildLevelP m_symbol;

public:
  SymbolCastItem(TXshChildLevel *symbol) : m_symbol(symbol) {}

  TXshChildLevel *getChildLevel() const override {
    return m_symbol.getPointer();
  }
  QString getName() const override {
    return QString::fromStdWString(m_symbol->getName());
  }
  QString getToolTip() const override { return getName(); }
  int getFrameCount() const override { return m_symbol->getFrameCount(); }
  QPixmap getPixmap(bool isSelected) const override { return QPixmap(); }
  CastItem *clone() const override {
    return new SymbolCastItem(m_symbol.getPointer());
  }
  bool exists() const override { return true; }
};

//=============================================================================
// SymbolList
//-----------------------------------------------------------------------------

class SymbolList final : public QListWidget {
public:
  SymbolList(QWidget *parent) : QListWidget(parent) {}

protected:
  void startDrag(Qt::DropActions supportedActions) override {
    QListWidgetItem *item = currentItem();
    TXshChildLevel *symbol =
        item ? findSymbol(item->data(Qt::UserRole).toString()) : 0;
    if (!symbol) return;

    CastItems *data = new CastItems();
    data->addItem(new SymbolCastItem(symbol));

    QDrag *drag    = new QDrag(this);
    QPixmap pixmap = item->icon().pixmap(iconSize());
    if (!pixmap.isNull()) drag->setPixmap(pixmap);
    drag->setMimeData(data);
    drag->exec(Qt::CopyAction);
  }
};

}  // namespace

//=============================================================================
// SymbolsPane
//-----------------------------------------------------------------------------

SymbolsPane::SymbolsPane(QWidget *parent, Qt::WindowFlags flags)
    : QWidget(parent, flags) {
  setObjectName("SymbolsPane");

  m_list = new SymbolList(this);
  m_list->setIconSize(SymbolIconSize);
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);
  m_list->setDragEnabled(true);
  m_list->setDragDropMode(QAbstractItemView::DragOnly);
  m_list->setContextMenuPolicy(Qt::CustomContextMenu);

  m_emptyIcon = QPixmap(SymbolIconSize);
  m_emptyIcon.fill(Qt::transparent);

  m_insertButton = new QPushButton(tr("Insert"), this);
  m_insertButton->setToolTip(
      tr("Insert an instance of the symbol in a new column at the current "
         "frame. Symbols can also be dragged to the xsheet cells."));
  m_editButton = new QPushButton(tr("Edit"), this);
  m_editButton->setToolTip(
      tr("Open the symbol from one of its instances in the current xsheet"));
  m_duplicateButton = new QPushButton(tr("Duplicate"), this);
  m_duplicateButton->setToolTip(
      tr("Make a separate copy of the symbol, which can be edited without "
         "changing the other instances"));
  m_renameButton = new QPushButton(tr("Rename"), this);

  QToolButton *convertButton = new QToolButton(this);
  convertButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
  convertButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  if (QAction *convertAction =
          CommandManager::instance()->getAction(MI_ConvertToSymbol))
    convertButton->setDefaultAction(convertAction);

  QVBoxLayout *mainLayout = new QVBoxLayout();
  mainLayout->setContentsMargins(0, 0, 0, 0);
  mainLayout->setSpacing(0);
  {
    mainLayout->addWidget(m_list, 1);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(3, 3, 3, 0);
    buttonLayout->setSpacing(3);
    buttonLayout->addWidget(m_insertButton);
    buttonLayout->addWidget(m_editButton);
    buttonLayout->addWidget(m_duplicateButton);
    buttonLayout->addWidget(m_renameButton);
    mainLayout->addLayout(buttonLayout);

    QHBoxLayout *convertLayout = new QHBoxLayout();
    convertLayout->setContentsMargins(3, 3, 3, 3);
    convertLayout->addWidget(convertButton);
    mainLayout->addLayout(convertLayout);
  }
  setLayout(mainLayout);

  TApp *app = TApp::instance();
  connect(app->getCurrentScene(), SIGNAL(sceneSwitched()), this,
          SLOT(refresh()));
  connect(app->getCurrentScene(), SIGNAL(castChanged()), this, SLOT(refresh()));
  connect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
          SLOT(refresh()));
  connect(app->getCurrentXsheet(), SIGNAL(xsheetSwitched()), this,
          SLOT(onXsheetSwitched()));
  connect(IconGenerator::instance(), SIGNAL(iconGenerated()), this,
          SLOT(updateIcons()));

  connect(m_list,
          SIGNAL(currentItemChanged(QListWidgetItem *, QListWidgetItem *)),
          this, SLOT(onCurrentSymbolChanged()));
  connect(m_list, SIGNAL(itemDoubleClicked(QListWidgetItem *)), this,
          SLOT(editSymbol()));
  connect(m_list, SIGNAL(customContextMenuRequested(const QPoint &)), this,
          SLOT(onContextMenu(const QPoint &)));

  connect(m_insertButton, SIGNAL(clicked()), this, SLOT(insertInstance()));
  connect(m_editButton, SIGNAL(clicked()), this, SLOT(editSymbol()));
  connect(m_duplicateButton, SIGNAL(clicked()), this, SLOT(duplicateSymbol()));
  connect(m_renameButton, SIGNAL(clicked()), this, SLOT(renameSymbol()));

  updateButtons();
}

//-----------------------------------------------------------------------------

TXshChildLevel *SymbolsPane::getSymbol(QListWidgetItem *item) const {
  return item ? findSymbol(item->data(Qt::UserRole).toString()) : 0;
}

//-----------------------------------------------------------------------------

TXshChildLevel *SymbolsPane::getCurrentSymbol() const {
  return getSymbol(m_list->currentItem());
}

//-----------------------------------------------------------------------------

// Returns the icon of the first symbol frame. Unless render is true, only an
// icon already rendered is returned.
QPixmap SymbolsPane::getIcon(TXshChildLevel *symbol, bool render) const {
  QPixmap icon =
      IconGenerator::instance()->getIcon(symbol, TFrameId(1), false, !render);
  if (icon.isNull()) return m_emptyIcon;
  return scalePixmapKeepingAspectRatio(icon, SymbolIconSize, Qt::transparent);
}

//-----------------------------------------------------------------------------

void SymbolsPane::showEvent(QShowEvent *e) {
  QWidget::showEvent(e);
  refresh();
}

//-----------------------------------------------------------------------------

void SymbolsPane::refresh() {
  if (!isVisible()) return;

  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  std::vector<TXshChildLevel *> symbols;
  std::set<TXshLevel *> usedLevels;
  if (scene) {
    TLevelSet *levelSet = scene->getLevelSet();
    for (int i = 0; i < levelSet->getLevelCount(); i++) {
      TXshLevel *level = levelSet->getLevel(i);
      if (level && level->getChildLevel())
        symbols.push_back(level->getChildLevel());
    }
    if (!symbols.empty()) scene->getTopXsheet()->getUsedLevels(usedLevels);
  }

  // rebuild the list only when the symbols change, to keep the current item
  bool sameSymbols = m_list->count() == (int)symbols.size();
  for (int i = 0; sameSymbols && i < (int)symbols.size(); i++)
    sameSymbols = m_list->item(i)->data(Qt::UserRole).toString() ==
                  QString::fromStdWString(symbols[i]->getName());
  if (!sameSymbols) {
    QListWidgetItem *current = m_list->currentItem();
    QString currentName =
        current ? current->data(Qt::UserRole).toString() : QString();
    m_list->clear();
    for (TXshChildLevel *symbol : symbols) {
      QString name          = QString::fromStdWString(symbol->getName());
      QListWidgetItem *item = new QListWidgetItem(m_list);
      item->setData(Qt::UserRole, name);
      if (name == currentName) m_list->setCurrentItem(item);
    }
  }

  // icons are rendered only for the current symbol when they are loaded on
  // demand, as for the column icons
  bool onDemand = Preferences::instance()->getColumnIconLoadingPolicy() ==
                  Preferences::LoadOnDemand;
  for (int i = 0; i < (int)symbols.size(); i++) {
    TXshChildLevel *symbol = symbols[i];
    QListWidgetItem *item  = m_list->item(i);
    QString name           = QString::fromStdWString(symbol->getName());
    bool used              = usedLevels.count(symbol) > 0;

    QString text = name + "\n" + tr("%1 frames").arg(symbol->getFrameCount());
    if (!used) text += "  " + tr("(unused)");
    item->setText(text);
    item->setToolTip(used ? name : tr("%1 is not used in the scene").arg(name));
    item->setIcon(
        QIcon(getIcon(symbol, !onDemand || item == m_list->currentItem())));
  }

  updateButtons();
}

//-----------------------------------------------------------------------------

void SymbolsPane::onXsheetSwitched() {
  if (!isVisible()) return;
  // a symbol may have just been edited: render the icons again
  for (int i = 0; i < m_list->count(); i++)
    if (TXshChildLevel *symbol = getSymbol(m_list->item(i)))
      IconGenerator::instance()->remove(symbol, TFrameId(1));
  refresh();
}

//-----------------------------------------------------------------------------

void SymbolsPane::updateIcons() {
  if (!isVisible()) return;
  for (int i = 0; i < m_list->count(); i++) {
    QListWidgetItem *item = m_list->item(i);
    if (TXshChildLevel *symbol = getSymbol(item))
      item->setIcon(QIcon(getIcon(symbol, false)));
  }
}

//-----------------------------------------------------------------------------

void SymbolsPane::updateButtons() {
  bool hasSymbol = getCurrentSymbol() != 0;
  m_insertButton->setEnabled(hasSymbol);
  m_editButton->setEnabled(hasSymbol);
  m_duplicateButton->setEnabled(hasSymbol);
  m_renameButton->setEnabled(hasSymbol);
}

//-----------------------------------------------------------------------------

void SymbolsPane::onCurrentSymbolChanged() {
  updateButtons();
  // render the icon of the current symbol, also when icons load on demand
  QListWidgetItem *item = m_list->currentItem();
  if (TXshChildLevel *symbol = getSymbol(item))
    item->setIcon(QIcon(getIcon(symbol, true)));
}

//-----------------------------------------------------------------------------

void SymbolsPane::onContextMenu(const QPoint &pos) {
  QListWidgetItem *item = m_list->itemAt(pos);
  if (item) m_list->setCurrentItem(item);

  QMenu menu(this);
  if (getSymbol(item)) {
    menu.addAction(tr("Insert Instance"), this, SLOT(insertInstance()));
    menu.addAction(tr("Edit Symbol"), this, SLOT(editSymbol()));
    menu.addAction(tr("Duplicate Symbol"), this, SLOT(duplicateSymbol()));
    menu.addAction(tr("Rename Symbol..."), this, SLOT(renameSymbol()));
    menu.addSeparator();
  }
  if (QAction *convertAction =
          CommandManager::instance()->getAction(MI_ConvertToSymbol))
    menu.addAction(convertAction);
  menu.exec(m_list->viewport()->mapToGlobal(pos));
}

//-----------------------------------------------------------------------------

void SymbolsPane::insertInstance() {
  TXshChildLevel *symbol = getCurrentSymbol();
  if (!symbol) return;

  // in the current column if it is empty, otherwise next to it (in a new
  // column unless the next one is empty)
  TApp *app    = TApp::instance();
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  int row      = app->getCurrentFrame()->getFrame();
  int col      = app->getCurrentColumn()->getColumnIndex();
  if (col < 0)
    col = 0;
  else if (!xsh->isColumnEmpty(col))
    col++;
  SymbolCmd::insertInstance(symbol, row, col, true);
}

//-----------------------------------------------------------------------------

void SymbolsPane::editSymbol() { SymbolCmd::editSymbol(getCurrentSymbol()); }

//-----------------------------------------------------------------------------

void SymbolsPane::duplicateSymbol() {
  SymbolCmd::duplicateSymbol(getCurrentSymbol());
}

//-----------------------------------------------------------------------------

void SymbolsPane::renameSymbol() {
  TXshChildLevel *symbol = getCurrentSymbol();
  if (!symbol) return;

  QString oldName = QString::fromStdWString(symbol->getName());
  bool ok         = false;
  QString name =
      DVGui::getText(tr("Rename Symbol"), tr("Symbol Name:"), oldName, &ok)
          .trimmed();
  if (!ok || name.isEmpty() || name == oldName) return;

  if (!SymbolCmd::renameSymbol(symbol, name.toStdWString())) {
    DVGui::warning(tr("The name %1 is already used.").arg(name));
    return;
  }
  // keep the renamed symbol current
  for (int i = 0; i < m_list->count(); i++)
    if (m_list->item(i)->data(Qt::UserRole).toString() == name)
      m_list->setCurrentRow(i);
}
