#include "exportfidelity.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

namespace {
int choose(const QString &message, bool renderedChoice, QWidget *parent) {
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

ExportFidelity::Choice ExportFidelity::chooseAnimatedSvg(QWidget *parent) {
  const QString message = QObject::tr(
      "Asset-based SVG does not reproduce the complete scene renderer. "
      "Camera and rig animation, deformations, compositing, effects and special "
      "vector styles may differ. Audio is not exported in either mode.\n\n"
      "Render Scene preserves the rendered picture as PNG frames. Export Supported "
      "Content keeps the selected vector/raster asset mode.");
  const int result = choose(message, true, parent);
  return result == RenderedScene ? RenderedScene
       : result == SupportedAssets ? SupportedAssets : Cancel;
}
