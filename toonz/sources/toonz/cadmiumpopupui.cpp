#include "cadmiumpopup.h"
#include "cadmiumreviewview.h"
#include "cadmiumruntime.h"
#include "tcolorstyles.h"
#include "tsimplecolorstyles.h"
#include "toonz/txshleveltypes.h"
#include "toonzqt/uitranslations.h"

#include <QAction>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTableWidget>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

namespace {
constexpr int SelectedDecisionRole = Qt::UserRole + 1;
QString frameName(const TFrameId &fid) {
  return QString::number(fid.getNumber()) + fid.getLetter();
}
QString translated(const char *text) {
  return QCoreApplication::translate("CadmiumPopup", text);
}
QString candidateName(const Cadmium::ProposalCandidate &candidate,
                      const QVector<TFrameId> &references) {
  QString origin;
  if (candidate.referenceIndex < 0) {
    origin = translated("Palette style %1").arg(candidate.style);
  } else {
    const QString drawing = candidate.referenceIndex < references.size()
        ? frameName(references[candidate.referenceIndex])
        : QString::number(candidate.referenceIndex + 1);
    origin = translated("Drawing %1, region %2").arg(drawing).arg(candidate.referenceRegion);
  }
  if (candidate.nativeColor.valid) {
    const Cadmium::NativeColor &color = candidate.nativeColor;
    origin += translated(" — %1, %2, %3; alpha %4")
        .arg(color.r, 0, 'g', 9).arg(color.g, 0, 'g', 9)
        .arg(color.b, 0, 'g', 9).arg(color.a, 0, 'g', 9);
  } else if (candidate.style >= 0) {
    origin += translated(" — style %1 (%2)").arg(candidate.style)
                  .arg(candidate.color.name(QColor::HexArgb));
  }
  if (candidate.supportingReferences.size() > 1)
    origin += translated(" — %1 references agree").arg(candidate.supportingReferences.size());
  return origin;
}

void updateRegionCaptions(QTableWidget *table, int region,
                          const Cadmium::RegionProposal &proposal,
                          const QVector<TFrameId> &references) {
  QTableWidgetItem *number = table->item(region, 0);
  QTableWidgetItem *prediction = table->item(region, 1);
  QTableWidgetItem *decision = table->item(region, 2);
  QTableWidgetItem *state = table->item(region, 3);
  if (!number || !prediction || !decision || !state) return;
  number->setText(region ? QString::number(region) : CadmiumPopup::tr("Background"));
  prediction->setText(proposal.candidates.isEmpty() ? CadmiumPopup::tr("Unmatched") :
      proposal.candidates.size() == 1 ? proposal.candidates.front().color.name(QColor::HexArgb) :
      CadmiumPopup::tr("%1 fill choices").arg(proposal.candidates.size()));
  const bool selected = proposal.selected >= 0 && proposal.selected < proposal.candidates.size();
  decision->setText(proposal.selected == -2 ? CadmiumPopup::tr("Skip region") :
      selected ? candidateName(proposal.candidates[proposal.selected], references) :
                 CadmiumPopup::tr("Unresolved"));
  decision->setToolTip(region ? decision->text() :
      CadmiumPopup::tr("Background and line pixels are protected."));
  QStringList states;
  if (!region || proposal.protectedRegion) states.append(CadmiumPopup::tr("Protected"));
  if (proposal.locked) states.append(CadmiumPopup::tr("Locked"));
  if (!proposal.protectedRegion && proposal.selected == -1 && proposal.candidates.size() > 1)
    states.append(CadmiumPopup::tr("Conflict"));
  state->setText(states.join(QStringLiteral(" · ")));
  prediction->setToolTip(selected ?
      candidateName(proposal.candidates[proposal.selected], references) : QString());
}

// Editors are created only for the active cell. Large drawings must not
// allocate a widget and duplicate every reference choice for every region.
class RegionDecisionDelegate final : public QStyledItemDelegate {
public:
  using Choices = QVector<QPair<QString, int>>;
  using ChoiceSource = std::function<Choices(int)>;
  using Commit = std::function<void(int, int, int)>;
  RegionDecisionDelegate(ChoiceSource choices, std::function<int()> current,
                         Commit commit, QObject *parent)
      : QStyledItemDelegate(parent), m_choices(std::move(choices)),
        m_current(std::move(current)), m_commit(std::move(commit)) {}
  QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                        const QModelIndex &index) const override {
    auto *editor = new QComboBox(parent);
    editor->setMaxVisibleItems(20);
    editor->setProperty("cadmiumDrawing", m_current());
    const Choices choices = m_choices(index.row());
    for (const auto &choice : choices) {
      editor->addItem(choice.first, choice.second);
      editor->setItemData(editor->count() - 1, choice.first, Qt::ToolTipRole);
    }
    QPointer<RegionDecisionDelegate> self(const_cast<RegionDecisionDelegate *>(this));
    QPointer<QComboBox> guard(editor);
    const int drawing = m_current();
    const int region = index.row();
    UiTranslations::onLanguageChanged(editor, [self, guard, drawing, region]() {
      if (!self || !guard || self->m_current() != drawing) return;
      const QSignalBlocker blocker(guard.data());
      const QSignalBlocker modelBlocker(guard->model());
      const Choices choices = self->m_choices(region);
      for (const auto &choice : choices) {
        const int choiceIndex = guard->findData(choice.second);
        if (choiceIndex < 0) continue;
        guard->setItemText(choiceIndex, choice.first);
        guard->setItemData(choiceIndex, choice.first, Qt::ToolTipRole);
      }
      guard->update();
    });
    QObject::connect(editor, QOverload<int>::of(&QComboBox::activated), editor,
                     [self, guard](int) {
      if (!self || !guard) return;
      emit self->commitData(guard.data());
      if (guard) emit self->closeEditor(guard.data());
    });
    return editor;
  }
  void setEditorData(QWidget *widget, const QModelIndex &index) const override {
    if (auto *editor = qobject_cast<QComboBox *>(widget))
      editor->setCurrentIndex(editor->findData(index.data(SelectedDecisionRole)));
  }
  void setModelData(QWidget *widget, QAbstractItemModel *,
                    const QModelIndex &index) const override {
    if (auto *editor = qobject_cast<QComboBox *>(widget))
      m_commit(editor->property("cadmiumDrawing").toInt(), index.row(),
                editor->currentData().toInt());
  }
  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    if (!(index.flags() & Qt::ItemIsEditable)) {
      QStyledItemDelegate::paint(painter, option, index);
      return;
    }
    QStyleOptionComboBox combo;
    if (option.widget) combo.initFrom(option.widget);
    combo.rect = option.rect;
    combo.state = option.state;
    combo.currentText = index.data(Qt::DisplayRole).toString();
    combo.frame = true;
    QStyle *style = option.widget ? option.widget->style() : QApplication::style();
    style->drawComplexControl(QStyle::CC_ComboBox, &combo, painter, option.widget);
    style->drawControl(QStyle::CE_ComboBoxLabel, &combo, painter, option.widget);
  }
