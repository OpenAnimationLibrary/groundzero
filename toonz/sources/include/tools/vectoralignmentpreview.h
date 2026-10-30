#pragma once

#include "tvectorimage.h"
#include "tgl.h"
#include "tools/vectoralignment.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "toonzqt/planeviewer.h"
#include "toonz/tscenehandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txsheethandle.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <algorithm>
#include <vector>

namespace VectorAlignmentPreview {
inline QString tr(const char *text) {
  return QApplication::translate("VectorAlignmentPreview", text);
}
struct Entry {
  QString drawing;
  TVectorImageP before, after;
};

// Both native swatches use the union of the original and proposed bounds, so
// movement remains visible instead of being hidden by independent auto-fit.
class Swatch final : public PlaneViewer {
  TVectorImageP m_image;
  TRectD m_bounds;
public:
  explicit Swatch(QWidget *parent) : PlaneViewer(parent) {
    setMinimumSize(300, 260);
    setBgColor(TPixel32(230, 230, 230), TPixel32(255, 255, 255));
  }
  void setImage(const TVectorImageP &image, const TRectD &bounds) {
    m_image = image;
    m_bounds = bounds;
    update();
  }
  const TVectorImageP &image() const { return m_image; }
  const TRectD &bounds() const { return m_bounds; }
  void paintGL() override {
    viewAff() = TAffine();
    drawBackground();
    if (!m_image) return;
    const TPointD center((m_bounds.x0 + m_bounds.x1) * .5,
                         (m_bounds.y0 + m_bounds.y1) * .5);
    const double scale = std::min((width() - 20.) / std::max(1., m_bounds.getLx()),
                                 (height() - 20.) / std::max(1., m_bounds.getLy()));
    viewAff() = TTranslation(width() * .5, height() * .5) * TScale(scale) *
                TTranslation(-center);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    pushGLWorldCoordinates();
    draw(m_image);
    popGLCoordinates();
    glDisable(GL_BLEND);
  }
};

inline QString operation(VectorAlignment::Type type) {
  using namespace VectorAlignment;
  switch (type) {
  case ALIGN_LEFT: return tr("Align Left");
  case ALIGN_RIGHT: return tr("Align Right");
  case ALIGN_TOP: return tr("Align Top");
  case ALIGN_BOTTOM: return tr("Align Bottom");
  case ALIGN_CENTER_H: return tr("Align Horizontal Centers");
  case ALIGN_CENTER_V: return tr("Align Vertical Centers");
  case DISTRIBUTE_H: return tr("Distribute Horizontally");
  case DISTRIBUTE_V: return tr("Distribute Vertically");
  }
  return QString();
}
inline QString reference(VectorAlignment::Method method) {
  using namespace VectorAlignment;
  switch (method) {
  case SELECT_AREA: return tr("Selection Area");
  case FIRST_SELECTED: return tr("First Selected");
  case LAST_SELECTED: return tr("Last Selected");
  case SMALLEST_OBJECT: return tr("Smallest Object");
  case LARGEST_OBJECT: return tr("Largest Object");
  case CAMERA_AREA: return tr("Camera Area at the Current Scene Frame");
  }
  return QString();
}

inline bool review(const std::vector<Entry> &entries, VectorAlignment::Type type,
                   VectorAlignment::Method method, const QString &scope) {
  if (entries.empty()) return false;
  QDialog dialog(QApplication::activeWindow());
  dialog.setObjectName("vectorAlignmentPreview");
  dialog.setWindowTitle(tr("Preview Alignment and Distribution"));
  bool invalidated = false;
  const auto invalidate = [&] { invalidated = true; dialog.reject(); };
  if (auto *app = TTool::getApplication()) {
    QObject::connect(app->getCurrentScene(), &TSceneHandle::sceneSwitched,
                     &dialog, invalidate);
    QObject::connect(app->getCurrentScene(), &TSceneHandle::sceneChanged,
                     &dialog, invalidate);
    QObject::connect(app->getCurrentLevel(), &TXshLevelHandle::xshLevelSwitched,
                     &dialog, invalidate);
    QObject::connect(app->getCurrentLevel(), &TXshLevelHandle::xshLevelChanged,
                     &dialog, invalidate);
    QObject::connect(app->getCurrentXsheet(), &TXsheetHandle::xsheetChanged,
                     &dialog, invalidate);
    QObject::connect(app->getCurrentFrame(), &TFrameHandle::frameSwitched,
                     &dialog, invalidate);
    QObject::connect(app->getCurrentColumn(), &TColumnHandle::columnIndexSwitched,
                     &dialog, invalidate);
    QObject::connect(app->getCurrentTool(), &ToolHandle::toolSwitched,
                     &dialog, invalidate);
    QObject::connect(app->getCurrentTool(), &ToolHandle::toolChanged,
                     &dialog, invalidate);
  }
  auto *layout = new QGridLayout(&dialog);
  auto *summary = new QLabel(tr("%1 — Reference: %2\n%3; %4 changed drawing(s)")
      .arg(operation(type), reference(method), scope).arg(int(entries.size())), &dialog);
  summary->setObjectName("vectorAlignmentSummary");
  summary->setWordWrap(true);
  layout->addWidget(summary, 0, 0, 1, 2);
  auto *frames = new QComboBox(&dialog);
  frames->setObjectName("vectorAlignmentDrawing");
  for (const auto &entry : entries) frames->addItem(entry.drawing);
  layout->addWidget(frames, 1, 0, 1, 2);
  layout->addWidget(new QLabel(tr("Original"), &dialog), 2, 0);
  layout->addWidget(new QLabel(tr("Proposed"), &dialog), 2, 1);
  auto *before = new Swatch(&dialog);
  auto *after = new Swatch(&dialog);
  before->setObjectName("vectorAlignmentBefore");
  after->setObjectName("vectorAlignmentAfter");
  before->setAccessibleName(tr("Original drawing"));
  after->setAccessibleName(tr("Proposed drawing"));
  frames->setAccessibleName(tr("Drawing to review"));
  layout->addWidget(before, 3, 0);
  layout->addWidget(after, 3, 1);
  auto *note = new QLabel(tr("Review each changed drawing. Apply uses exactly these positions "
      "in one undo step; Cancel keeps every original drawing."), &dialog);
  note->setWordWrap(true);
  layout->addWidget(note, 4, 0, 1, 2);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply |
                                      QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Apply)->setObjectName("vectorAlignmentApply");
  buttons->button(QDialogButtonBox::Cancel)->setObjectName("vectorAlignmentCancel");
  buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
  QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
                   &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons, 5, 0, 1, 2);
  auto refresh = [&](int index) {
    const auto &entry = entries.at(index);
    const TRectD bounds = entry.before->getBBox() + entry.after->getBBox();
    before->setImage(entry.before, bounds);
    after->setImage(entry.after, bounds);
  };
  QObject::connect(frames, QOverload<int>::of(&QComboBox::currentIndexChanged),
                   &dialog, refresh);
  refresh(0);
  const bool accepted = dialog.exec() == QDialog::Accepted;
  return accepted && !invalidated;
}
} // namespace VectorAlignmentPreview
