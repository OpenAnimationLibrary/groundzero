#include "spacingchartpane.h"

#include "motionarcs.h"
#include "tapp.h"

#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

//=============================================================================
// SpacingChart
//-----------------------------------------------------------------------------

SpacingChart::SpacingChart(QWidget *parent)
    : QWidget(parent), m_currentFrame(-1) {
  setMinimumSize(200, 110);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

//-----------------------------------------------------------------------------

void SpacingChart::setTicks(const std::vector<MotionArc::Tick> &ticks,
                            int currentFrame) {
  m_ticks        = ticks;
  m_currentFrame = currentFrame;
  update();
}

//-----------------------------------------------------------------------------

void SpacingChart::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QColor fg = palette().color(QPalette::WindowText);
  const QColor highlight(255, 80, 60);

  if (m_ticks.size() < 2) {
    p.setPen(fg);
    p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
               tr("The current column does not move between these frames."));
    return;
  }

  const int margin = 16;
  const double x0 = margin, x1 = width() - margin;
  const double w = std::max(1.0, x1 - x0);
  // The chart in the upper half, the strip of the distances below it
  const double lineY  = height() * 0.36;
  const double stripY = height() - margin;
  const double stripH = height() * 0.30;

  // The chart: the line from the first frame to the last, a faint mark at
  // the half way, then a tick per frame (long on the keys)
  p.setPen(QPen(fg, 1));
  p.drawLine(QPointF(x0, lineY), QPointF(x1, lineY));
  QColor faint = fg;
  faint.setAlpha(90);
  p.setPen(QPen(faint, 1, Qt::DashLine));
  p.drawLine(QPointF(x0 + 0.5 * w, lineY - 18),
             QPointF(x0 + 0.5 * w, lineY + 18));

  const QFontMetrics fm(font());
  for (const MotionArc::Tick &tick : m_ticks) {
    const double x     = x0 + tick.t * w;
    const bool current = tick.frame == m_currentFrame;
    const double h     = tick.key ? 14 : 7;
    p.setPen(QPen(current ? highlight : fg, tick.key || current ? 2 : 1));
    p.drawLine(QPointF(x, lineY - h), QPointF(x, lineY + h));
    if (tick.key || current) {
      const QString text = QString::number(tick.frame + 1);
      p.drawText(QPointF(x - 0.5 * fm.horizontalAdvance(text), lineY - h - 3),
                 text);
    }
  }

  // The strip: a bar per frame, as high as the distance from the previous
  // frame, in the colour of its speed
  double top = 0.0;
  for (const MotionArc::Tick &tick : m_ticks) top = std::max(top, tick.spacing);
  const int n       = int(m_ticks.size());
  const double step = w / std::max(1, n - 1);
  const double barW = std::max(1.0, std::min(12.0, 0.6 * step));
  p.setPen(QPen(faint, 1));
  p.drawLine(QPointF(x0, stripY), QPointF(x1, stripY));
  for (int i = 0; i < n; ++i) {
    const MotionArc::Tick &tick = m_ticks[i];
    const double v              = top > 0.0 ? tick.spacing / top : 0.0;
    int r, g, b;
    MotionArc::rampColor(v, r, g, b);
    const double x = x0 + i * step;
    const QRectF bar(x - 0.5 * barW, stripY - v * stripH, barW, v * stripH);
    p.fillRect(bar, QColor(r, g, b));
    if (tick.frame == m_currentFrame) {
      p.setPen(QPen(highlight, 1));
      p.drawRect(bar.adjusted(-1, -1, 1, 1));
    }
  }
}

//=============================================================================
// SpacingChartPane
//-----------------------------------------------------------------------------

