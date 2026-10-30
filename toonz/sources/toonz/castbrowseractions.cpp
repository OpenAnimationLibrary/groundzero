#include "castbrowseractions.h"
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
QString cleanPath(const TFilePath &path) {
  QFileInfo info(path.getQString());
  QString canonical = info.canonicalFilePath();
  if (!canonical.isEmpty()) return QDir::cleanPath(canonical);
  // Resolve symlinked ancestors even when the asset itself is missing.
  const QString absolute = info.absoluteFilePath();
  QDir ancestor(info.absolutePath());
  for (;;) {
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
class CoverUndo final : public TUndo {
  TXshLevelP m_level;
  TFrameId m_before, m_after;
  bool m_hadBefore, m_hasAfter;
  void apply(bool present, const TFrameId &frame) const {
    if (present) m_level->setCastCover(frame); else m_level->clearCastCover();
    changed();
  }
public:
  CoverUndo(TXshLevel *level, bool present, const TFrameId &frame)
      : m_level(level), m_before(level->getCastCover()), m_after(frame),
        m_hadBefore(level->hasCastCover()), m_hasAfter(present) {}
  void undo() const override { apply(m_hadBefore, m_before); }
  void redo() const override { apply(m_hasAfter, m_after); }
  int getSize() const override { return sizeof(*this); }
  QString getHistoryString() override { return tr("Choose Cast Cover Drawing"); }
};
QString drawingRange(TXshLevel *level) {
  std::vector<TFrameId> ids;
  level->getFids(ids);
  if (ids.empty()) return tr("No drawing identifiers");
  if (ids.size() == 1 && ids.front().isNoFrame()) return tr("Single image");
  QStringList gaps;
  qint64 previous = ids.front().getNumber();
  for (const auto &id : ids) {
    const qint64 current = id.getNumber();
    if (current > previous + 1) {
      gaps << (current == previous + 2 ? QString::number(previous + 1)
                 : QStringLiteral("%1–%2").arg(previous + 1).arg(current - 1));
      if (gaps.size() == 32) { gaps << tr("… more gaps"); break; }
    }
    previous = current;
  }
  return QStringLiteral("%1–%2; ").arg(QString::fromStdString(ids.front().expand()),
                                      QString::fromStdString(ids.back().expand())) +
      (gaps.empty() ? tr("no numeric gaps") : tr("numeric gaps: %1").arg(gaps.join(", ")));
}
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
bool missing(ToonzScene *source, TXshLevel *level) {
  TFilePath actual = source->decodeFilePath(level->getPath());
  ResourceImporter::extractPsdSuffix(actual);
  return level->getPath().isEmpty() || !TSystem::doesExistFileOrLevel(actual);
}
bool external(ToonzScene *source, TXshLevel *level) {
  if (level->getPath().isEmpty()) return false;
  QString path = cleanPath(source->decodeFilePath(level->getPath()));
  const auto project = source->getProject();
  if (project) {
    if (within(path, cleanPath(project->getProjectFolder()))) return false;
    for (int i = 0; i < project->getFolderCount(); ++i)
      if (within(path, cleanPath(source->decodeFilePath(TFilePath("+" + project->getFolderName(i)))))) return false;
  }
  if (!source->isUntitled() &&
      within(path, cleanPath(source->getScenePath().getParentDir()))) return false;
  return true;
}
QString status(ToonzScene *source, TXshLevel *level) {
  QStringList labels;
  if (missing(source, level)) labels << tr("Not saved / missing on disk");
  auto *simple = level->getSimpleLevel();
  auto *palette = level->getPaletteLevel();
  if ((simple && (simple->getProperties()->getDirtyFlag() ||
                  (simple->getPalette() && simple->getPalette()->getDirtyFlag()))) ||
      (palette && palette->getPalette() && palette->getPalette()->getDirtyFlag()))
    labels << tr("Modified");
  if (external(source, level)) labels << tr("External link");
  if (simple && simple->isReadOnly()) labels << tr("Read-only");
  if (labels.isEmpty()) labels << tr("Saved in project / scene folders");
  return labels.join(QStringLiteral(" · "));
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
  if (levels.empty()) return;
  std::vector<TXshLevelP> retained;
  for (auto *level : levels) retained.emplace_back(level);
  QDialog dialog(parent);
  dialog.setObjectName("CastInspector");
  QObject::connect(TApp::instance()->getCurrentScene(), &TSceneHandle::sceneSwitched,
                   &dialog, &QDialog::reject);
  dialog.setWindowTitle(tr("Scene Cast — Asset Inspector"));
  auto *layout = new QVBoxLayout(&dialog);
  auto *view = table(dialog, {tr("Level"), tr("Status"), tr("Frames"), tr("Drawing range / gaps"),
                             tr("Resolution / DPI / depth"), tr("Palette"), tr("Path"), tr("Tags")});
  for (auto *level : levels) {
    QString geometry;
    TPalette *palette = nullptr;
    if (auto *sl = level->getSimpleLevel()) {
      palette = sl->getPalette();
      const auto resolution = sl->getResolution();
      const auto dpi = sl->getDpi();
      geometry = tr("%1 × %2 / %3 × %4 DPI / %5 bpp")
          .arg(resolution.lx).arg(resolution.ly).arg(dpi.x).arg(dpi.y)
          .arg(sl->getProperties()->getBpp());
    }
    if (auto *pl = level->getPaletteLevel()) palette = pl->getPalette();
    QString paletteInfo = tr("None");
    if (palette) {
      paletteInfo = tr("%1; %2 styles").arg(QString::fromStdWString(palette->getPaletteName()))
                                         .arg(palette->getStyleCount());
      if (!palette->getGlobalName().empty())
        paletteInfo += tr("; studio link: %1").arg(QString::fromStdWString(palette->getGlobalName()));
      if (palette->getDirtyFlag()) paletteInfo += tr("; modified");
    }
    row(view, {name(level), status(scene(), level), QString::number(level->getFrameCount()),
               level->getSimpleLevel() ? drawingRange(level) : tr("Not a drawing level"),
               geometry, paletteInfo, scene()->decodeFilePath(level->getPath()).getQString(),
               level->getCastTags().join(", ")});
  }
  layout->addWidget(view);
  auto *preview = new QLabel(&dialog);
  preview->setAlignment(Qt::AlignCenter);
  preview->setMinimumHeight(160);
  layout->addWidget(preview);
  auto *slider = new QSlider(Qt::Horizontal, &dialog);
  slider->setAccessibleName(tr("Preview drawing"));
  layout->addWidget(slider);
  auto *cover = new QPushButton(tr("Use Preview Drawing as Cover"), &dialog);
  cover->setObjectName("CastChooseCover");
  auto *resetCover = new QPushButton(tr("Use First Drawing as Cover"), &dialog);
  resetCover->setObjectName("CastResetCover");
  auto *coverButtons = new QHBoxLayout;
  coverButtons->addWidget(cover); coverButtons->addWidget(resetCover);
  layout->addLayout(coverButtons);
  auto *play = new QPushButton(tr("Audition Audio"), &dialog);
  auto *stop = new QPushButton(tr("Stop Audio"), &dialog);
  auto *audioButtons = new QHBoxLayout;
  audioButtons->addWidget(play); audioButtons->addWidget(stop);
  layout->addLayout(audioButtons);
  std::unique_ptr<TSoundOutputDevice> audio;
  std::vector<TFrameId> frames;
  auto updatePreview = [&]() {
    int r = view->currentRow();
    if (r < 0 || r >= int(levels.size())) return;
    auto *sl = levels[r]->getSimpleLevel();
    if (!sl || frames.empty()) { preview->clear(); return; }
    int frame = std::min(slider->value(), int(frames.size()) - 1);
    QPixmap image = IconGenerator::instance()->getIcon(sl, frames[frame], false, false);
    preview->setPixmap(image.scaled(320, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    preview->setToolTip(tr("Drawing %1").arg(QString::fromStdString(frames[frame].expand())));
  };
  QObject::connect(view, &QTableWidget::itemSelectionChanged, &dialog, [&]() {
    if (audio) audio->stop();
    frames.clear();
    int r = view->currentRow();
    if (r >= 0 && r < int(levels.size())) levels[r]->getFids(frames);
    slider->setRange(0, std::max(0, int(frames.size()) - 1));
    slider->setValue(0);
    slider->setEnabled(r >= 0 && levels[r]->getSimpleLevel() && !frames.empty());
    cover->setEnabled(slider->isEnabled());
    resetCover->setEnabled(r >= 0 && levels[r]->getSimpleLevel() && levels[r]->hasCastCover());
    play->setEnabled(r >= 0 && levels[r]->getSoundLevel());
    updatePreview();
  });
  QObject::connect(IconGenerator::instance(), &IconGenerator::iconGenerated, &dialog, updatePreview);
  QObject::connect(slider, &QSlider::valueChanged, &dialog, [&](int) { updatePreview(); });
  auto setCover = [&](bool present) {
    const int r = view->currentRow();
    if (r < 0 || r >= int(levels.size()) || frames.empty()) return;
    auto *level = levels[r];
    const TFrameId frame = frames[std::min(slider->value(), int(frames.size()) - 1)];
    if ((present && level->hasCastCover() && level->getCastCover() == frame) ||
        (!present && !level->hasCastCover())) return;
    auto *undo = new CoverUndo(level, present, frame);
    undo->redo(); TUndoManager::manager()->add(undo);
    resetCover->setEnabled(present);
  };
  QObject::connect(cover, &QPushButton::clicked, &dialog, [&]() { setCover(true); });
  QObject::connect(resetCover, &QPushButton::clicked, &dialog, [&]() { setCover(false); });
  QObject::connect(play, &QPushButton::clicked, &dialog, [&]() {
    int r = view->currentRow();
    if (r < 0 || r >= int(levels.size())) return;
    auto *sound = levels[r]->getSoundLevel();
    if (!sound) return;
    try {
      auto track = sound->getSoundTrack();
      if (!track || track->getSampleCount() == 0) throw std::runtime_error("No audio samples");
      if (!audio) audio.reset(new TSoundOutputDevice);
      audio->stop();
      audio->play(track, TINT32(0), TINT32(track->getSampleCount() - 1));
    } catch (...) { QMessageBox::warning(&dialog, tr("Audition Audio"), tr("This sound could not be played.")); }
  });
  QObject::connect(stop, &QPushButton::clicked, &dialog, [&]() { if (audio) audio->stop(); });
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  view->selectRow(0);
  dialog.resize(950, 580);
  dialog.exec();
  if (audio) { audio->stop(); audio->close(); }
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
