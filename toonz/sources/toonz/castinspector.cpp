#include "castinspector.h"
#include "castbrowseractions.h"
#include "tapp.h"
#include "toonz/levelset.h"
#include "toonz/levelproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshpalettelevel.h"
#include "toonz/txshsoundlevel.h"
#include "toonzqt/icongenerator.h"
#include "tpalette.h"
#include "tsound.h"
#include "tundo.h"
#include <QElapsedTimer>
#include <QHeaderView>
#include <QHideEvent>
#include <QShowEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace {
QString tr(const char *text) { return QObject::tr(text); }
ToonzScene *scene() { return TApp::instance()->getCurrentScene()->getScene(); }
class CoverUndo final : public TUndo {
  TXshLevelP m_level;
  TFrameId m_before, m_after;
  bool m_hadBefore, m_hasAfter;
  void apply(bool present, const TFrameId &frame) const {
    if (present) m_level->setCastCover(frame); else m_level->clearCastCover();
    TApp::instance()->getCurrentScene()->setDirtyFlag(true);
    TApp::instance()->getCurrentScene()->notifyCastChange();
  }
public:
  CoverUndo(TXshLevel *level, bool present, const TFrameId &frame)
      : m_level(level), m_before(level->getCastCover()), m_after(frame),
        m_hadBefore(level->hasCastCover()), m_hasAfter(present) {}
  void undo() const override { apply(m_hadBefore, m_before); }
  void redo() const override { apply(m_hasAfter, m_after); }
  int getSize() const override { return sizeof(*this); }
  QString getHistoryString() override { return tr("Choose Cast Cover Drawing"); }
};
QString drawingRange(TXshLevel *level) {
  std::vector<TFrameId> ids; level->getFids(ids);
  if (ids.empty()) return tr("No drawing identifiers");
  if (ids.size() == 1 && ids.front().isNoFrame()) return tr("Single image");
  QStringList gaps;
  qint64 previous = ids.front().getNumber();
  for (const auto &id : ids) {
    const qint64 current = id.getNumber();
    if (current > previous + 1) {
      gaps << (current == previous + 2 ? QString::number(previous + 1)
          : QStringLiteral("%1–%2").arg(previous + 1).arg(current - 1));
      if (gaps.size() == 32) { gaps << tr("… more gaps"); break; }
    }
    previous = current;
  }
  return QStringLiteral("%1–%2; ").arg(QString::fromStdString(ids.front().expand()),
      QString::fromStdString(ids.back().expand())) +
      (gaps.empty() ? tr("no numeric gaps") : tr("numeric gaps: %1").arg(gaps.join(", ")));
}

