#include "castbrowseractions.h"
#include "castviewer.h"
#include "toonz/imagemanager.h"
#include "timagecache.h"
#include "tapp.h"
#include "menubarcommandids.h"
#include "toonz/childstack.h"
#include "toonz/levelset.h"
#include "toonz/levelproperties.h"
#include "toonz/sceneresources.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshpalettelevel.h"
#include "toonz/txshsoundlevel.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/icongenerator.h"
#include "toonzqt/dvdialog.h"
#include "toonzqt/selection.h"
#include "tpalette.h"
#include "tsystem.h"
#include "tundo.h"
#include "tsound.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <stdexcept>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSlider>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QDir>
#include <QSignalBlocker>

namespace {
QString tr(const char *s) { return QObject::tr(s); }
ToonzScene *scene() { return TApp::instance()->getCurrentScene()->getScene(); }
QString name(TXshLevel *level) { return QString::fromStdWString(level->getName()); }
void changed() {
  TApp::instance()->getCurrentScene()->setDirtyFlag(true);
  TApp::instance()->getCurrentScene()->notifyCastChange();
  TApp::instance()->getCurrentXsheet()->notifyXsheetChanged();
}
QString cleanPath(const TFilePath &path, quint64 *queries = nullptr) {
  QFileInfo info(path.getQString());
  if (queries) ++*queries;
  QString canonical = info.canonicalFilePath();
  if (!canonical.isEmpty()) return QDir::cleanPath(canonical);
  // Resolve symlinked ancestors even when the asset itself is missing.
  const QString absolute = info.absoluteFilePath();
  QDir ancestor(info.absolutePath());
  for (;;) {
    if (queries) ++*queries;
    canonical = QFileInfo(ancestor.absolutePath()).canonicalFilePath();
    if (!canonical.isEmpty())
      return QDir::cleanPath(QDir(canonical).filePath(ancestor.relativeFilePath(absolute)));
    const QString before = ancestor.absolutePath();
    if (!ancestor.cdUp() || ancestor.absolutePath() == before) break;
  }
  return QDir::cleanPath(absolute);
}
bool within(const QString &path, const QString &folder) {
#ifdef _WIN32
  const Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
  const Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
  return path.compare(folder, sensitivity) == 0 ||
         path.startsWith(folder.endsWith('/') ? folder : folder + QLatin1Char('/'), sensitivity);
}
QTableWidget *table(QDialog &dialog, const QStringList &headers) {
  auto *view = new QTableWidget(&dialog);
  view->setColumnCount(headers.size());
  view->setHorizontalHeaderLabels(headers);
  view->setEditTriggers(QAbstractItemView::NoEditTriggers);
  view->setSelectionBehavior(QAbstractItemView::SelectRows);
  view->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  view->horizontalHeader()->setStretchLastSection(true);
  return view;
}
void row(QTableWidget *table, const QStringList &values) {
  int r = table->rowCount();
  table->insertRow(r);
  for (int c = 0; c < values.size(); ++c) {
    auto *item = new QTableWidgetItem(values[c]);
    item->setToolTip(values[c]);
    table->setItem(r, c, item);
  }
}
CastWorkflow::Index uses(QWidget *parent, const std::set<TXshLevel *> &wanted) {
  QProgressDialog progress(tr("Finding exposed level uses…"), tr("Cancel"),
                           0, 0, parent);
  progress.setWindowModality(Qt::WindowModal);
  progress.setMinimumDuration(300);
  ToonzScene *source = scene();
  TXsheetP top(source->getTopXsheet());
  CastWorkflow::Limits limits;
  if (wanted.empty()) { limits.recordOccurrences = false; limits.expandInstances = false; }
  return CastWorkflow::index(top.getPointer(), wanted, [&]() {
    QApplication::processEvents();
    return progress.wasCanceled() || scene() != source;
  }, limits);
}
class TagsUndo final : public TUndo {
  std::vector<TXshLevelP> m_levels;
  std::vector<QStringList> m_before, m_after;
  void apply(const std::vector<QStringList> &tags) const {
    for (size_t i = 0; i < m_levels.size(); ++i) m_levels[i]->setCastTags(tags[i]);
    changed();
  }
public:
  TagsUndo(const std::vector<TXshLevel *> &levels,
           const std::vector<QStringList> &tags) : m_after(tags) {
    for (auto *level : levels) {
      m_levels.push_back(TXshLevelP(level));
      m_before.push_back(level->getCastTags());
    }
  }
  void undo() const override { apply(m_before); }
  void redo() const override { apply(m_after); }
  int getSize() const override { return sizeof(*this) + int(m_levels.size()) * 1024; }
  QString getHistoryString() override { return tr("Edit Scene Cast Tags"); }
};
class RemoveUndo final : public TUndo {
  TXshLevelP m_level;
  TFilePath m_folder;
public:
  explicit RemoveUndo(TXshLevel *level) : m_level(level),
      m_folder(scene()->getLevelSet()->getFolder(level)) {}
  void undo() const override {
    auto *set = scene()->getLevelSet();
    set->insertLevel(m_level.getPointer());
    set->moveLevelToFolder(m_folder, m_level.getPointer());
    changed();
  }
  void redo() const override {
    scene()->getLevelSet()->removeLevel(m_level.getPointer());
    changed();
  }
  int getSize() const override { return sizeof(*this); }
  QString getHistoryString() override { return tr("Remove Unused Cast Level"); }
};
}

