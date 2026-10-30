#pragma once
#include <QImage>

namespace NeuralTemporal {
// All inputs share the same fixed model canvas. Coverage is original source
// alpha stored as grayscale. The current drawing's alpha is restored by the FX,
// never taken from history. False means cancellation; result is unchanged.
bool stabilize(const QImage &previousSource, const QImage &currentSource,
               const QImage &previousCoverage, const QImage &currentCoverage,
               const QImage &previousStyled, const QImage &freshStyled,
               double amount, QImage &result, const int *cancel = nullptr);
}