// Exact peak envelopes are built in bounded chunks on the UI event loop. Each
// tick examines at most 65536 source samples; selection/hide cancels the work.
class Waveform final : public QWidget {
  TSoundTrackP m_track;
  std::vector<std::vector<QPair<double, double>>> m_peaks;
  QTimer m_timer;
  int m_bin = 0;
  TINT32 m_sample = 0;
  double m_position = 0;
  void advance() {
    if (!m_track || m_peaks.empty()) return;
    const int bins = int(m_peaks.front().size());
    const qint64 count = m_track->getSampleCount();
    int budget = 65536;
    while (m_bin < bins && budget > 0) {
      const TINT32 end = TINT32((qint64(m_bin + 1) * count) / bins);
      const TINT32 next = std::min<TINT32>(end, m_sample + std::min<int>(budget, end - m_sample));
      if (next > m_sample) {
        for (int channel = 0; channel < int(m_peaks.size()); ++channel) {
          double low = 0, high = 0;
          m_track->getMinMaxPressure(m_sample, next - 1, TSound::Channel(channel), low, high);
          // Native integer sample pressure is centered but unnormalized;
          // float tracks already use amplitude units. Preserve quiet peaks.
          if (m_track->getSampleType() != TSound::FLOAT) {
            const double scale = std::ldexp(1., m_track->getBitPerSample() - 1);
            low /= scale; high /= scale;
          }
          auto &peak = m_peaks[channel][m_bin];
          peak.first = std::min(peak.first, low);
          peak.second = std::max(peak.second, high);
        }
        budget -= next - m_sample; m_sample = next;
      }
      if (m_sample >= end) ++m_bin;
    }
    setProperty("WaveformSamplesProcessed", m_sample);
    if (m_bin == bins) {
      m_timer.stop();
      double peak = 0;
      for (const auto &channel : m_peaks) for (const auto &p : channel)
        peak = std::max(peak, std::max(std::abs(p.first), std::abs(p.second)));
      setProperty("WaveformReady", true); setProperty("WaveformPeak", peak);
      setAccessibleDescription(tr("Waveform ready, %1 channels, %2 seconds, peak %3")
          .arg(m_track->getChannelCount()).arg(m_track->getDuration(), 0, 'f', 2).arg(peak, 0, 'f', 3));
    }
    update();
  }
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this); painter.fillRect(rect(), palette().base());
    if (m_peaks.empty()) {
      painter.setPen(palette().text().color());
      painter.drawText(rect(), Qt::AlignCenter, tr("No audio samples")); return;
    }
    const int channels = int(m_peaks.size()), bins = int(m_peaks.front().size());
    const double band = double(height()) / channels;
    for (int channel = 0; channel < channels; ++channel) {
      const double center = band * (channel + .5);
      painter.setPen(palette().mid().color()); painter.drawLine(0, int(center), width(), int(center));
      painter.setPen(palette().highlight().color());
      for (int bin = 0; bin < std::min(m_bin, bins); ++bin) {
        const auto &p = m_peaks[channel][bin];
        const double x = (bin + .5) * width() / bins;
        painter.drawLine(QPointF(x, center - std::max(-1., std::min(1., p.second)) * band * .44),
                         QPointF(x, center - std::max(-1., std::min(1., p.first)) * band * .44));
      }
    }
    painter.setPen(palette().text().color());
    painter.drawLine(int(m_position * width()), 0, int(m_position * width()), height());
    if (m_timer.isActive()) painter.drawText(rect(), Qt::AlignTop | Qt::AlignRight, tr("Reading waveform…"));
  }
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && seek && width() > 0)
      seek(std::max(0., std::min(1., double(event->pos().x()) / width())));
  }
  void hideEvent(QHideEvent *event) override { m_timer.stop(); QWidget::hideEvent(event); }
  void showEvent(QShowEvent *event) override {
    QWidget::showEvent(event);
    if (!m_peaks.empty() && m_bin < int(m_peaks.front().size())) m_timer.start();
  }
public:
  std::function<void(double)> seek;
  explicit Waveform(QWidget *parent) : QWidget(parent) {
    setObjectName("CastAudioWaveform"); setAccessibleName(tr("Audio waveform"));
    setMinimumHeight(120); m_timer.setInterval(1);
    connect(&m_timer, &QTimer::timeout, this, [this]() { advance(); });
  }
  void setTrack(const TSoundTrackP &track) {
    if (m_track == track) return;
    m_timer.stop(); m_track = track; m_bin = 0; m_sample = 0; m_position = 0;
    m_peaks.clear(); setProperty("WaveformReady", false); setProperty("WaveformPeak", 0.);
    setProperty("WaveformSamplesProcessed", 0);
    setProperty("WaveformSampleCount", track ? track->getSampleCount() : 0);
    if (track && track->getSampleCount() > 0) {
      const int bins = std::min(1024, int(track->getSampleCount()));
      m_peaks.resize(std::min(2, track->getChannelCount()),
                     std::vector<QPair<double, double>>(bins, {0., 0.}));
      if (isVisible()) m_timer.start();
    }
    update();
  }
  void setPosition(double position) { m_position = position; update(); }
};
}