private:
  ChoiceSource m_choices;
  std::function<int()> m_current;
  Commit m_commit;
};

bool fullyResolved(const QVector<Cadmium::RegionProposal> &proposals) {
  for (int region = 1; region < proposals.size(); ++region)
    if (!proposals[region].protectedRegion &&
        proposals[region].selected == -1)
      return false;
  return true;
}
}

CadmiumPopup::CadmiumPopup(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Cadmium Assisted Paint"));
  resize(1240, 920);
  auto *layout = new QVBoxLayout(this);
  auto *intro = new QLabel(tr("Choose painted references, compare proposals, and click regions to correct, "
      "skip, or protect them. Save painting progress and regenerate selected work while retaining approved decisions."), this);
  intro->setWordWrap(true);
  layout->addWidget(intro);
  auto *setup = new QHBoxLayout;
  auto *executable = new QPushButton(tr("Choose Worker…"), this);
  auto *models = new QPushButton(tr("Choose Models Folder…"), this);
  auto *unload = new QPushButton(tr("Unload Models"), this);
  setup->addWidget(executable); setup->addWidget(models); setup->addWidget(unload);
  setup->addStretch();
  auto *workerStatus = new QLabel(CadmiumRuntime::instance()->statusText(), this);
  workerStatus->setObjectName(QStringLiteral("cadmiumWorkerStatus"));
  workerStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  workerStatus->setMinimumWidth(160);
  setup->addWidget(workerStatus, 1);
  layout->addLayout(setup);
  connect(executable, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose Cadmium Worker"));
    if (!path.isEmpty()) CadmiumRuntime::instance()->setExecutablePath(path);
  });
  connect(models, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getExistingDirectory(this, tr("Choose Model Folder"));
    if (!path.isEmpty()) CadmiumRuntime::instance()->setModelsDirectory(path);
  });
  connect(unload, &QPushButton::clicked, CadmiumRuntime::instance(), &CadmiumRuntime::stop);

  auto *splitter = new QSplitter(this);
  m_list = new QListWidget(splitter);
  m_list->setObjectName(QStringLiteral("cadmiumDrawingJobs"));
  m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_list->setMinimumWidth(160);
  m_list->setMaximumWidth(240);
  m_list->setToolTip(tr("Select drawing jobs for generation. Current-drawing modes use the active row."));
  auto *details = new QWidget(splitter);
  auto *detail = new QVBoxLayout(details);
  detail->setContentsMargins(0, 0, 0, 0);
  auto *references = new QGroupBox(tr("Painted references for the current drawing"), details);
  auto *referenceLayout = new QHBoxLayout(references);
  m_referenceList = new QListWidget(references);
  m_referenceList->setObjectName(QStringLiteral("cadmiumReferenceChoices"));
  m_referenceList->setMaximumHeight(90);
  m_referenceList->setToolTip(tr("Check each reference to compare. Disagreeing native fills remain explicit choices."));
  referenceLayout->addWidget(m_referenceList, 1);
  auto *referencePreview = new QFormLayout;
  m_previewReference = new QComboBox(references);
  m_previewReference->setObjectName(QStringLiteral("cadmiumReferencePreview"));
  referencePreview->addRow(tr("Overlay reference:"), m_previewReference);
  referenceLayout->addLayout(referencePreview);
  detail->addWidget(references);

  auto *raster = new QGroupBox(tr("Full-color line source"), details);
  raster->setObjectName(QStringLiteral("cadmiumRasterOptions"));
  auto *source = new QFormLayout(raster);
  m_rasterSource = new QComboBox(raster);
  m_rasterSource->addItems({tr("Transparent line artwork"), tr("Opaque paper scan"), tr("Linked clean-line level")});
  UiTranslations::watchCombo(m_rasterSource);
  source->addRow(tr("Raster source:"), m_rasterSource);
  m_lineLevel = new QComboBox(raster);
  UiTranslations::onLanguageChanged(m_lineLevel, [this]() {
    const int index = m_lineLevel->findData(QVariant::fromValue<qulonglong>(0));
    if (index < 0) return;
    const QSignalBlocker blocker(m_lineLevel);
    m_lineLevel->setItemText(index, tr("Choose a clean-line level"));
  });
  source->addRow(tr("Clean-line level:"), m_lineLevel);
  m_paperThreshold = new QDoubleSpinBox(raster);
  m_paperThreshold->setRange(0.0, 1.0); m_paperThreshold->setSingleStep(0.01);
  m_paperThreshold->setValue(0.92);
  source->addRow(tr("Paper threshold:"), m_paperThreshold);
  m_lineThreshold = new QDoubleSpinBox(raster);
  m_lineThreshold->setRange(0.0, 1.0); m_lineThreshold->setSingleStep(0.01);
  m_lineThreshold->setValue(0.2);
  source->addRow(tr("Line threshold:"), m_lineThreshold);
  m_extraction = new QCheckBox(tr("I reviewed and approve the extracted line mask"), raster);
  source->addRow(m_extraction);
  detail->addWidget(raster);
  m_repaint = new QCheckBox(tr("Replace existing paint in accepted target regions"), details);
  detail->addWidget(m_repaint);
  m_view = new CadmiumReviewView(details);
  detail->addWidget(m_view, 1);
  m_regions = new QTableWidget(details);
  m_regions->setObjectName(QStringLiteral("cadmiumRegionDecisions"));
  m_regions->setColumnCount(4);
  m_regions->setHorizontalHeaderLabels({tr("Region"), tr("Predictions"), tr("Decision"), tr("State")});
  UiTranslations::onLanguageChanged(m_regions, [this]() {
    const QSignalBlocker blocker(m_regions);
    const QSignalBlocker modelBlocker(m_regions->model());
    const int horizontal = m_regions->horizontalScrollBar()->value();
    const int vertical = m_regions->verticalScrollBar()->value();
    m_regions->setHorizontalHeaderLabels(
        {tr("Region"), tr("Predictions"), tr("Decision"), tr("State")});
    const int row = m_list->currentRow();
    if (row >= 0 && row < int(m_drawings.size())) {
      const Drawing &drawing = m_drawings[row];
      const int regions = std::min(m_regions->rowCount(), int(drawing.proposals.size()));
      for (int region = 0; region < regions; ++region)
        updateRegionCaptions(m_regions, region, drawing.proposals[region], drawing.referenceIds);
    }
    m_regions->horizontalScrollBar()->setValue(horizontal);
    m_regions->verticalScrollBar()->setValue(vertical);
    m_regions->horizontalHeader()->viewport()->update();
    m_regions->viewport()->update();
  });
  m_regions->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_regions->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_regions->setEditTriggers(QAbstractItemView::SelectedClicked |
                             QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
  m_regions->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_regions->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_regions->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
  m_regions->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  m_regions->setMinimumHeight(140); m_regions->setMaximumHeight(250);
  m_regions->setContextMenuPolicy(Qt::CustomContextMenu);
  detail->addWidget(m_regions);
  splitter->setStretchFactor(1, 1);
  layout->addWidget(splitter, 1);

  auto *generation = new QHBoxLayout;
  m_generationMode = new QComboBox(this);
  m_generationMode->setObjectName(QStringLiteral("cadmiumRegenerationMode"));
  m_generationMode->addItem(tr("Selected drawings"), int(Cadmium::RegenerationMode::All));
  m_generationMode->addItem(tr("Current drawing"), int(Cadmium::RegenerationMode::CurrentDrawing));
  m_generationMode->addItem(tr("Unresolved regions in selected drawings"), int(Cadmium::RegenerationMode::UnresolvedRegions));
  m_generationMode->addItem(tr("Selected regions in the current drawing"), int(Cadmium::RegenerationMode::SelectedRegions));
  UiTranslations::watchCombo(m_generationMode);
  generation->addWidget(m_generationMode, 1);
  m_generate = new QPushButton(tr("Generate"), this);
  m_cancel = new QPushButton(tr("Cancel Processing"), this);
  generation->addWidget(m_generate); generation->addWidget(m_cancel);
  layout->addLayout(generation);
  auto *application = new QHBoxLayout;
  m_apply = new QPushButton(tr("Apply Drawing"), this);
  m_review = new QPushButton(tr("Include Drawing in Reviewed Batch"), this);
  m_applyAll = new QPushButton(tr("Apply Reviewed Batch"), this);
  application->addWidget(m_apply); application->addWidget(m_review); application->addWidget(m_applyAll);
  layout->addLayout(application);
  auto *session = new QHBoxLayout;
  m_saveSession = new QPushButton(tr("Save Paint Session"), this);
  m_restoreSession = new QPushButton(tr("Resume Saved Session"), this);
  m_clearSession = new QPushButton(tr("Clear Saved Session"), this);
  m_saveSession->setToolTip(tr("Save references, decisions, protection, and review progress locally."));
  m_restoreSession->setToolTip(tr("Validate current artwork and resume saved painting progress."));
  session->addWidget(m_saveSession); session->addWidget(m_restoreSession); session->addWidget(m_clearSession);
  layout->addLayout(session);
  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  layout->addWidget(m_status);

  auto *delegate = new RegionDecisionDelegate(
      [this](int region) {
        RegionDecisionDelegate::Choices choices;
        choices.append(qMakePair(tr("Unresolved"), -1));
        choices.append(qMakePair(tr("Skip region"), -2));
        const int row = m_list->currentRow();
        if (row < 0 || row >= int(m_drawings.size())) return choices;
        const Drawing &drawing = m_drawings[row];
        if (region <= 0 || region >= drawing.proposals.size()) return choices;
        for (int candidate = 0; candidate < drawing.proposals[region].candidates.size(); ++candidate)
          choices.append(qMakePair(candidateName(drawing.proposals[region].candidates[candidate],
                                                drawing.referenceIds), candidate));
        return choices;
      }, [this] { return m_list->currentRow(); },
      [this](int drawing, int region, int candidate) {
        if (drawing < 0 || drawing >= int(m_drawings.size())) return;
        const TFrameId fid = m_drawings[drawing].fid;
        const quint64 epoch = m_snapshotEpoch;
        QTimer::singleShot(0, this, [this, drawing, fid, epoch, region, candidate] {
          if (m_refreshing || m_generating || m_restoring || m_backgroundTasks ||
              epoch != m_snapshotEpoch ||
              drawing != m_list->currentRow() || drawing >= int(m_drawings.size()) ||
              m_drawings[drawing].fid != fid) return;
          setRegionDecision(region, candidate);
        });
      }, m_regions);
  m_regions->setItemDelegateForColumn(2, delegate);

  connect(m_generate, &QPushButton::clicked, this, &CadmiumPopup::generate);
  connect(m_cancel, &QPushButton::clicked, this, &CadmiumPopup::cancel);
  connect(m_apply, &QPushButton::clicked, this, [this] { apply(false); });
  connect(m_applyAll, &QPushButton::clicked, this, [this] { apply(true); });
  connect(m_review, &QPushButton::clicked, this, [this] {
    const int row = m_list->currentRow();
    if (row < 0 || row >= int(m_drawings.size()) || m_generating || m_restoring || m_backgroundTasks) return;
    Drawing &drawing = m_drawings[row];
    if (!drawing.ready || drawing.applied || !fullyResolved(drawing.proposals)) return;
    drawing.reviewed = true; autosave(); refresh();
  });
  connect(m_saveSession, &QPushButton::clicked, this, [this] { saveSession(true); });
  connect(m_restoreSession, &QPushButton::clicked, this, &CadmiumPopup::restoreSession);
  connect(m_clearSession, &QPushButton::clicked, this, &CadmiumPopup::clearSession);
  m_saveTimer.setSingleShot(true);
  m_saveTimer.setInterval(500);
  connect(&m_saveTimer, &QTimer::timeout, this, [this] { saveSession(false); });
  connect(qApp, &QCoreApplication::aboutToQuit, this, &CadmiumPopup::flushSession);
  connect(m_list, &QListWidget::currentRowChanged, this, [this] {
    if (!m_refreshing) { autosave(); refreshPreview(); refresh(); }
  });
  connect(m_list, &QListWidget::itemSelectionChanged, this, [this] { if (!m_refreshing) refresh(); });
  connect(m_referenceList, &QListWidget::itemChanged, this, [this] {
    if (!m_refreshing) assignReferences();
  });
  connect(m_previewReference, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this] { if (!m_refreshing) updateProposalPreview(); });
  connect(m_generationMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
    if (m_refreshing) return;
    m_generation = Cadmium::RegenerationMode(m_generationMode->currentData().toInt());
    refresh();
  });
  const auto sourceChanged = [this] {
    if (m_refreshing) return;
    { const QSignalBlocker blocker(m_extraction); m_extraction->setChecked(false); }
    optionsChanged();
  };
  connect(m_rasterSource, QOverload<int>::of(&QComboBox::currentIndexChanged), this, sourceChanged);
  connect(m_lineLevel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, sourceChanged);
  connect(m_paperThreshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, sourceChanged);
  connect(m_lineThreshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, sourceChanged);
  connect(m_extraction, &QCheckBox::toggled, this, [this] { if (!m_refreshing) optionsChanged(); });
  connect(m_repaint, &QCheckBox::toggled, this, [this] {
    if (m_refreshing) return;
    for (Drawing &drawing : m_drawings) drawing.reviewed = false;
    autosave(); updateProposalPreview(); refresh();
  });
  connect(m_regions, &QTableWidget::itemSelectionChanged, this, [this] {
    if (m_refreshing) return;
    QSet<int> selected;
    for (const QModelIndex &index : m_regions->selectionModel()->selectedRows())
      if (index.row() > 0) selected.insert(index.row());
    setSelectedRegions(selected);
  });
  connect(m_regions, &QTableWidget::currentCellChanged, this,
          [this](int row, int, int, int) { if (!m_refreshing) m_view->setSelectedRegion(row); });
  connect(m_regions, &QTableWidget::customContextMenuRequested, this, [this](const QPoint &point) {
    const int row = m_regions->rowAt(point.y());
    if (row >= 0) showRegionMenu(row, m_regions->viewport()->mapToGlobal(point));
  });
  connect(m_view, &CadmiumReviewView::regionActivated, this, [this](int region) {
    const int row = m_list->currentRow();
    if (m_generating || m_restoring || m_backgroundTasks || row < 0 || row >= int(m_drawings.size())) return;
    QSet<int> selected;
    if (QApplication::keyboardModifiers() & Qt::ControlModifier) {
      selected = m_drawings[row].selectedRegions;
      if (selected.contains(region)) selected.remove(region); else if (region > 0) selected.insert(region);
    } else if (region > 0) selected.insert(region);
    if (region >= 0 && region < m_regions->rowCount()) {
      const QSignalBlocker blocker(m_regions);
      m_regions->setCurrentCell(region, 0, QItemSelectionModel::NoUpdate);
    }
    setSelectedRegions(selected);
  });
  connect(m_view, &CadmiumReviewView::regionContextMenuRequested, this, &CadmiumPopup::showRegionMenu);
  CadmiumRuntime *runtime = CadmiumRuntime::instance();
  connect(runtime, &CadmiumRuntime::statusChanged, workerStatus, [runtime, workerStatus] {
    workerStatus->setText(runtime->statusText());
    workerStatus->setToolTip(runtime->statusText());
  });
  connect(runtime, &CadmiumRuntime::resultReady, this, &CadmiumPopup::received);
  connect(runtime, &CadmiumRuntime::failed, this, &CadmiumPopup::failed);
  connect(runtime, &CadmiumRuntime::workerStopped, this, [this] {
    if (!m_generating && !m_restoring && !m_backgroundTasks) return;
    ++m_snapshotEpoch; m_cancelled = true; m_generating = m_restoring = false;
    m_running = -1; m_jobs.clear();
    if (m_sessionDirty) m_saveTimer.start();
    refresh();
    m_status->setText(tr("The worker was unloaded. Pending processing was cancelled."));
  });
  connect(runtime, &CadmiumRuntime::statusChanged, this, [this] {
    if (m_generating || m_restoring || m_backgroundTasks) return;
    for (const Drawing &drawing : m_drawings) if (drawing.ready && !drawing.applied) return;
    m_status->setText(CadmiumRuntime::instance()->statusText());
  });
  UiTranslations::onLanguageChanged(this, [this]() {
    const QSignalBlocker listBlocker(m_list);
    const int horizontal = m_list->horizontalScrollBar()->value();
    const int vertical = m_list->verticalScrollBar()->value();
    updateJobCaptions();
    m_list->doItemsLayout();
    m_list->horizontalScrollBar()->setValue(horizontal);
    m_list->verticalScrollBar()->setValue(vertical);

    const int row = m_list->currentRow();
    if (row < 0 || row >= int(m_drawings.size())) return;
    const Drawing &drawing = m_drawings[row];
    const QSignalBlocker previewBlocker(m_previewReference);
    for (int index = 0; index < m_previewReference->count(); ++index) {
      const int reference = m_previewReference->itemData(index).toInt();
      if (reference >= 0 && reference < drawing.referenceIds.size())
        m_previewReference->setItemText(index,
            tr("Drawing %1").arg(frameName(drawing.referenceIds[reference])));
    }
  });
}

