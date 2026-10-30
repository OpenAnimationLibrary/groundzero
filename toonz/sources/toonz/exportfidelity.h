#pragma once
#include <QStringList>
class QWidget;
namespace ExportFidelity {
enum Choice { Cancel, SupportedAssets, RenderedScene };
// These dialogs run before any output is written. Closing is always Cancel.
bool confirmLottie(const QStringList &losses, QWidget *parent = nullptr);
Choice chooseAnimatedSvg(QWidget *parent = nullptr);
}
