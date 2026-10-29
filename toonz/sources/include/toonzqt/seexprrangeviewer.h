#pragma once

#include "tcommon.h"

#include <QString>
#include <QVector>

#include <functional>
#include <vector>

class QWidget;

#undef DVAPI
#ifdef TOONZQT_EXPORTS
#define DVAPI DV_EXPORT_API
#else
#define DVAPI DV_IMPORT_API
#endif

namespace SeExprRangeViewer {
using Evaluator = std::function<bool(double, std::vector<double> &, QString &)>;

struct Request {
  double start = 1;
  double end = 48;
  double step = 1;
};

struct Sample {
  double frame = 0;
  std::vector<double> values;
  QString error;
};

struct Result {
  QVector<Sample> samples;
  QString error;
  bool cancelled = false;
};

// Evaluate an immutable draft context. Invalid requests fail before invoking
// the host; failed frames remain visible and do not discard successful frames.
DVAPI Result evaluate(const Request &request, const Evaluator &evaluator,
                      const std::function<bool()> &cancelled = {},
                      const std::function<void(int, int)> &progress = {});

DVAPI void show(QWidget *parent, const Evaluator &evaluator, double start = 1,
                double end = 48, double step = 1);
}  // namespace SeExprRangeViewer
