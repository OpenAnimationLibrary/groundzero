#include "toonzqt/seexprrangeviewer.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace SeExprRangeViewer {
namespace {
constexpr int MaxSamples = 512;
constexpr std::size_t MaxComponents = 16;

class RangeGraph final : public QWidget {
public:
  Result result;

  explicit RangeGraph(QWidget *parent) : QWidget(parent) {
    setObjectName("seExprRangeGraph");
    setAccessibleName(QObject::tr("Expression values over the frame range"));
    setMinimumSize(480, 200);
  }

  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().base());
    const QRectF plot = QRectF(rect()).adjusted(55, 12, -12, -32);
    painter.setPen(palette().mid().color());
    painter.drawRect(plot);
    if (result.samples.isEmpty()) return;

    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -minimum;
    std::size_t components = 0;
    for (const auto &sample : result.samples) {
      if (!sample.error.isEmpty()) continue;
      components = std::max(components, sample.values.size());
      for (double value : sample.values) {
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
      }
    }
    if (!components || !std::isfinite(minimum) || !std::isfinite(maximum)) return;
    const bool constant = minimum == maximum;
    const double scale = std::max({1.0, std::abs(minimum), std::abs(maximum)});
    const double lower = minimum / scale, upper = maximum / scale;
    const double first = result.samples.first().frame;
    const double last = result.samples.last().frame;
    const double frameSpan = last == first ? 1 : last - first;
    painter.setPen(palette().text().color());
    painter.drawText(QRectF(0, plot.top() - 7, plot.left() - 5, 20),
                     Qt::AlignRight, QString::number(maximum, 'g', 6));
    painter.drawText(QRectF(0, plot.bottom() - 10, plot.left() - 5, 20),
                     Qt::AlignRight, QString::number(minimum, 'g', 6));
    painter.drawText(QRectF(plot.left(), plot.bottom() + 5, 100, 20),
                     Qt::AlignLeft, QString::number(first, 'g', 8));
    painter.drawText(QRectF(plot.right() - 100, plot.bottom() + 5, 100, 20),
                     Qt::AlignRight, QString::number(last, 'g', 8));
    const QColor base = palette().highlight().color();
    for (std::size_t component = 0; component < components; ++component) {
      QPainterPath path;
      bool connected = false;
      for (const auto &sample : result.samples) {
        if (!sample.error.isEmpty() || component >= sample.values.size()) {
          connected = false;
          continue;
        }
        const double x = plot.left() + (sample.frame - first) / frameSpan * plot.width();
        const double normalized = constant ? 0.5 :
            (sample.values[component] / scale - lower) / (upper - lower);
        const QPointF point(x, plot.bottom() - normalized * plot.height());
        if (connected) path.lineTo(point);
        else path.moveTo(point);
        connected = true;
        if (result.samples.size() == 1) painter.drawEllipse(point, 2, 2);
      }
      QColor color = base;
      if (components > 1)
        color.setHsv((std::max(0, base.hue()) + int(component) * 47) % 360,
                     std::max(100, base.saturation()), std::max(100, base.value()));
      painter.setPen(QPen(color, 1.5));
      painter.drawPath(path);
    }
  }
};
}  // namespace

Result evaluate(const Request &request, const Evaluator &evaluator,
                const std::function<bool()> &cancelled,
                const std::function<void(int, int)> &progress) {
  Result result;
  if (!evaluator || !std::isfinite(request.start) || !std::isfinite(request.end) ||
      !std::isfinite(request.step) || request.end < request.start || request.step <= 0) {
    result.error = QObject::tr("Choose a finite increasing frame range and a positive step.");
    return result;
  }
  const long double span = static_cast<long double>(request.end) - request.start;
  const long double intervals = span / request.step;
  if (!std::isfinite(intervals) || intervals > MaxSamples - 1 + 1e-9L) {
    result.error = QObject::tr("The preview is limited to %1 samples. Increase the step or shorten the range.").arg(MaxSamples);
    return result;
  }
  const int count = static_cast<int>(std::floor(intervals + 1e-9L)) + 1;
  result.samples.reserve(count);
  if (progress) progress(0, count);
  for (int index = 0; index < count; ++index) {
    if (cancelled && cancelled()) {
      result.cancelled = true;
      break;
    }
    Sample sample;
    sample.frame = static_cast<double>(static_cast<long double>(request.start) +
                                       static_cast<long double>(index) * request.step);
    try {
      if (!evaluator(sample.frame, sample.values, sample.error)) {
        if (sample.error.isEmpty()) sample.error = QObject::tr("Expression evaluation failed.");
      } else if (sample.values.empty() || sample.values.size() > MaxComponents ||
                 !std::all_of(sample.values.begin(), sample.values.end(),
                              [](double value) { return std::isfinite(value); })) {
        sample.error = QObject::tr("The preview requires one to sixteen finite numeric values.");
      }
    } catch (const std::exception &error) {
      sample.error = QString::fromUtf8(error.what());
    } catch (...) {
      sample.error = QObject::tr("Native expression evaluation failed.");
    }
    if (!sample.error.isEmpty()) sample.values.clear();
    result.samples.push_back(std::move(sample));
    if (progress) progress(index + 1, count);
  }
  return result;
}