namespace CastBrowserActions {
const QStringList &StatusContext::roots() {
  if (m_rootsReady) return m_roots;
  m_rootsReady = true;
  ++queries.rootSets;
  const auto project = m_scene->getProject();
  if (project) {
    m_roots.append(cleanPath(project->getProjectFolder(), &queries.canonicalQueries));
    for (int i = 0; i < project->getFolderCount(); ++i) {
      ++queries.decodedPaths;
      m_roots.append(cleanPath(m_scene->decodeFilePath(
          TFilePath("+" + project->getFolderName(i))), &queries.canonicalQueries));
    }
  }
  if (!m_scene->isUntitled())
    m_roots.append(cleanPath(m_scene->getScenePath().getParentDir(), &queries.canonicalQueries));
  m_roots.removeDuplicates();
  return m_roots;
}

const TFilePath &AssetStatus::path() {
  if (!m_pathReady) {
    ++m_context.queries.decodedPaths;
    m_path = m_context.m_scene->decodeFilePath(m_level->getPath());
    m_pathReady = true;
  }
  return m_path;
}
bool AssetStatus::missing() {
  if (m_missing < 0) {
    if (m_level->getPath().isEmpty()) m_missing = true;
    else {
      TFilePath actual = path();
      ResourceImporter::extractPsdSuffix(actual);
      ++m_context.queries.diskQueries;
      m_missing = !TSystem::doesExistFileOrLevel(actual);
    }
  }
  return m_missing != 0;
}
bool AssetStatus::external() {
  if (m_external < 0) {
    m_external = false;
    if (!m_level->getPath().isEmpty()) {
      const QString actual = cleanPath(path(), &m_context.queries.canonicalQueries);
      m_external = true;
      for (const auto &root : m_context.roots())
        if (within(actual, root)) { m_external = false; break; }
    }
  }
  return m_external != 0;
}
bool AssetStatus::modified() {
  if (m_modified < 0) m_modified = CastBrowserActions::modified(m_level);
  return m_modified != 0;
}
bool AssetStatus::cached() {
  if (m_cached < 0) {
    m_cached = false;
    if (auto *simple = m_level->getSimpleLevel()) {
      std::vector<TFrameId> frames;
      simple->getFids(frames);
      for (const auto &frame : frames) {
        ++m_context.queries.cacheQueries;
        if (TImageCache::instance()->isCached(simple->getImageId(frame))) {
          m_cached = true;
          break;
        }
      }
    }
  }
  return m_cached != 0;
}
bool AssetStatus::unsaved() {
  return !m_level->sourceExpectedOnDisk() && missing();
}

bool modified(TXshLevel *level) {
  auto *simple = level->getSimpleLevel();
  auto *palette = level->getPaletteLevel();
  return (simple && (simple->getProperties()->getDirtyFlag() ||
      (simple->getPalette() && simple->getPalette()->getDirtyFlag()))) ||
      (palette && palette->getPalette() && palette->getPalette()->getDirtyFlag());
}
bool cached(TXshLevel *level) {
  auto *simple = level->getSimpleLevel();
  if (!simple) return false;
  std::vector<TFrameId> frames;
  simple->getFids(frames);
  for (const auto &frame : frames)
    if (TImageCache::instance()->isCached(simple->getImageId(frame))) return true;
  return false;
}
bool unsaved(ToonzScene *source, TXshLevel *level) {
  return !level->sourceExpectedOnDisk() && missing(source, level);
}
bool missing(ToonzScene *source, TXshLevel *level) {
  TFilePath actual = source->decodeFilePath(level->getPath());
  ResourceImporter::extractPsdSuffix(actual);
  return level->getPath().isEmpty() || !TSystem::doesExistFileOrLevel(actual);
}
bool external(ToonzScene *source, TXshLevel *level) {
  StatusContext context(source);
  return AssetStatus(context, level).external();
}
QString status(ToonzScene *source, TXshLevel *level) {
  StatusContext context(source);
  return AssetStatus(context, level).text();
}
QString AssetStatus::text() {
  QStringList labels;
  if (missing()) labels << (unsaved()
      ? tr("New / not saved") : tr("Missing on disk"));
  else labels << tr("Saved source exists");
  auto *simple = m_level->getSimpleLevel();
  if (modified()) labels << tr("Modified");
  if (simple) labels << (cached() ? tr("Decoded drawing cached (memory or swap)") : tr("No decoded drawing cached"));
  if (external()) labels << tr("External link");
  ++m_context.queries.libraryQueries;
  switch (m_level->getLibraryRelationship()) {
  case TXshLevel::LibraryProvenance::Linked: labels << tr("Library-linked file"); break;
  case TXshLevel::LibraryProvenance::LocalCopy: labels << tr("Local copy from Library"); break;
  case TXshLevel::LibraryProvenance::Snapshot: labels << tr("Library snapshot"); break;
  case TXshLevel::LibraryProvenance::Replaced: labels << tr("Library origin; source replaced"); break;
  default: break;
  }
  if (simple && simple->isReadOnly()) labels << tr("Read-only");
  if (labels.isEmpty()) labels << tr("Saved in project / scene folders");
  return labels.join(QStringLiteral(" · "));
}
QChar AssetStatus::badge() {
  if (unsaved()) return QLatin1Char('N');
  if (missing()) return QLatin1Char('!');
  if (modified()) return QLatin1Char('*');
  if (external()) return QLatin1Char('L');
  if (cached()) return QLatin1Char('C');
  return QChar();
}

void whereUsed(QWidget *parent, const std::vector<TXshLevel *> &levels) {
  if (levels.empty()) return;
  ToonzScene *source = scene();
  std::vector<TXshLevelP> retained;
  for (auto *level : levels) retained.emplace_back(level);
  std::set<TXshLevel *> wanted(levels.begin(), levels.end());
  auto found = uses(parent, wanted);
  if (scene() != source) return;
  QDialog dialog(parent);
  QObject::connect(TApp::instance()->getCurrentScene(), &TSceneHandle::sceneSwitched,
                   &dialog, &QDialog::reject);
  dialog.setObjectName("CastWhereUsed");
  dialog.setWindowTitle(tr("Scene Cast — Where Used"));
  auto *layout = new QVBoxLayout(&dialog);
  auto *summary = new QLabel(found.complete ?
      tr("Select an exposure and choose Go To. Rows are local to the named sheet. Cyclic references are not followed.") :
      tr("Search stopped or reached its limit. Results below are partial."), &dialog);
  summary->setWordWrap(true);
  layout->addWidget(summary);
  auto *view = table(dialog, {tr("Level"), tr("Sheet instance"), tr("Column"), tr("Frames")});
  view->setSelectionMode(QAbstractItemView::SingleSelection);
  for (const auto &use : found.occurrences) {
    QStringList path{tr("Scene")};
    for (const auto &step : use.path)
      path << tr("%1 [column %2, frame %3]")
                  .arg(name(step.child.getPointer())).arg(step.column + 1).arg(step.row + 1);
    row(view, {name(use.level.getPointer()), path.join(QStringLiteral(" / ")),
               QString::number(use.column + 1),
               QStringLiteral("%1–%2").arg(use.first + 1).arg(use.last + 1)});
  }
  layout->addWidget(view);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
  auto *go = buttons->addButton(tr("Go To Exposure"), QDialogButtonBox::ActionRole);
  go->setEnabled(false);
  QObject::connect(view, &QTableWidget::itemSelectionChanged, &dialog,
                   [=]() { go->setEnabled(view->currentRow() >= 0); });
  auto navigate = [&]() {
    int index = view->currentRow();
    if (scene() != source || index < 0 || index >= int(found.occurrences.size())) return;
    const auto &use = found.occurrences[index];
    if (!CastWorkflow::validPath(source->getTopXsheet(), use)) {
      summary->setText(tr("The scene changed. Close and search again."));
      go->setEnabled(false);
      return;
    }
    TApp *app = TApp::instance();
    if (TSelection::getCurrent()) TSelection::getCurrent()->selectNone();
    TUndoManager::manager()->beginBlock();
    const int depth = source->getChildStack()->getAncestorCount();
    for (int i = 0; i < depth; ++i)
      CommandManager::instance()->execute(MI_CloseChild);
    bool navigated = source->getChildStack()->getAncestorCount() == 0;
    for (const auto &step : use.path) {
      if (!navigated) break;
      app->getCurrentColumn()->setColumnIndex(step.column);
      app->getCurrentFrame()->setFrame(step.row);
      CommandManager::instance()->execute(MI_OpenChild);
      navigated = app->getCurrentXsheet()->getXsheet() == step.child->getXsheet();
    }
    TUndoManager::manager()->endBlock();
    if (!navigated || app->getCurrentXsheet()->getXsheet() != use.sheet.getPointer()) {
      summary->setText(tr("The requested sheet could not be opened. Close and search again."));
      return;
    }
    app->getCurrentColumn()->setColumnIndex(use.column);
    app->getCurrentFrame()->setFrame(use.first);
    app->getCurrentLevel()->setLevel(use.level.getPointer());
    dialog.accept();
  };
  QObject::connect(go, &QPushButton::clicked, &dialog, navigate);
  QObject::connect(view, &QTableWidget::cellDoubleClicked, &dialog,
                   [&](int, int) { navigate(); });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  dialog.resize(850, 460);
  dialog.exec();
}

void editTags(QWidget *parent, const std::vector<TXshLevel *> &levels) {
  if (levels.empty()) return;
  ToonzScene *source = scene();
  std::vector<TXshLevelP> retained;
  for (auto *level : levels) retained.emplace_back(level);
  bool accepted = false;
  QString input = QInputDialog::getText(parent, tr("Edit Scene Cast Tags"),
      tr("Comma-separated tags. Replaces tags on every selected level."),
      QLineEdit::Normal, levels.size() == 1 ? levels.front()->getCastTags().join(", ") : QString(),
      &accepted);
  if (!accepted || scene() != source) return;
  auto tags = CastWorkflow::normalizeTags(input);
  if (std::all_of(levels.begin(), levels.end(), [&](TXshLevel *level) { return level->getCastTags() == tags; })) return;
  std::vector<QStringList> after(levels.size(), tags);
  auto *undo = new TagsUndo(levels, after);
  undo->redo();
  TUndoManager::manager()->add(undo);
}

void inspect(QWidget *parent, const std::vector<TXshLevel *> &levels) {
  // Every native entry point belongs to a Cast browser. Keep the inspector
  // docked to that browser and return immediately to normal editing.
  for (QWidget *widget = parent; widget; widget = widget->parentWidget())
    if (auto *browser = qobject_cast<CastBrowser *>(widget)) {
      browser->showInspector(levels);
      return;
    }
}

void removeUnused(QWidget *parent) {
  ToonzScene *source = scene();
  auto found = uses(parent, {});
  if (scene() != source || !found.complete || found.cycle) {
    QMessageBox::warning(parent, tr("Unused Levels"), tr("The scene could not be indexed completely. No levels were removed."));
    return;
  }
  QDialog dialog(parent);
  dialog.setObjectName("CastUnusedReview");
  QObject::connect(TApp::instance()->getCurrentScene(), &TSceneHandle::sceneSwitched,
                   &dialog, &QDialog::reject);
  dialog.setWindowTitle(tr("Review Unused Cast Levels"));
  auto *layout = new QVBoxLayout(&dialog);
  layout->addWidget(new QLabel(tr("Checked levels will leave the Cast. Files stay on disk. Undo restores membership."), &dialog));
  auto *view = table(dialog, {tr("Remove"), tr("Level"), tr("Status / tags")});
  std::vector<TXshLevelP> unused;
  std::vector<TXshLevel *> levels;
  source->getLevelSet()->listLevels(levels);
  for (auto *level : levels) if (!found.used.count(level)) {
    unused.push_back(TXshLevelP(level));
    row(view, {QString(), name(level), status(source, level) + " / " + level->getCastTags().join(", ")});
    auto *check = view->item(view->rowCount() - 1, 0);
    check->setFlags(check->flags() | Qt::ItemIsUserCheckable);
    check->setCheckState(level->getCastTags().contains("keep", Qt::CaseInsensitive) ? Qt::Unchecked : Qt::Checked);
  }
  layout->addWidget(view);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Remove Checked"));
  buttons->button(QDialogButtonBox::Ok)->setEnabled(!unused.empty());
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  dialog.resize(750, 430);
  if (dialog.exec() != QDialog::Accepted || scene() != source) return;
  auto current = uses(parent, {});
  if (!current.complete || current.cycle || scene() != source) return;
  TUndoManager::manager()->beginBlock();
  for (int i = 0; i < int(unused.size()); ++i)
    if (view->item(i, 0)->checkState() == Qt::Checked &&
        !current.used.count(unused[i].getPointer()) &&
        source->getLevelSet()->getLevel(unused[i]->getName()) == unused[i].getPointer()) {
      auto *undo = new RemoveUndo(unused[i].getPointer());
      undo->redo(); TUndoManager::manager()->add(undo);
    }
  TUndoManager::manager()->endBlock();
}
}  // namespace CastBrowserActions
