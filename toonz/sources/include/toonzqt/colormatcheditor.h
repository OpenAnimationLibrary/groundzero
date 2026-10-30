#pragma once
#include "tcommon.h"
#include <QImage>
#include <QString>
class QWidget;
class TPaletteHandle;
#undef DVAPI
#ifdef TOONZQT_EXPORTS
#define DVAPI DV_EXPORT_API
#else
#define DVAPI DV_IMPORT_API
#endif

// Returns a validated scene-owned profile; Cancel leaves the argument unchanged.
DVAPI bool editColorMatchProfile(QWidget *parent, QString &profile,
                                 const QImage &reference = QImage(),
                                 const QImage &source = QImage());
DVAPI void matchPaletteToReference(QWidget *parent, TPaletteHandle *handle,
                                    const QImage &reference);
DVAPI bool editNeuralStyleProfile(QWidget *parent, QString &profile,
                                  const QImage &reference = QImage(),
                                  const QImage &source = QImage());
DVAPI void createNeuralStyleProfile(QWidget *parent, const QImage &reference);