void CadmiumPopup::updateJobCaptions() {
  for (int index = 0; index < int(m_drawings.size()); ++index) {
    const Drawing &drawing = m_drawings[index];
    const QString state = drawing.applied ? tr("Applied") : !drawing.error.isEmpty() ? tr("Failed") :
        m_generating && index == m_running ? tr("Processing") :
        m_restoring ? tr("Restoring") : drawing.ready ?
        drawing.reviewed ? tr("Reviewed") : tr("Review") : tr("Not generated");
    if (QListWidgetItem *item = m_list->item(index)) {
      item->setText(tr("%1 — %2").arg(frameName(drawing.fid), state));
      const UiTranslations::Recipe error = UiTranslations::recipe(drawing.error);
      item->setToolTip(drawing.error.isEmpty()
          ? tr("%1 assigned references").arg(drawing.referenceIds.size()) :
            error ? error() : drawing.error);
    }
  }
}

void CadmiumPopup::refresh() {
  const QScopedValueRollback<bool> guard(m_refreshing, true);
  const bool busy = m_generating || m_restoring || m_backgroundTasks;
  const bool available = currentScene();
  const int row = m_list->currentRow();
  const bool current = row >= 0 && row < int(m_drawings.size());
  const bool restrictJobs = !m_list->selectedItems().isEmpty();
  bool reviewed = false, eligible = false;
  updateJobCaptions();
  for (int index = 0; index < int(m_drawings.size()); ++index) {
    const Drawing &drawing = m_drawings[index];
    reviewed |= drawing.ready && drawing.reviewed && !drawing.applied;
    const QListWidgetItem *job = m_list->item(index);
    eligible |= !drawing.applied && !drawing.referenceIds.isEmpty() &&
                (!restrictJobs || (job && job->isSelected()));
  }
  const bool ready = current && m_drawings[row].ready && !m_drawings[row].applied;
  const bool resolved = ready && fullyResolved(m_drawings[row].proposals);
  if (m_generation == Cadmium::RegenerationMode::CurrentDrawing ||
      m_generation == Cadmium::RegenerationMode::SelectedRegions)
    eligible = current && !m_drawings[row].applied && !m_drawings[row].referenceIds.isEmpty();
  if (m_generation == Cadmium::RegenerationMode::SelectedRegions)
    eligible = eligible && !m_drawings[row].selectedRegions.isEmpty();
  m_generate->setEnabled(available && !busy && eligible && !m_references.empty());
  m_apply->setEnabled(available && !busy && resolved);
  m_review->setEnabled(available && !busy && resolved);
  m_applyAll->setEnabled(available && !busy && reviewed);
  m_apply->setToolTip(resolved ? tr("Apply accepted fills to this drawing.") :
      tr("Resolve, skip, or protect every unmatched or conflicting region before applying."));
  m_review->setToolTip(resolved ? tr("Include this drawing when applying the reviewed batch.") :
      tr("Resolve, skip, or protect every unmatched or conflicting region before reviewing."));
  m_cancel->setEnabled(busy);
  m_generationMode->setEnabled(!busy);
  m_generationMode->setCurrentIndex(m_generationMode->findData(int(m_generation)));
  m_referenceList->setEnabled(available && current && !busy && !m_drawings[row].applied);
  m_previewReference->setEnabled(current && m_previewReference->count() > 0);
  m_rasterSource->parentWidget()->setVisible(m_level && m_level->getType() == OVL_XSHLEVEL);
  m_rasterSource->setEnabled(available && !busy);
  m_lineLevel->setEnabled(available && !busy &&
      m_rasterSource->currentIndex() == int(Cadmium::RasterSource::LinkedLines));
  m_paperThreshold->setEnabled(available && !busy);
  m_lineThreshold->setEnabled(available && !busy);
  m_extraction->setEnabled(available && !busy);
  m_repaint->setEnabled(available && !busy);
  m_regions->setEnabled(available && current && !busy && !m_drawings[row].applied);
  m_saveSession->setEnabled(available && !busy && !m_drawings.empty() && !m_sessionWrites);
  m_restoreSession->setEnabled(available && !busy && !m_sessionWrites);
  m_clearSession->setEnabled(available && !busy && !m_sessionWrites);
}

