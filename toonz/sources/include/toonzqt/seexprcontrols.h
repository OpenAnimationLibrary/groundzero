#pragma once

#include "tcommon.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>

class QWidget;
namespace TSyntax {
class Grammar;
}

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
using AnimationCurveLinkResolver = std::function<bool(const QString &, QJsonObject &, QString &)>;
DVAPI bool editAnimationCurve(QWidget *parent, QJsonObject &control,
                              const AnimationCurveLinkResolver &resolver = {});
DVAPI QJsonObject defaultAnimationCurve();
DVAPI bool validateAnimationCurve(const QJsonObject &control,
                                  QString *error = nullptr);
// Existing calls retain their exact lookup and surrounding source. With no
// call, this explicit authoring action returns a new complete expression.
DVAPI bool editInlineAnimationCurve(QWidget *parent, QString &source,
                                    QString *error = nullptr,
                                    bool createIfMissing = true,
                                    const AnimationCurveLinkResolver &resolver = {});
DVAPI bool refreshAnimationCurveLink(const TSyntax::Grammar *grammar,
                                     const QString &reference,
                                     QJsonObject &control, QString &error);
DVAPI QJsonArray spectrumPoints(const QJsonObject &parameters,
                               int sampleCount = 64);
DVAPI QJsonObject defaultSpectrumParameters();
}  // namespace SeExprControls
