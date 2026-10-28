#include "cadmiumtypes.h"
#include "trasterimage.h"

#include <QCryptographicHash>
#include <QHash>
#include <algorithm>
#include <cmath>
#include <limits>

namespace Cadmium {
namespace {
struct Sample { double r, g, b, a; };
double clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }
double toDisplay(double v) {
  return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}
double toLinear(double v) {
  return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
int byte(double v) { return int(std::floor(clamp01(v) * 255.0 + 0.5)); }

template <class Pixel> Sample decode(const Pixel &p, double scale) {
  return {double(p.r) / scale, double(p.g) / scale,
          double(p.b) / scale, double(p.m) / scale};
}
Sample read(const TRasterP &r, int x, int y) {
  if (TRaster32P r32 = r) return decode(r32->pixels(y)[x], 255.0);
  if (TRaster64P r64 = r) return decode(r64->pixels(y)[x], 65535.0);
  TRasterFP rf = r;
  return decode(rf->pixels(y)[x], 1.0);
}
bool supported(const TRasterP &r) {
  return r && (TRaster32P(r) || TRaster64P(r) || TRasterFP(r));
}
bool finite(const Sample &s) {
  return std::isfinite(s.r) && std::isfinite(s.g) &&
         std::isfinite(s.b) && std::isfinite(s.a) && s.a >= 0.0 && s.a <= 1.0;
}
Sample straight(Sample s) {
  if (s.a > 0.0) { s.r /= s.a; s.g /= s.a; s.b /= s.a; }
  else s.r = s.g = s.b = 0.0;
  return s;
}
Sample display(Sample s, bool linear) {
  s = straight(s);
  if (linear) {
    s.r = toDisplay(std::max(0.0, s.r));
    s.g = toDisplay(std::max(0.0, s.g));
    s.b = toDisplay(std::max(0.0, s.b));
  }
  return s;
}
Sample extractInk(Sample input, bool paper, bool linear) {
  if (!paper) return input;
  Sample s = display(input, linear);
  // This decomposition is an artist-approved interpretation of a white-paper
  // scan. Linked clean lines avoid treating colored paint as line coverage.
  double a = 1.0 - std::min({clamp01(s.r), clamp01(s.g), clamp01(s.b)});
  if (a <= 0.0) return {0, 0, 0, 0};
  Sample ink{clamp01((s.r - (1.0 - a)) / a),
             clamp01((s.g - (1.0 - a)) / a),
             clamp01((s.b - (1.0 - a)) / a), a};
  if (linear) {
    ink.r = toLinear(ink.r); ink.g = toLinear(ink.g); ink.b = toLinear(ink.b);
  }
  ink.r *= a; ink.g *= a; ink.b *= a;
  return ink;
}
void store(const TRasterP &r, int x, int y, const Sample &s) {
  if (TRaster32P r32 = r) {
    r32->pixels(y)[x] = TPixel32(byte(s.r), byte(s.g), byte(s.b), byte(s.a));
  } else if (TRaster64P r64 = r) {
    auto c = [](double v) { return int(std::floor(clamp01(v) * 65535.0 + 0.5)); };
    r64->pixels(y)[x] = TPixel64(c(s.r), c(s.g), c(s.b), c(s.a));
  } else {
    TRasterFP rf = r;
    rf->pixels(y)[x] = TPixelF(float(s.r), float(s.g), float(s.b), float(s.a));
  }
}
struct RasterLock {
  TRasterP raster;
  explicit RasterLock(const TRasterP &r) : raster(r) { raster->lock(); }
  ~RasterLock() { raster->unlock(); }
};
bool opaque(const TRasterP &r) {
  for (int y = 0; y < r->getLy(); ++y)
    for (int x = 0; x < r->getLx(); ++x)
      if (read(r, x, y).a < 1.0) return false;
  return true;
}
void hashToken(QCryptographicHash &hash, const QByteArray &value) {
  hash.addData(value);
  hash.addData("\n");
}
void hashRaster(QCryptographicHash &hash, const TRasterImageP &image) {
  const TRasterP r = image->getRaster();
  hashToken(hash, QByteArray::number(r->getLx()));
  hashToken(hash, QByteArray::number(r->getLy()));
  hashToken(hash, QByteArray::number(r->getPixelSize()));
  for (int y = 0; y < r->getLy(); ++y)
    hash.addData(reinterpret_cast<const char *>(r->getRawData() +
        y * r->getWrap() * r->getPixelSize()), r->getLx() * r->getPixelSize());
  double dx, dy; image->getDpi(dx, dy);
  hashToken(hash, QByteArray::number(dx, 'g', 17));
  hashToken(hash, QByteArray::number(dy, 'g', 17));
  hashToken(hash, QByteArray::number(image->getOffset().x));
  hashToken(hash, QByteArray::number(image->getOffset().y));
  const TRect box = image->getSavebox();
  hashToken(hash, QByteArray::number(box.x0));
  hashToken(hash, QByteArray::number(box.y0));
  hashToken(hash, QByteArray::number(box.x1));
  hashToken(hash, QByteArray::number(box.y1));
  hashToken(hash, QByteArray::number(image->getSubsampling()));
  hashToken(hash, r->isLinear() ? "linear" : "display");
  hashToken(hash, image->isOpaque() ? "opaque" : "alpha");
  hashToken(hash, image->isScanBW() ? "scan" : "color");
}
QByteArray fingerprint(const TRasterImageP &source,
                       const TRasterImageP &lines, const Frame &options) {
  RasterLock sourceLock(source->getRaster()), lineLock(lines->getRaster());
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hashRaster(hash, source); hashRaster(hash, lines);
  hashToken(hash, QByteArray::number(int(options.rasterSource)));
  hashToken(hash, options.extractionApproved ? "approved" : "unapproved");
  hashToken(hash, QByteArray::number(options.paperThreshold, 'g', 17));
  hashToken(hash, QByteArray::number(options.lineThreshold, 'g', 17));
  hashToken(hash, QByteArray::number(options.protectionMask.width()));
  hashToken(hash, QByteArray::number(options.protectionMask.height()));
  // Canonical pixels exclude uninitialized scanline padding and make the
  // freshness check independent of the mask's storage format.
  for (int y = 0; y < options.protectionMask.height(); ++y)
    for (int x = 0; x < options.protectionMask.width(); ++x) {
      const QRgb color = options.protectionMask.pixel(x, y);
      hash.addData(reinterpret_cast<const char *>(&color), sizeof(color));
    }
  return hash.result();
}
}

bool snapshotFullColor(const TImageP &image, Frame &frame, QString &error) {
  error.clear();
  TRasterImageP input = image;
  if (!input || !supported(input->getRaster())) {
    error = QStringLiteral("Choose an 8-bit, 16-bit, or floating-point raster drawing.");
    return false;
  }
  if (input->getSubsampling() != 1) {
    error = QStringLiteral("Reload the full-resolution drawing before painting.");
    return false;
  }
  const TRasterP source = input->getRaster();
  const int w = source->getLx(), h = source->getLy();
  if (w <= 0 || h <= 0 || qint64(w) * h > std::numeric_limits<int>::max()) {
    error = QStringLiteral("The raster dimensions exceed the supported image size.");
    return false;
  }
  if (!std::isfinite(frame.paperThreshold) || frame.paperThreshold < 0.0 ||
      frame.paperThreshold > 1.0 || !std::isfinite(frame.lineThreshold) ||
      frame.lineThreshold <= 0.0 || frame.lineThreshold >= 1.0) {
    error = QStringLiteral("The line and paper thresholds are invalid.");
    return false;
  }
  TRasterImageP lineImage = frame.rasterSource == RasterSource::LinkedLines
      ? TRasterImageP(frame.lineSource) : input;
  if (!lineImage || !supported(lineImage->getRaster()) ||
      lineImage->getRaster()->getSize() != source->getSize() ||
      lineImage->getOffset() != input->getOffset()) {
    error = QStringLiteral("Choose a clean-line drawing with matching dimensions and offset.");
    return false;
  }
  double dx, dy, lx, ly;
  input->getDpi(dx, dy); lineImage->getDpi(lx, ly);
  if (dx != lx || dy != ly || lineImage->getSubsampling() != 1) {
    error = QStringLiteral("The clean-line drawing must have matching DPI and full resolution.");
    return false;
  }
  if (!frame.protectionMask.isNull() && frame.protectionMask.size() != QSize(w, h)) {
    error = QStringLiteral("The protection mask has the wrong dimensions.");
    return false;
  }
  RasterLock sourceLock(source);
  const TRasterP line = lineImage->getRaster();
  RasterLock lineLock(line);
  const bool paper = frame.rasterSource == RasterSource::PaperScan || opaque(line);
  if (paper && !frame.extractionApproved) {
    error = QStringLiteral("Preview and approve white-paper line extraction, or choose transparent clean lines.");
    return false;
  }
  QImage lines(w, h, QImage::Format_ARGB32);
  if (lines.isNull()) {
    error = QStringLiteral("There is not enough memory for the line snapshot.");
    return false;
  }
  QVector<int> labels(w * h, -1);
  for (int y = 0; y < h; ++y) {
    QRgb *row = reinterpret_cast<QRgb *>(lines.scanLine(y));
    for (int x = 0; x < w; ++x) {
      Sample s = read(line, x, h - 1 - y);
      if (!finite(s) || !finite(read(source, x, h - 1 - y))) {
        error = QStringLiteral("The drawing contains nonfinite samples or invalid alpha.");
        return false;
      }
      Sample ink = extractInk(s, paper, line->isLinear());
      if (paper && display(s, line->isLinear()).r >= frame.paperThreshold &&
          display(s, line->isLinear()).g >= frame.paperThreshold &&
          display(s, line->isLinear()).b >= frame.paperThreshold)
        ink = {0, 0, 0, 0};
      Sample visible = display(ink, line->isLinear());
      row[x] = qRgba(byte(visible.r), byte(visible.g), byte(visible.b), byte(ink.a));
      if (ink.a >= frame.lineThreshold) labels[y * w + x] = 0;
    }
  }
  // Label enclosed regions without narrowing labels to a byte. Components
  // touching the canvas border are exterior, never implicit paint targets.
  int count = 0;
  QVector<int> queue;
  for (int seed = 0; seed < labels.size(); ++seed) {
    if (labels[seed] != -1) continue;
    queue.clear(); queue.append(seed); labels[seed] = -2;
    bool exterior = false;
    for (int k = 0; k < queue.size(); ++k) {
      const int p = queue[k], x = p % w, y = p / w;
      exterior = exterior || x == 0 || y == 0 || x == w - 1 || y == h - 1;
      auto add = [&](int n) { if (labels[n] == -1) { labels[n] = -2; queue.append(n); } };
      if (x) add(p - 1); if (x + 1 < w) add(p + 1);
      if (y) add(p - w); if (y + 1 < h) add(p + w);
    }
    const int label = exterior ? 0 : ++count;
    for (int p : queue) labels[p] = label;
  }
  if (count == 0) {
    error = QStringLiteral("No enclosed paint regions were found. Review the clean lines and extraction threshold.");
    return false;
  }
  QVector<QHash<QRgb, int>> histograms(count + 1);
  QVector<int> samples(count + 1, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int label = labels[y * w + x];
      if (!label || qAlpha(lines.pixel(x, y)) > 25) continue;
      Sample s = display(read(source, x, h - 1 - y), source->isLinear());
      if (s.a <= 0.0) continue;
      ++histograms[label][qRgba(byte(s.r), byte(s.g), byte(s.b), byte(s.a))];
      ++samples[label];
    }
  QVector<QColor> colors(count + 1);
  for (int label = 1; label <= count; ++label) {
    int best = 0; QRgb color = 0;
    for (auto i = histograms[label].cbegin(); i != histograms[label].cend(); ++i)
      if (i.value() > best || (i.value() == best && i.key() < color)) {
        best = i.value(); color = i.key();
      }
    if (best > 0 && qint64(best) * 5 >= qint64(samples[label]) * 4)
      colors[label] = QColor::fromRgba(color);
  }
  // Extend interior labels beneath partially covered line-edge pixels only.
  // Opaque ink and exterior transparent/paper pixels retain label zero.
  queue.clear();
  for (int p = 0; p < labels.size(); ++p) if (labels[p] > 0) queue.append(p);
  for (int k = 0; k < queue.size(); ++k) {
    const int p = queue[k], x = p % w, y = p / w;
    auto add = [&](int n) {
      const int a = qAlpha(lines.pixel(n % w, n / w));
      if (labels[n] == 0 && a > 0 && a < 255) {
        labels[n] = labels[p]; queue.append(n);
      }
    };
    if (x) add(p - 1); if (x + 1 < w) add(p + 1);
    if (y) add(p - w); if (y + 1 < h) add(p + w);
  }
  Frame result = frame;
  result.kind = Kind::FullColor;
  result.original = TImageP(input->cloneImage());
  if (frame.lineSource) result.lineSource = TImageP(frame.lineSource->cloneImage());
  result.lines = lines;
  result.labels = labels;
  result.regionStyles = QVector<int>(count + 1, -1);
  result.regionColors = colors;
  result.fingerprint = fingerprint(input, lineImage, frame);
  frame = result;
  return true;
}

bool matchesFullColor(const TImageP &source, const Frame &options,
                      const Frame &frame, QString &error) {
  error.clear();
  TRasterImageP current = source;
  TRasterImageP lines = options.rasterSource == RasterSource::LinkedLines
      ? TRasterImageP(options.lineSource) : current;
  if (frame.kind != Kind::FullColor || frame.fingerprint.isEmpty() ||
      !current || !lines || !supported(current->getRaster()) ||
      !supported(lines->getRaster()) || current->getSubsampling() != 1 ||
      lines->getSubsampling() != 1 ||
      (current->getRaster()->getLx() != frame.lines.width() ||
       current->getRaster()->getLy() != frame.lines.height())) {
    error = QStringLiteral("The full-color source or clean-line drawing is unavailable or has changed.");
    return false;
  }
  if (fingerprint(current, lines, options) != frame.fingerprint) {
    error = QStringLiteral("The full-color drawing, clean lines, or image settings changed. Generate the paint proposal again.");
    return false;
  }
  return true;
}

bool matchesFullColor(const TImageP &source, const Frame &frame,
                      const TImageP &lineSource, QString &error) {
  Frame options = frame;
  options.lineSource = lineSource;
  return matchesFullColor(source, options, frame, error);
}

TImageP applyFullColor(const Frame &frame, const QVector<int> &styleIds,
                      const QVector<QColor> &colors, bool repaint, QString &error) {
  Q_UNUSED(styleIds);
  error.clear();
  TRasterImageP original = frame.original;
  if (frame.kind != Kind::FullColor || !original || !supported(original->getRaster())) {
    error = QStringLiteral("The full-color paint snapshot is invalid."); return TImageP();
  }
  const TRasterP source = original->getRaster();
  const int w = source->getLx(), h = source->getLy();
  const int regionCount = frame.regionStyles.size();
  if (frame.lines.size() != QSize(w, h) || qint64(w) * h != frame.labels.size() ||
      regionCount <= 1 || colors.size() != regionCount ||
      (!frame.protectionMask.isNull() && frame.protectionMask.size() != QSize(w, h))) {
    error = QStringLiteral("The paint proposal does not match its source snapshot."); return TImageP();
  }
  for (int label : frame.labels)
    if (label < 0 || label >= regionCount) {
      error = QStringLiteral("The paint proposal contains an invalid region label."); return TImageP();
    }
  TRasterImageP inkImage = frame.rasterSource == RasterSource::LinkedLines
      ? TRasterImageP(frame.lineSource) : original;
  if (!inkImage || !supported(inkImage->getRaster()) ||
      inkImage->getRaster()->getSize() != source->getSize()) {
    error = QStringLiteral("The clean-line snapshot is unavailable."); return TImageP();
  }
  TRasterImageP output = TImageP(original->cloneImage());
  const TRasterP destination = output->getRaster();
  const TRasterP inkRaster = inkImage->getRaster();
  RasterLock sourceLock(source), inkLock(inkRaster), destinationLock(destination);
  const bool paper = frame.rasterSource == RasterSource::PaperScan || opaque(inkRaster);
  if (paper && !frame.extractionApproved) {
    error = QStringLiteral("The line extraction has not been approved."); return TImageP();
  }
  const bool paperBase = frame.rasterSource == RasterSource::PaperScan ||
      (frame.extractionApproved && opaque(source));
  bool changed = false;
  TRect savebox = output->getSavebox();
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int label = frame.labels[y * w + x];
      if (label == 0 || !colors[label].isValid()) continue;
      if (!frame.protectionMask.isNull() && qGray(frame.protectionMask.pixel(x, y)) > 0)
        continue;
      const int nativeY = h - 1 - y;
      Sample current = read(source, x, nativeY);
      Sample ink = extractInk(read(inkRaster, x, nativeY), paper, inkRaster->isLinear());
      if (qAlpha(frame.lines.pixel(x, y)) == 0) ink = {0, 0, 0, 0};
      Sample visible = display(current, source->isLinear());
      if (!repaint) {
        // Transparent pixels are empty. In explicitly approved white-paper
        // mode only the selected paper color is empty. A supplied mask may
        // protect white-painted areas that pixels alone cannot distinguish.
        if (paperBase) {
          if (std::min({visible.r, visible.g, visible.b}) < frame.paperThreshold &&
              current.a > ink.a + 1.0 / 65535.0) continue;
        } else if (current.a > ink.a + 1.0 / 65535.0) continue;
      }
      const QColor &c = colors[label];
      double r = c.redF(), g = c.greenF(), b = c.blueF(), a = c.alphaF();
      if (source->isLinear()) { r = toLinear(r); g = toLinear(g); b = toLinear(b); }
      // Convert the line samples to the destination color space before the
      // native-precision source-over operation.
      if (inkRaster->isLinear() != source->isLinear()) {
        Sample straightInk = straight(ink);
        auto convert = [&](double v) {
          return source->isLinear() ? toLinear(clamp01(v)) : toDisplay(std::max(0.0, v));
        };
        ink.r = convert(straightInk.r) * ink.a;
        ink.g = convert(straightInk.g) * ink.a;
        ink.b = convert(straightInk.b) * ink.a;
      }
      const double behind = (1.0 - ink.a) * a;
      Sample painted{ink.r + behind * r, ink.g + behind * g,
                     ink.b + behind * b, ink.a + behind};
      store(destination, x, nativeY, painted);
      savebox += TRect(x, nativeY, x, nativeY); changed = true;
    }
  if (changed) {
    output->setSavebox(savebox);
    output->setScanBWFlag(false);
  }
  return TImageP(output);
}
}