void CadmiumPopup::refreshPreview() {
  const QScopedValueRollback<bool> guard(m_refreshing, true);
  const int row = m_list->currentRow();
  m_regions->setRowCount(0);
  m_referenceList->clear();
  const QVariant previousReference = m_previewReference->currentData();
  m_previewReference->clear();
  if (row < 0 || row >= int(m_drawings.size())) {
    m_view->clear(tr("Select a drawing job to inspect it."));
    return;
  }
  Drawing &drawing = m_drawings[row];
  for (const TFrameId &fid : m_references) {
    auto *item = new QListWidgetItem(frameName(fid), m_referenceList);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(drawing.referenceIds.contains(fid) ? Qt::Checked : Qt::Unchecked);
  }
  for (int reference = 0; reference < drawing.referenceIds.size(); ++reference)
    m_previewReference->addItem(tr("Drawing %1").arg(frameName(drawing.referenceIds[reference])), reference);
  const int preview = m_previewReference->findData(previousReference);
  if (preview >= 0) m_previewReference->setCurrentIndex(preview);
  if (drawing.target.lines.isNull()) {
    const bool busy = m_generating || m_restoring || m_backgroundTasks;
    m_view->clear(busy ? tr("Analyzing drawing…") : tr("Analyzing the line mask…"));
    if (!busy && currentScene()) {
      const TFrameId fid = drawing.fid;
      QTimer::singleShot(0, this, [this, row, fid] {
        if (!m_refreshing && !m_generating && !m_restoring && !m_backgroundTasks &&
            row == m_list->currentRow() && row < int(m_drawings.size()) &&
            m_drawings[row].fid == fid && m_drawings[row].target.lines.isNull()) {
          inspectDrawing();
          refresh();
        }
      });
    }
    return;
  }
  m_regions->setRowCount(drawing.proposals.size());
  for (int region = 0; region < drawing.proposals.size(); ++region) {
    const Cadmium::RegionProposal &proposal = drawing.proposals[region];
    auto *number = new QTableWidgetItem;
    auto *prediction = new QTableWidgetItem;
    if (proposal.candidates.size() == 1) prediction->setBackground(proposal.candidates.front().color);
    auto *decision = new QTableWidgetItem;
    decision->setData(SelectedDecisionRole, proposal.selected);
    auto *state = new QTableWidgetItem;
    for (QTableWidgetItem *item : {number, prediction, decision, state})
      item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    if (region > 0 && drawing.ready && !drawing.applied && !proposal.protectedRegion)
      decision->setFlags(decision->flags() | Qt::ItemIsEditable);
    m_regions->setItem(region, 0, number); m_regions->setItem(region, 1, prediction);
    m_regions->setItem(region, 2, decision); m_regions->setItem(region, 3, state);
    updateRegionCaptions(m_regions, region, proposal, drawing.referenceIds);
  }
  setSelectedRegions(drawing.selectedRegions);
  updateProposalPreview();
}