void show(QWidget *parent, const Evaluator &evaluator, double start, double end,
          double step) {
  // The host may close while processEvents services cancellation. A heap
  // dialog guarded by QPointer avoids deleting a stack child with its parent.
  QPointer<QDialog> dialog = new QDialog(parent);
  dialog->setObjectName("seExprRangeDialog");
  dialog->setWindowTitle(QObject::tr("Expression Range Preview"));
  auto *layout = new QVBoxLayout(dialog);
  auto *description = new QLabel(QObject::tr(
      "Preview the current draft over a frame range. This does not apply changes to the scene."), dialog);
  description->setWordWrap(true);
  layout->addWidget(description);
  auto *form = new QFormLayout;
  layout->addLayout(form);
  const auto field = [&](const QString &name, double value, bool positive) {
    auto *control = new QDoubleSpinBox(dialog);
    control->setObjectName(name);
    control->setDecimals(6);
    control->setRange(positive ? .000001 : -1000000000, 1000000000);
    control->setValue(std::isfinite(value) ? value : 1);
    return control;
  };
  auto *from = field("seExprRangeStart", start, false);
  auto *to = field("seExprRangeEnd", end, false);
  auto *increment = field("seExprRangeStep", step, true);
  form->addRow(QObject::tr("First Frame"), from);
  form->addRow(QObject::tr("Last Frame"), to);
  form->addRow(QObject::tr("Step"), increment);
  auto *preview = new QPushButton(QObject::tr("Preview Range"), dialog);
  preview->setObjectName("seExprRangeRun");
  layout->addWidget(preview);
  auto *graph = new RangeGraph(dialog);
  layout->addWidget(graph);
  auto *table = new QTableWidget(dialog);
  table->setObjectName("seExprRangeSamples");
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->horizontalHeader()->setStretchLastSection(true);
  layout->addWidget(table);
  auto *status = new QLabel(dialog);
  status->setObjectName("seExprRangeStatus");
  status->setWordWrap(true);
  status->setTextFormat(Qt::PlainText);
  layout->addWidget(status);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
  layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::rejected, dialog.data(), &QDialog::reject);
  QObject::connect(preview, &QPushButton::clicked, dialog.data(),
                   [=, evaluator = Evaluator(evaluator)] {
    const Request request{from->value(), to->value(), increment->value()};
    QPointer<QProgressDialog> progress = new QProgressDialog(
        QObject::tr("Evaluating expression samples…"), QObject::tr("Cancel"),
        0, MaxSamples, dialog);
    progress->setObjectName("seExprRangeProgress");
    // Modal QProgressDialog::setValue pumps events internally. Service events
    // only at the guarded boundary below so owner deletion is always observed.
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    preview->setEnabled(false);
    const auto samples = evaluate(request, evaluator,
        [&] { return !dialog || !dialog->isVisible() || !progress || progress->wasCanceled(); },
        [&](int completed, int count) {
          if (!dialog || !progress) return;
          progress->setMaximum(count);
          progress->setValue(completed);
          QApplication::processEvents(QEventLoop::AllEvents, 5);
        });
    if (progress) delete progress;
    if (!dialog || !dialog->isVisible()) return;
    preview->setEnabled(true);
    graph->result = samples;
    graph->update();
    std::size_t components = 0;
    int failures = 0;
    for (const auto &sample : samples.samples) {
      components = std::max(components, sample.values.size());
      if (!sample.error.isEmpty()) ++failures;
    }
    QStringList columns{QObject::tr("Frame")};
    for (std::size_t component = 0; component < components; ++component)
      columns << QObject::tr("Component %1").arg(int(component) + 1);
    columns << QObject::tr("Status");
    table->clear();
    table->setColumnCount(columns.size());
    table->setHorizontalHeaderLabels(columns);
    table->setRowCount(samples.samples.size());
    for (int row = 0; row < samples.samples.size(); ++row) {
      const auto &sample = samples.samples[row];
      table->setItem(row, 0, new QTableWidgetItem(QString::number(sample.frame, 'g', 16)));
      for (std::size_t component = 0; component < sample.values.size(); ++component)
        table->setItem(row, int(component) + 1,
                       new QTableWidgetItem(QString::number(sample.values[component], 'g', 16)));
      table->setItem(row, int(components) + 1, new QTableWidgetItem(
          sample.error.isEmpty() ? QObject::tr("Valid") : sample.error));
    }
    if (!samples.error.isEmpty()) status->setText(samples.error);
    else if (samples.cancelled)
      status->setText(QObject::tr("Cancelled after %1 samples; %2 failed frames.")
                      .arg(samples.samples.size()).arg(failures));
    else status->setText(QObject::tr("Evaluated %1 samples; %2 failed frames.")
                          .arg(samples.samples.size()).arg(failures));
  });
  dialog->resize(720, 700);
  dialog->exec();
  if (dialog) delete dialog;
}
}  // namespace SeExprRangeViewer
