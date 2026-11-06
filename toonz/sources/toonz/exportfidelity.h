#pragma once
#include <QStringList>
#ifdef BUILD_UI_PORT_TESTS
#include <functional>
#endif
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
#ifdef BUILD_UI_PORT_TESTS
// Deterministic lifecycle observation for the disposable native fixture. The
// queued callback runs before the exporter dispatches the renderer's event.
struct SvgRenderTestHooks {
  std::function<void()> queued;
  std::function<void(bool)> cancelStarted;
  std::function<void()> completed;
  std::function<void(bool)> cancelDrained;
};
void setSvgRenderTestHooks(SvgRenderTestHooks *hooks);
#endif
}