void CadmiumPopup::updateProposalPreview() {
  const int row = m_list->currentRow();
  if (row < 0 || row >= int(m_drawings.size())) return;
  const Drawing &drawing = m_drawings[row];
  if (!drawing.target.original || drawing.target.lines.isNull()) return;
  const QImage before = imagePreview(drawing.target.original, drawing.target);
  QImage after = drawing.target.lines;
  QString error;
  if (drawing.ready) {
    const TImageP result = composeProposal(drawing, error);
    if (result) after = imagePreview(result, drawing.target);
    else {
      after = before;
      if (!error.isEmpty()) m_status->setText(error);
    }
  }
  const int reference = m_previewReference->currentData().toInt();
  const QImage referenceImage = reference >= 0 && reference < drawing.referencePreviews.size()
      ? drawing.referencePreviews[reference] : QImage();
  m_view->setImages(before, after, referenceImage);
  m_view->setRegions(drawing.target.lines.size(), drawing.target.labels);
  QSet<int> protectedRegions;
  for (int region = 1; region < drawing.proposals.size(); ++region)
    if (drawing.proposals[region].protectedRegion) protectedRegions.insert(region);
  m_view->setProtectedRegions(protectedRegions);
  m_view->setSelectedRegion(m_regions->currentRow());
  QStringList warnings = drawing.target.warnings;
  for (const Cadmium::Frame &referenceFrame : drawing.references)
    for (const QString &warning : referenceFrame.warnings)
      if (!warnings.contains(warning)) warnings.append(warning);
  if (!warnings.isEmpty()) m_status->setText(warnings.join(QStringLiteral("\n")));
}