SpacingChartPane::SpacingChartPane(QWidget *parent, Qt::WindowFlags flags)
    : QFrame(parent, flags) {
  m_columnLabel = new QLabel(this);
  m_infoLabel   = new QLabel(this);
  m_keysAround  = new QCheckBox(tr("Keys Around the Frame"), this);
  m_keysAround->setChecked(true);
  m_keysAround->setToolTip(
      tr("Chart the spacing from the key at or before the current frame to "
         "the next key"));
  m_from = new QSpinBox(this);
  m_to   = new QSpinBox(this);
  for (QSpinBox *box : {m_from, m_to}) {
    box->setRange(1, 99999);
    box->setEnabled(false);
  }
  m_chart = new SpacingChart(this);

  QHBoxLayout *range = new QHBoxLayout();
  range->addWidget(m_keysAround);
  range->addStretch(1);
  range->addWidget(new QLabel(tr("From:"), this));
  range->addWidget(m_from);
  range->addWidget(new QLabel(tr("To:"), this));
  range->addWidget(m_to);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->addWidget(m_columnLabel);
  layout->addLayout(range);
  layout->addWidget(m_chart, 1);
  layout->addWidget(m_infoLabel);

  connect(m_keysAround, &QCheckBox::toggled, this, [this](bool on) {
    m_from->setEnabled(!on);
    m_to->setEnabled(!on);
    refresh();
  });
  connect(m_from, QOverload<int>::of(&QSpinBox::valueChanged), this,
          &SpacingChartPane::refresh);
  connect(m_to, QOverload<int>::of(&QSpinBox::valueChanged), this,
          &SpacingChartPane::refresh);
}

//-----------------------------------------------------------------------------

void SpacingChartPane::showEvent(QShowEvent *) {
  TApp *app = TApp::instance();
  connect(app->getCurrentScene(), SIGNAL(sceneSwitched()), this,
          SLOT(refresh()));
  // Also when the tracked point of the Motion Arcs changes
  connect(app->getCurrentScene(), SIGNAL(sceneChanged()), this,
          SLOT(refresh()));
  connect(app->getCurrentXsheet(), SIGNAL(xsheetSwitched()), this,
          SLOT(refresh()));
  connect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
          SLOT(refresh()));
  connect(app->getCurrentColumn(), SIGNAL(columnIndexSwitched()), this,
          SLOT(refresh()));
  connect(app->getCurrentObject(), SIGNAL(objectChanged(bool)), this,
          SLOT(refresh()));
  connect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
          SLOT(refresh()));
  refresh();
}

//-----------------------------------------------------------------------------

void SpacingChartPane::hideEvent(QHideEvent *) {
  TApp *app = TApp::instance();
  disconnect(app->getCurrentScene(), SIGNAL(sceneSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentScene(), SIGNAL(sceneChanged()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentXsheet(), SIGNAL(xsheetSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentColumn(), SIGNAL(columnIndexSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentObject(), SIGNAL(objectChanged(bool)), this,
             SLOT(refresh()));
  disconnect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
             SLOT(refresh()));
}

//-----------------------------------------------------------------------------

void SpacingChartPane::refresh() {
  if (!isVisible()) return;
  TApp *app       = TApp::instance();
  TXsheet *xsh    = app->getCurrentXsheet()->getXsheet();
  const int col   = app->getCurrentColumn()->getColumnIndex();
  const int frame = app->getCurrentFrame()->getFrame();
  if (!xsh || col < 0) {
    m_columnLabel->setText(tr("No current column."));
    m_infoLabel->clear();
    m_chart->setTicks({}, frame);
    return;
  }
  m_columnLabel->setText(
      tr("Column: %1")
          .arg(QString::fromStdString(
              xsh->getStageObject(TStageObjectId::ColumnId(col))->getName())));

  const std::vector<MotionArc::Sample> samples = MotionArcs::columnSamples(col);
  int f0 = 0, f1 = 0;
  if (m_keysAround->isChecked()) {
    if (MotionArc::keysAround(samples, frame, f0, f1)) {
      QSignalBlocker fromBlocker(m_from), toBlocker(m_to);
      m_from->setValue(f0 + 1);
      m_to->setValue(f1 + 1);
    }
  } else
    f0 = m_from->value() - 1, f1 = m_to->value() - 1;

  const std::vector<MotionArc::Tick> ticks = MotionArc::chart(samples, f0, f1);
  m_chart->setTicks(ticks, frame);
  if (ticks.size() < 2)
    m_infoLabel->clear();
  else {
    int keys = 0;
    for (const MotionArc::Tick &tick : ticks) keys += tick.key ? 1 : 0;
    m_infoLabel->setText(tr("Frames %1 to %2: %3 frames, %4 keys")
                             .arg(ticks.front().frame + 1)
                             .arg(ticks.back().frame + 1)
                             .arg(int(ticks.size()))
                             .arg(keys));
  }
}
