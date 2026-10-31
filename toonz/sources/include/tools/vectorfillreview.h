#pragma once

#include "tvectorimage.h"
#include "tregion.h"
#include "tgl.h"
#include "toonzqt/planeviewer.h"
#include "tools/tool.h"
#include "tools/vectormodalguard.h"
#include "tools/toolhandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/txshlevelhandle.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <algorithm>
#include <vector>

// These snapshots belong to the review, never to a live drawing. Opening or
// repainting the dialog must not recompute regions in the document itself.
namespace VectorFillReview {
struct Entry {
  QString drawing;
  TVectorImageP before, after;
  Entry(const QString &name, const TVectorImageP &oldImage,
        const TVectorImageP &newImage)
      : drawing(name), before(oldImage->clone()),
        after(newImage->clone()) {}
};

inline int filledRegions(const TRegion *region) {
  int result = region->getStyle() != 0 ? 1 : 0;
  for (int i = 0; i < region->getSubregionCount(); ++i)
    result += filledRegions(region->getSubregion(i));
  return result;
}
inline int filledRegions(const TVectorImageP &image) {
  int result = 0;
  for (int i = 0; i < int(image->getRegionCount()); ++i)
    result += filledRegions(image->getRegion(i));
  return result;
}

class Swatch final : public PlaneViewer {
  TVectorImageP m_image;
  TRectD m_bounds;
public:
  explicit Swatch(QWidget *parent) : PlaneViewer(parent) {
    setMinimumSize(270, 240);
    setBgColor(TPixel32(230, 230, 230), TPixel32(255, 255, 255));
  }
  void setImage(const TVectorImageP &image, const TRectD &bounds) {
    m_image = image;
    m_bounds = bounds;
    update();
  }
  void paintGL() override {
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

inline bool review(const std::vector<Entry> &entries) {
  if (entries.empty()) return true;
  VectorModalGuard owner(QApplication::activeWindow());
  QDialog dialog;
  bool invalidated = false;
  const auto invalidate = [&] { invalidated = true; dialog.reject(); };
  if (auto *app = TTool::getApplication()) {
    QObject::connect(app->getCurrentScene(), &TSceneHandle::sceneSwitched,
                     &dialog, invalidate);
    QObject::connect(app->getCurrentLevel(), &TXshLevelHandle::xshLevelSwitched,
                     &dialog, invalidate);
    QObject::connect(app->getCurrentFrame(), &TFrameHandle::frameSwitched,
                     &dialog, invalidate);
    QObject::connect(app->getCurrentColumn(), &TColumnHandle::columnIndexSwitched,
                     &dialog, invalidate);
    QObject::connect(app->getCurrentTool(), &ToolHandle::toolSwitched,
                     &dialog, invalidate);
  }
  dialog.setObjectName("vectorFillReview");
  dialog.setWindowTitle(QApplication::translate("VectorFillReview", "Review Fills"));
  auto *layout = new QGridLayout(&dialog);
  auto *frames = new QComboBox(&dialog);
  frames->setObjectName("vectorFillFrame");
  for (const auto &entry : entries) frames->addItem(entry.drawing);
  layout->addWidget(frames, 0, 0, 1, 2);
  auto *beforeTitle = new QLabel(&dialog);
  auto *afterTitle = new QLabel(&dialog);
  layout->addWidget(beforeTitle, 1, 0);
  layout->addWidget(afterTitle, 1, 1);
  auto *before = new Swatch(&dialog);
  auto *after = new Swatch(&dialog);
  layout->addWidget(before, 2, 0);
  layout->addWidget(after, 2, 1);
  auto *note = new QLabel(QApplication::translate("VectorFillReview",
      "Inspect the filled areas in every drawing. Applying accepts the displayed "
      "topology; restoring keeps the original geometry and fills."), &dialog);
  note->setWordWrap(true);
  layout->addWidget(note, 3, 0, 1, 2);
  auto *buttons = new QDialogButtonBox(&dialog);
  auto *apply = buttons->addButton(QApplication::translate("VectorFillReview", "Apply"),
                                   QDialogButtonBox::AcceptRole);
  auto *restore = buttons->addButton(QApplication::translate("VectorFillReview", "Restore Original"),
                                     QDialogButtonBox::RejectRole);
  apply->setObjectName("vectorFillApply");
  restore->setObjectName("vectorFillRestore");
  restore->setDefault(true);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons, 4, 0, 1, 2);
  auto refresh = [&](int index) {
    const Entry &entry = entries.at(index);
    const TRectD bounds = entry.before->getBBox() + entry.after->getBBox();
    before->setImage(entry.before, bounds);
    after->setImage(entry.after, bounds);
    beforeTitle->setText(QApplication::translate("VectorFillReview", "Original: %1 filled regions")
                            .arg(filledRegions(entry.before)));
    afterTitle->setText(QApplication::translate("VectorFillReview", "Proposed: %1 filled regions")
                           .arg(filledRegions(entry.after)));
  };
  QObject::connect(frames, QOverload<int>::of(&QComboBox::currentIndexChanged),
                   &dialog, refresh);
  refresh(0);
  const bool accepted = owner.exec(dialog) == QDialog::Accepted;
  // A queued Apply must not revive a review rejected by a context switch,
  // including a round trip to the same frame before the click is delivered.
  return accepted && !invalidated;
}
}  // namespace VectorFillReview