void CadmiumPopup::setSelectedRegions(const QSet<int> &regions) {
  const int row = m_list->currentRow();
  if (row < 0 || row >= int(m_drawings.size())) return;
  Drawing &drawing = m_drawings[row];
  QSet<int> selected;
  for (int region : regions)
    if (region > 0 && region < drawing.proposals.size()) selected.insert(region);
  drawing.selectedRegions = selected;
  const QSignalBlocker blocker(m_regions);
  int current = m_regions->currentRow();
  m_regions->clearSelection();
  for (int region : selected)
    m_regions->selectionModel()->select(m_regions->model()->index(region, 0),
        QItemSelectionModel::Select | QItemSelectionModel::Rows);
  if (!selected.contains(current)) current = selected.isEmpty() ? -1 : *std::min_element(selected.cbegin(), selected.cend());
  if (current >= 0) m_regions->setCurrentCell(current, 0, QItemSelectionModel::NoUpdate);
  else m_regions->selectionModel()->setCurrentIndex(QModelIndex(), QItemSelectionModel::NoUpdate);
  m_view->setSelectedRegion(current);
  refresh();
}

void CadmiumPopup::showRegionMenu(int region, const QPoint &position) {
  const int row = m_list->currentRow();
  if (m_generating || m_restoring || m_backgroundTasks || row < 0 || row >= int(m_drawings.size())) return;
  Drawing &drawing = m_drawings[row];
  if (drawing.applied || region <= 0 || region >= drawing.proposals.size()) return;
  if (!drawing.selectedRegions.contains(region)) {
    QSet<int> selected; selected.insert(region); setSelectedRegions(selected);
  }
  const TFrameId fid = drawing.fid;
  const quint64 epoch = m_snapshotEpoch;
  const QSet<int> selected = drawing.selectedRegions;
  const Cadmium::RegionProposal proposal = drawing.proposals[region];
  enum Operation { Accept = 1, Skip, Protect, Lock, Palette, SkipSelected, ProtectSelected, UnprotectSelected };
  QMenu menu(this);
  const auto action = [&](QMenu *owner, const QString &text, Operation operation, int candidate = -1) {
    QAction *item = owner->addAction(text);
    item->setData(QVariantMap{{QStringLiteral("operation"), int(operation)},
                              {QStringLiteral("candidate"), candidate}});
    return item;
  };
  if (drawing.ready) {
    if (proposal.selected >= 0 && proposal.selected < proposal.candidates.size())
      action(&menu, tr("Accept and lock chosen fill"), Accept, proposal.selected);
    if (!proposal.candidates.isEmpty()) {
      QMenu *choices = menu.addMenu(tr("Choose reference fill"));
      for (int candidate = 0; candidate < proposal.candidates.size(); ++candidate)
        action(choices, candidateName(proposal.candidates[candidate], drawing.referenceIds), Accept, candidate);
    }
    action(&menu, tr("Skip region"), Skip);
    if (proposal.selected >= 0 || proposal.selected == -2)
      action(&menu, proposal.locked ? tr("Unlock decision") : tr("Lock decision"), Lock);
    if (drawing.target.kind != Cadmium::Kind::FullColor && drawing.target.palette &&
        !drawing.target.palette->isAnimated()) {
      QMenu *palette = menu.addMenu(tr("Choose palette fill"));
      for (int style = 0; style < drawing.target.palette->getStyleCount(); ++style)
        if (dynamic_cast<const TSolidColorStyle *>(drawing.target.palette->getStyle(style)))
          action(palette, tr("Style %1").arg(style), Palette, style);
    }
  }
  action(&menu, proposal.protectedRegion ? tr("Unprotect region") : tr("Protect region"), Protect);
  if (selected.size() > 1) {
    menu.addSeparator();
    if (drawing.ready) action(&menu, tr("Skip selected regions"), SkipSelected);
    action(&menu, tr("Protect selected regions"), ProtectSelected);
    action(&menu, tr("Unprotect selected regions"), UnprotectSelected);
  }
  const QAction *chosen = menu.exec(position);
  if (!chosen || !currentScene() || row != m_list->currentRow() || row >= int(m_drawings.size()) ||
      m_drawings[row].fid != fid || epoch != m_snapshotEpoch ||
      m_drawings[row].applied || region >= m_drawings[row].proposals.size() ||
      m_generating || m_restoring || m_backgroundTasks) return;
  const QVariantMap data = chosen->data().toMap();
  const int candidate = data.value(QStringLiteral("candidate")).toInt();
  switch (data.value(QStringLiteral("operation")).toInt()) {
  case Accept: setRegionDecision(region, candidate); break;
  case Skip: setRegionDecision(region, -2); break;
  case Protect: setRegionProtected(region, !m_drawings[row].proposals[region].protectedRegion); break;
  case Lock: setRegionLocked(region, !m_drawings[row].proposals[region].locked); break;
  case Palette: addPaletteDecision(region, candidate); break;
  case SkipSelected: setRegionsSkipped(selected); break;
  case ProtectSelected: setRegionsProtected(selected, true); break;
  case UnprotectSelected: setRegionsProtected(selected, false); break;
  }
}