struct CastInspector::State {
  ToonzScene *source = nullptr;
  std::vector<TXshLevelP> levels;
  std::vector<TFrameId> frames;
  QTableWidget *table;
  QLabel *preview, *time;
  QSlider *drawing, *seek;
  QPushButton *cover, *resetCover, *play, *stop;
  Waveform *waveform;
  TSoundTrackP track;
  std::unique_ptr<TSoundOutputDevice> audio;
  QTimer playback;
  QTimer previewDelay;
  quint64 pendingPreview = 0;
  TXshLevelP pendingLevel, failedLevel;
  TFrameId pendingFrame, failedFrame;
  QElapsedTimer elapsed;
  double start = 0;
  bool rebuilding = false;
  TXshLevel *current() const {
    const int row = table->currentRow();
    if (source != scene() || row < 0 || row >= int(levels.size())) return nullptr;
    auto *level = levels[row].getPointer();
    return source->getLevelSet()->getLevel(level->getName()) == level ? level : nullptr;
  }
};

CastInspector::CastInspector(QWidget *parent) : QWidget(parent), m(new State) {
  setObjectName("CastInspector"); setAccessibleName(tr("Scene Cast asset inspector"));
  setMinimumWidth(260);
  auto *layout = new QVBoxLayout(this);
  auto *heading = new QHBoxLayout;
  heading->addWidget(new QLabel(tr("Asset Inspector"), this));
  auto *close = new QPushButton(tr("Hide Inspector"), this);
  close->setObjectName("CastHideInspector");
  heading->addWidget(close); layout->addLayout(heading);
  connect(close, &QPushButton::clicked, this, &QWidget::hide);
  m->table = new QTableWidget(this);
  m->table->setObjectName("CastInspectorDetails");
  m->table->setAccessibleName(tr("Selected asset details"));
  m->table->setColumnCount(9);
  m->table->setHorizontalHeaderLabels({tr("Level"), tr("Status"), tr("Frames"), tr("Drawing range / gaps"),
      tr("Resolution / DPI / depth"), tr("Palette"), tr("Path"), tr("Tags"), tr("Library provenance")});
  m->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m->table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m->table->setSelectionMode(QAbstractItemView::SingleSelection);
  m->table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  m->table->horizontalHeader()->setStretchLastSection(true);
  layout->addWidget(m->table, 1);
  m->preview = new QLabel(this); m->preview->setAlignment(Qt::AlignCenter);
  m->preview->setObjectName("CastDrawingImage");
  setProperty("PreviewRequests", 0);
  setProperty("PreviewPending", false);
  m->previewDelay.setSingleShot(true); m->previewDelay.setInterval(40);
  m->previewDelay.setObjectName("CastPreviewDelay"); m->previewDelay.setParent(this);
  m->preview->setMinimumHeight(120); layout->addWidget(m->preview);
  m->drawing = new QSlider(Qt::Horizontal, this);
  m->drawing->setObjectName("CastDrawingPreview"); m->drawing->setAccessibleName(tr("Preview drawing"));
  layout->addWidget(m->drawing);
  m->cover = new QPushButton(tr("Use Preview Drawing as Cover"), this);
  m->cover->setObjectName("CastChooseCover");
  m->resetCover = new QPushButton(tr("Use First Drawing as Cover"), this);
  m->resetCover->setObjectName("CastResetCover");
  layout->addWidget(m->cover); layout->addWidget(m->resetCover);
  m->waveform = new Waveform(this); layout->addWidget(m->waveform);
  m->seek = new QSlider(Qt::Horizontal, this); m->seek->setRange(0, 10000);
  m->seek->setObjectName("CastAudioSeek"); m->seek->setAccessibleName(tr("Audio audition start position"));
  layout->addWidget(m->seek);
  m->time = new QLabel(this); m->time->setObjectName("CastAudioTime"); layout->addWidget(m->time);
  auto *audioButtons = new QHBoxLayout;
  m->play = new QPushButton(tr("Audition Audio"), this); m->play->setObjectName("CastAudioPlay");
  m->stop = new QPushButton(tr("Stop Audio"), this); m->stop->setObjectName("CastAudioStop");
  audioButtons->addWidget(m->play); audioButtons->addWidget(m->stop); layout->addLayout(audioButtons);
  connect(m->table, &QTableWidget::itemSelectionChanged, this, [this]() { if (!m->rebuilding) selectRow(); });
  connect(m->drawing, &QSlider::valueChanged, this, [this](int) { preview(); });
  connect(&m->previewDelay, &QTimer::timeout, this, [this]() { requestPreview(); });
  connect(IconGenerator::instance(), &IconGenerator::iconGenerated, this, [this]() { if (isVisible()) preview(); });
  connect(IconGenerator::instance(), &IconGenerator::iconRequestFinished, this,
          [this](quint64 request, bool generated) {
    if (request != m->pendingPreview) return;
    auto *simple = m->pendingLevel ? m->pendingLevel->getSimpleLevel() : nullptr;
    if (!generated && simple &&
        !IconGenerator::instance()->pendingIconRequest(simple, m->pendingFrame, false)) {
      m->failedLevel = m->pendingLevel; m->failedFrame = m->pendingFrame;
    }
    m->pendingPreview = 0; m->pendingLevel = TXshLevelP();
    setProperty("PreviewPending", false);
    if (isVisible()) preview();
  });
  connect(m->cover, &QPushButton::clicked, this, [this]() { setCover(true); });
  connect(m->resetCover, &QPushButton::clicked, this, [this]() { setCover(false); });
  connect(m->stop, &QPushButton::clicked, this, [this]() { stopAudio(); });
  connect(m->seek, &QSlider::valueChanged, this, [this](int value) {
    stopAudio(); m->waveform->setPosition(value / 10000.);
    const double duration = m->track ? m->track->getDuration() : 0;
    m->time->setText(tr("%1 / %2 seconds").arg(duration * value / 10000., 0, 'f', 2).arg(duration, 0, 'f', 2));
  });
  m->waveform->seek = [this](double fraction) { m->seek->setValue(int(fraction * 10000)); };
  connect(m->play, &QPushButton::clicked, this, [this]() {
    if (!m->current() || !m->current()->getSoundLevel() || !m->track) return;
    try {
      if (!m->track->getSampleCount()) throw std::runtime_error("No audio samples");
      stopAudio(); if (!m->audio) m->audio.reset(new TSoundOutputDevice);
      const TINT32 first = std::min<TINT32>(m->track->getSampleCount() - 1,
          TINT32(qint64(m->seek->value()) * m->track->getSampleCount() / 10000));
      m->start = double(first) / m->track->getSampleRate();
      m->audio->play(m->track, first, m->track->getSampleCount() - 1);
      m->elapsed.start(); m->playback.start(40); m->stop->setEnabled(true);
    } catch (...) { QMessageBox::warning(this, tr("Audition Audio"), tr("This sound could not be played.")); }
  });
  connect(&m->playback, &QTimer::timeout, this, [this]() {
    if (!m->audio || !m->audio->isPlaying() || !m->track || !m->current()) { stopAudio(); return; }
    const double duration = m->track->getDuration();
    const double position = std::min(duration, m->start + m->elapsed.elapsed() / 1000.);
    m->waveform->setPosition(duration > 0 ? position / duration : 0);
    m->time->setText(tr("%1 / %2 seconds").arg(position, 0, 'f', 2).arg(duration, 0, 'f', 2));
  });
  connect(TApp::instance()->getCurrentScene(), &TSceneHandle::sceneSwitched,
          this, [this]() { setLevels({}); });
  setLevels({});
}

