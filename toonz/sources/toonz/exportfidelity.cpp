#include "exportfidelity.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "tstrokeopacitybounds.h"
#include "tvectorimage.h"
#include "tpalette.h"
#include <map>
#include <set>
#include <vector>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

namespace {
int choose(const QString &message, bool renderedChoice, QWidget *parent,
           bool assetsSupported = true) {
  QDialog dialog(parent);
  dialog.setObjectName("exportFidelityDialog");
  dialog.setWindowTitle(QObject::tr("Export Fidelity"));
  auto *layout = new QVBoxLayout(&dialog);
  auto *text = new QTextEdit(&dialog);
  text->setObjectName("exportFidelityDetails");
  text->setAccessibleName(QObject::tr("Export limitations and choices"));
  text->setReadOnly(true);
  text->setPlainText(message);
  text->setMinimumSize(520, 230);
  layout->addWidget(text);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Cancel)->setObjectName("cancelExportFidelity");
  buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
  auto *assets = buttons->addButton(QObject::tr("Export Supported Content"), QDialogButtonBox::ActionRole);
  assets->setObjectName("exportSupportedContent");
  assets->setEnabled(assetsSupported);
  if (!assetsSupported) assets->setToolTip(QObject::tr("Use Render Scene for the unsupported drawing styles listed above."));
  QObject::connect(assets, &QPushButton::clicked, &dialog, [&]() { dialog.done(ExportFidelity::SupportedAssets); });
  if (renderedChoice) {
    auto *render = buttons->addButton(QObject::tr("Render Scene"), QDialogButtonBox::ActionRole);
    render->setObjectName("renderSceneForExport");
    QObject::connect(render, &QPushButton::clicked, &dialog, [&]() { dialog.done(ExportFidelity::RenderedScene); });
  }
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  return dialog.exec();
}
}

bool ExportFidelity::confirmLottie(const QStringList &losses, QWidget *parent) {
  if (losses.isEmpty()) return true;
  const QString message = QObject::tr(
      "This Lottie export cannot preserve:\n- %1\n\n"
      "Export the supported content, or cancel to adjust the scene. "
      "For rendered appearance, use Animated SVG with Render Scene Before Export.")
      .arg(losses.join("\n- "));
  return choose(message, false, parent) == SupportedAssets;
}

ExportFidelity::SvgCapabilities ExportFidelity::inspectAnimatedSvg(TXsheet *sheet) {
  SvgCapabilities result;
  std::vector<TXsheet *> pending{sheet};
  std::set<TXsheet *> visited;
  std::map<TXshSimpleLevel *, std::set<TFrameId>> drawings;
  int inspectedCells = 0;
  while (!pending.empty()) {
    TXsheet *current = pending.back();
    pending.pop_back();
    if (!current || !visited.insert(current).second) continue;
    for (int column = 0; column < current->getColumnCount(); ++column) {
      auto *track = current->getColumn(column);
      if (!track || !track->getCellColumn() || track->getSoundColumn() ||
          !track->isPreviewVisible()) continue;
      int first = 0, last = -1;
      if (!track->getRange(first, last)) continue;
      for (int row = first; row <= last; ++row) {
        if (++inspectedCells > 2000000) {
          result.assetBlockers.append(QObject::tr("The asset inspection limit was reached. Render the scene or export a smaller scene."));
          return result;
        }
        const TXshCell cell = current->getCell(row, column);
        if (auto *child = cell.getChildLevel()) {
          pending.push_back(child->getXsheet());
          continue;
        }
        auto *level = cell.getSimpleLevel();
        if (!level || level->getType() != PLI_XSHLEVEL ||
            !drawings[level].insert(cell.getFrameId()).second) continue;
        const QString name = QString::fromStdWString(level->getName()) +
                             QStringLiteral(" [") + QString::fromStdString(cell.getFrameId().expand()) + QStringLiteral("]");
        try {
          TVectorImageP image = level->getFrame(cell.getFrameId(), false);
          if (!image || !image->getPalette()) {
            result.assetBlockers.append(QObject::tr("%1 could not be inspected.").arg(name));
            continue;
          }
          bool profiled = false, unsupported = false;
          for (unsigned int i = 0; i < image->getStrokeCount(); ++i) {
            const TStroke *stroke = image->getStroke(i);
            if (stroke->getOpacityProfile().empty()) continue;
            profiled = true;
            ++result.profiledStrokes;
            const auto *style = image->getPalette()->getStyle(stroke->getStyle());
            TRectD bounds;
            if (!style || (style->isStrokeStyle() &&
                !StrokeOpacity::renderBounds(*style, *stroke, bounds))) unsupported = true;
          }
          if (profiled) result.profiledDrawings.append(name);
          if (unsupported)
            result.assetBlockers.append(QObject::tr("%1 has profiled stroke styles without verified SVG bounds.").arg(name));
        } catch (...) {
          result.assetBlockers.append(QObject::tr("%1 could not be inspected.").arg(name));
        }
      }
    }
  }
  return result;
}

ExportFidelity::Choice ExportFidelity::chooseAnimatedSvg(QWidget *parent) {
  return chooseAnimatedSvg(SvgCapabilities(), parent);
}

ExportFidelity::Choice ExportFidelity::chooseAnimatedSvg(
    const SvgCapabilities &capabilities, QWidget *parent) {
  QString message = QObject::tr(
      "Asset-based SVG does not reproduce the complete scene renderer. "
      "Camera and rig animation, deformations, compositing, effects and special "
      "vector styles may differ. Audio is not exported in either mode.\n\n"
      "Render Scene preserves the rendered picture as PNG frames. Export Supported "
      "Content keeps the selected vector/raster asset mode.");
  if (capabilities.profiledStrokes) {
    message += QObject::tr(
        "\n\n%1 strokes in %2 drawings use opacity envelopes. In vector asset mode, "
        "these strokes become embedded PNG images; the remaining paths stay vectors. "
        "This mixed SVG preserves their rendered appearance at the selected raster resolution, "
        "but cannot be imported as editable vectors. Keep the original PLI 150.1 drawings "
        "to edit the envelopes.\nDrawings: %3")
        .arg(capabilities.profiledStrokes).arg(capabilities.profiledDrawings.size())
        .arg(capabilities.profiledDrawings.join(QStringLiteral(", ")));
  }
  if (!capabilities.assetBlockers.isEmpty())
    message += QObject::tr("\n\nAsset export is unavailable:\n- %1\nChoose Render Scene for the full camera canvas, or Cancel.")
        .arg(capabilities.assetBlockers.join(QStringLiteral("\n- ")));
  const int result = choose(message, true, parent, capabilities.assetBlockers.isEmpty());
  return result == RenderedScene ? RenderedScene
       : result == SupportedAssets ? SupportedAssets : Cancel;
}
