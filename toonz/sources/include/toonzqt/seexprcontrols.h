#pragma once

#include "tcommon.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

class QWidget;

#undef DVAPI
#ifdef TOONZQT_EXPORTS
#define DVAPI DV_EXPORT_API
#else
#define DVAPI DV_IMPORT_API
#endif

namespace SeExprControls {
struct ImportResult {
  QString source;
  QJsonArray controls;
  QStringList diagnostics;
};

// Import literal assignments and upstream range annotations into stable
// native controls. Expressions outside the recognized literal forms survive
// unchanged; errors are returned instead of guessing at their semantics.
DVAPI ImportResult importAnnotations(const QString &source,
                                    const QJsonArray &existing = {},
                                    const QString &sourceNamespace = {});

// Native artist controls return a complete draft control; callers apply it in
// the same transaction as their formula and other control changes.
DVAPI bool editSwatches(QWidget *parent, QJsonObject &control);
DVAPI bool editSpectrum(QWidget *parent, QJsonObject &control);
DVAPI QJsonArray spectrumPoints(const QJsonObject &parameters,
                               int sampleCount = 64);
DVAPI QJsonObject defaultSpectrumParameters();
}  // namespace SeExprControls
