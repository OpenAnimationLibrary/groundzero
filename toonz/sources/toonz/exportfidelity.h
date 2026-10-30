#pragma once
#include <QStringList>
class QWidget;
class TXsheet;
namespace ExportFidelity {
enum Choice { Cancel, SupportedAssets, RenderedScene };
struct SvgCapabilities {
  int profiledStrokes = 0;
  QStringList profiledDrawings;
  QStringList assetBlockers;
};
// Read-only preflight, deduplicated by actual level/drawing identity across
// nested xsheets. Incomplete inspection is an explicit asset-mode blocker.
SvgCapabilities inspectAnimatedSvg(TXsheet *sheet);
// These dialogs run before any output is written. Closing is always Cancel.
bool confirmLottie(const QStringList &losses, QWidget *parent = nullptr);
Choice chooseAnimatedSvg(QWidget *parent = nullptr);
Choice chooseAnimatedSvg(const SvgCapabilities &capabilities,
                         QWidget *parent = nullptr);
}