CastInspector::~CastInspector() { stopAudio(); if (m->audio) m->audio->close(); }
void CastInspector::hideEvent(QHideEvent *event) {
  stopAudio(); m->previewDelay.stop(); m->preview->clear();
  setProperty("PreviewFrame", QString());
  // The single shared request may still finish for another icon consumer.
  // Keep its token until completion; never cancel the global icon queue.
  QWidget::hideEvent(event);
}
void CastInspector::showEvent(QShowEvent *event) {
  QWidget::showEvent(event); preview();
}
void CastInspector::stopAudio() {
  m->playback.stop(); if (m->audio) m->audio->stop(); m->stop->setEnabled(false);
}
void CastInspector::setLevels(const std::vector<TXshLevel *> &levels) {
  TXshLevel *previous = m->current();
  std::vector<TXshLevelP> retained;
  for (auto *level : levels)
    if (scene()->getLevelSet()->getLevel(level->getName()) == level) retained.emplace_back(level);
  const auto active = std::find_if(retained.begin(), retained.end(),
      [previous](const TXshLevelP &level) { return level.getPointer() == previous; });
  const bool same = m->source == scene() && previous && active != retained.end();
  const int activeRow = same ? int(active - retained.begin()) : 0;
  if (!same) stopAudio();
  m->source = scene(); m->levels = std::move(retained);
  refreshRows();
  { QSignalBlocker blocked(m->table); m->table->selectRow(activeRow); }
  if (!same) selectRow();
  else {
    auto *level = m->current();
    auto track = level && level->getSoundLevel() ? level->getSoundLevel()->getSoundTrack() : TSoundTrackP();
    if (track != m->track) selectRow();
    else if (level && level->getSimpleLevel()) {
      const TFrameId old = m->frames.empty() ? TFrameId() : m->frames[std::min(m->drawing->value(), int(m->frames.size()) - 1)];
      m->frames.clear(); level->getFids(m->frames);
      QSignalBlocker blocked(m->drawing); m->drawing->setRange(0, std::max(0, int(m->frames.size()) - 1));
      auto at = std::find(m->frames.begin(), m->frames.end(), old);
      m->drawing->setValue(at == m->frames.end() ? 0 : int(at - m->frames.begin()));
      m->drawing->setEnabled(!m->frames.empty()); m->cover->setEnabled(!m->frames.empty());
      m->resetCover->setEnabled(level->hasCastCover()); preview();
    }
  }
}
void CastInspector::refreshRows() {
  m->rebuilding = true;
  const int current = m->table->currentRow();
  m->table->setRowCount(int(m->levels.size()));
  for (int r = 0; r < int(m->levels.size()); ++r) {
    auto *level = m->levels[r].getPointer(); QString geometry; TPalette *palette = nullptr;
    if (auto *simple = level->getSimpleLevel()) {
      palette = simple->getPalette(); const auto size = simple->getResolution(); const auto dpi = simple->getDpi();
      geometry = tr("%1 × %2 / %3 × %4 DPI / %5 bpp").arg(size.lx).arg(size.ly)
          .arg(dpi.x).arg(dpi.y).arg(simple->getProperties()->getBpp());
    }
    if (auto *pl = level->getPaletteLevel()) palette = pl->getPalette();
    QString paletteInfo = tr("None");
    if (palette) {
      paletteInfo = tr("%1; %2 styles").arg(QString::fromStdWString(palette->getPaletteName())).arg(palette->getStyleCount());
      if (!palette->getGlobalName().empty()) paletteInfo += tr("; studio link: %1").arg(QString::fromStdWString(palette->getGlobalName()));
      if (palette->getDirtyFlag()) paletteInfo += tr("; modified");
    }
    const auto &origin = level->getLibraryProvenance();
    const QString originText = origin.relationship == TXshLevel::LibraryProvenance::NotRecorded
        ? tr("No recorded Library origin")
        : tr("%1; revision %2; item %3; source %4").arg(origin.kind).arg(origin.revision).arg(origin.itemPath, origin.sourceAssetPath);
    const QStringList values{QString::fromStdWString(level->getName()), CastBrowserActions::status(scene(), level),
        QString::number(level->getFrameCount()), level->getSimpleLevel() ? drawingRange(level) : tr("Not a drawing level"),
        geometry, paletteInfo, scene()->decodeFilePath(level->getPath()).getQString(), level->getCastTags().join(", "), originText};
    for (int c = 0; c < values.size(); ++c) {
      auto *item = m->table->item(r, c);
      if (!item) { item = new QTableWidgetItem; m->table->setItem(r, c, item); }
      item->setText(values[c]); item->setToolTip(values[c]);
    }
  }
  if (current >= 0 && current < int(m->levels.size())) m->table->selectRow(current);
  m->rebuilding = false;
}
void CastInspector::selectRow() {
  stopAudio(); m->previewDelay.stop(); m->failedLevel = TXshLevelP();
  m->frames.clear(); m->track = TSoundTrackP();
  auto *level = m->current();
  if (level && level->getSimpleLevel()) level->getFids(m->frames);
  if (level && level->getSoundLevel()) m->track = level->getSoundLevel()->getSoundTrack();
  m->drawing->setRange(0, std::max(0, int(m->frames.size()) - 1)); m->drawing->setValue(0);
  m->drawing->setEnabled(!m->frames.empty()); m->cover->setEnabled(!m->frames.empty());
  m->resetCover->setEnabled(level && level->getSimpleLevel() && level->hasCastCover());
  m->play->setEnabled(m->track && m->track->getSampleCount() > 0);
  m->seek->setEnabled(m->play->isEnabled()); m->seek->setValue(0);
  m->waveform->setTrack(m->track);
  m->time->setText(m->track ? tr("0.00 / %1 seconds").arg(m->track->getDuration(), 0, 'f', 2) : tr("Select an audio asset to preview its waveform."));
  m->waveform->setVisible(level && level->getSoundLevel());
  preview();
}
void CastInspector::preview() {
  if (!isVisible()) return;
  auto *level = m->current();
  m->preview->clear(); setProperty("PreviewFrame", QString());
  if (!level || !level->getSimpleLevel() || m->frames.empty()) { m->previewDelay.stop(); return; }
  const auto frame = m->frames[std::min(m->drawing->value(), int(m->frames.size()) - 1)];
  auto *simple = level->getSimpleLevel();
  if (!simple->isFid(frame)) { m->previewDelay.stop(); return; }
  auto *icons = IconGenerator::instance();
  // Probing may convert an existing icon, but never loads a drawing or queues.
  const QPixmap image = icons->pendingIconRequest(simple, frame, false) ? QPixmap()
      : icons->getIcon(simple, frame, false, true);
  m->preview->setToolTip(tr("Drawing %1").arg(QString::fromStdString(frame.expand())));
  if (!image.isNull()) {
    m->previewDelay.stop();
    m->preview->setPixmap(image.scaled(320, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    setProperty("PreviewFrame", QString::fromStdString(frame.expand()));
  } else if (m->failedLevel.getPointer() == level && m->failedFrame == frame) {
    m->previewDelay.stop(); m->preview->setText(tr("Drawing preview unavailable"));
  } else if (!m->pendingPreview) {
    // Repeated scrubs replace the desired frame (the slider value). Only one
    // generation is in flight; completion schedules the latest desired frame.
    m->preview->setText(tr("Loading drawing preview…"));
    if (!m->previewDelay.isActive()) m->previewDelay.start();
  }
}
void CastInspector::requestPreview() {
  if (!isVisible() || m->pendingPreview) return;
  auto *level = m->current();
  if (!level || !level->getSimpleLevel() || m->frames.empty()) return;
  const auto frame = m->frames[std::min(m->drawing->value(), int(m->frames.size()) - 1)];
  if (!level->getSimpleLevel()->isFid(frame)) return;
  m->pendingLevel = TXshLevelP(level); m->pendingFrame = frame;
  try {
    m->pendingPreview = IconGenerator::instance()->requestIcon(level->getSimpleLevel(), frame, false);
  } catch (...) {
    m->failedLevel = m->pendingLevel; m->failedFrame = frame;
  }
  if (m->pendingPreview) {
    setProperty("PreviewRequests", property("PreviewRequests").toInt() + 1);
    setProperty("PreviewPending", true);
  } else {
    m->pendingLevel = TXshLevelP(); preview();
  }
}
void CastInspector::setCover(bool present) {
  auto *level = m->current(); if (!level || m->frames.empty()) return;
  const auto frame = m->frames[std::min(m->drawing->value(), int(m->frames.size()) - 1)];
  if ((present && level->hasCastCover() && level->getCastCover() == frame) || (!present && !level->hasCastCover())) return;
  auto *undo = new CoverUndo(level, present, frame); undo->redo(); TUndoManager::manager()->add(undo);
  m->resetCover->setEnabled(present);
}
