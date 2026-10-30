#include "color_metrics.h"
#include "toonz/rasterbrushimport.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace RasterBrush;
namespace {
void require(bool v, const QString &message) {
  if (!v)
    throw std::runtime_error(message.toStdString());
}
Pixel pixel(QRgb q) {
  double a = qAlpha(q) / 255.0;
  return {qRed(q) / 255.0 * a, qGreen(q) / 255.0 * a, qBlue(q) / 255.0 * a, a};
}
QRgb encoded(Pixel p) {
  auto byte = [](double v) {
    return int(std::floor(std::clamp(v, 0.0, 1.0) * 255 + .5));
  };
  return qRgba(p.a > 0 ? byte(p.r / p.a) : 0, p.a > 0 ? byte(p.g / p.a) : 0,
               p.a > 0 ? byte(p.b / p.a) : 0, byte(p.a));
}
struct Canvas : Surface {
  int w, h;
  std::vector<Pixel> data;
  explicit Canvas(const QImage &image)
      : w(image.width()), h(image.height()), data(size_t(w) * h) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        data[size_t(y) * w + x] = pixel(image.pixel(x, y));
  }
  int width() const override { return w; }
  int height() const override { return h; }
  bool prepare(int, int, int, int) override { return true; }
  Pixel read(int x, int y) const override { return data[size_t(y) * w + x]; }
  void write(int x, int y, const Pixel &p) override {
    data[size_t(y) * w + x] = p;
  }
  QImage image() const {
    QImage result(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        result.setPixel(x, y, encoded(read(x, y)));
    return result;
  }
};
QJsonArray bounds(const QRect &r) {
  return {r.x(), r.y(), r.width(), r.height()};
}
QJsonObject compare(const QImage &base, const QImage &reference,
                    const QImage &candidate, const QString &diffPath) {
  const int w = base.width(), h = base.height();
  require(reference.size() == base.size() && candidate.size() == base.size(),
          "Reference dimensions differ; resizing is prohibited");
  std::vector<unsigned char> region(size_t(w) * h, 0);
  QRect refBounds, candidateBounds;
  QRect refAlphaBounds, candidateAlphaBounds;
  qint64 alphaIntersection = 0, alphaUnion = 0;
  qint64 visibleIntersection = 0, visibleUnion = 0;
  bool transparentBase = true;
  qint64 refCount = 0, candidateCount = 0, intersection = 0, unionCount = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      transparentBase = transparentBase && qAlpha(base.pixel(x, y)) == 0;
      const bool ra = qAlpha(reference.pixel(x, y)) > 0;
      const bool ca = qAlpha(candidate.pixel(x, y)) > 0;
      if (ra)
        refAlphaBounds |= QRect(x, y, 1, 1);
      if (ca)
        candidateAlphaBounds |= QRect(x, y, 1, 1);
      alphaIntersection += ra && ca;
      alphaUnion += ra || ca;
      bool r = reference.pixel(x, y) != base.pixel(x, y);
      bool c = candidate.pixel(x, y) != base.pixel(x, y);
      // Hidden RGB at zero alpha is not a visible change, including on a
      // partially transparent background. Keep the raw union for error
      // sampling.
      const bool rv =
          r && (qAlpha(reference.pixel(x, y)) || qAlpha(base.pixel(x, y)));
      const bool cv =
          c && (qAlpha(candidate.pixel(x, y)) || qAlpha(base.pixel(x, y)));
      visibleIntersection += rv && cv;
      visibleUnion += rv || cv;
      if (r) {
        ++refCount;
        refBounds |= QRect(x, y, 1, 1);
      }
      if (c) {
        ++candidateCount;
        candidateBounds |= QRect(x, y, 1, 1);
      }
      if (r && c)
        ++intersection;
      if (!r && !c)
        continue;
      ++unionCount;
      for (int yy = std::max(0, y - 2); yy <= std::min(h - 1, y + 2); ++yy)
        for (int xx = std::max(0, x - 2); xx <= std::min(w - 1, x + 2); ++xx)
          region[size_t(yy) * w + xx] = 1;
    }
  double rgbError = 0, alphaError = 0, maxRGB = 0, maxAlpha = 0;
  std::vector<double> alphaErrors;
  std::vector<double> perceptual[2];
  double perceptualSum[2] = {0, 0};
  QImage diff(w, h, QImage::Format_ARGB32);
  diff.fill(qRgb(0, 0, 0));
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      Pixel r = pixel(reference.pixel(x, y)), c = pixel(candidate.pixel(x, y));
      double er = std::abs(r.r - c.r), eg = std::abs(r.g - c.g),
             eb = std::abs(r.b - c.b), ea = std::abs(r.a - c.a);
      diff.setPixel(x, y,
                    qRgb(int(std::min(1.0, er + ea) * 255 + .5),
                         int(std::min(1.0, eg + ea) * 255 + .5),
                         int(std::min(1.0, eb + ea) * 255 + .5)));
      if (!region[size_t(y) * w + x])
        continue;
      rgbError += (er + eg + eb) / 3;
      alphaError += ea;
      maxRGB = std::max(maxRGB, std::max(er, std::max(eg, eb)));
      maxAlpha = std::max(maxAlpha, ea);
      alphaErrors.push_back(ea);
      for (int background = 0; background <= 1; ++background) {
        auto lab = [background](Pixel p) {
          double paper = background * (1 - p.a);
          return BrushMetrics::srgbLab(p.r + paper, p.g + paper, p.b + paper);
        };
        double difference = BrushMetrics::deltaE00(lab(r), lab(c));
        perceptual[background].push_back(difference);
        perceptualSum[background] += difference;
      }
    }
  require(diff.save(diffPath), "Cannot save full-canvas difference");
  std::sort(alphaErrors.begin(), alphaErrors.end());
  for (auto &errors : perceptual)
    std::sort(errors.begin(), errors.end());
  const double n = double(alphaErrors.size());
  return {
      {"region", "union of changed sets, square dilation radius 2"},
      {"regionPixels", n},
      {"referenceBounds", bounds(refBounds)},
      {"candidateBounds", bounds(candidateBounds)},
      {"referenceAlphaBounds", bounds(refAlphaBounds)},
      {"candidateAlphaBounds", bounds(candidateAlphaBounds)},
      {"transparentBase", transparentBase},
      {"alphaCoverageIoU",
       alphaUnion ? double(alphaIntersection) / alphaUnion : 1},
      {"referenceChangedPixels", double(refCount)},
      {"candidateChangedPixels", double(candidateCount)},
      {"changedPixelIoU", unionCount ? double(intersection) / unionCount : 1},
      {"visibleChangedPixelIoU",
       visibleUnion ? double(visibleIntersection) / visibleUnion : 1},
      {"premultipliedRGBMAE", n ? rgbError / n : 0},
      {"alphaMAE", n ? alphaError / n : 0},
      {"maxRGBError", maxRGB},
      {"maxAlphaError", maxAlpha},
      {"alphaP99", n ? alphaErrors[size_t(std::ceil(.99 * n)) - 1] : 0},
      {"deltaE00BlackMean", n ? perceptualSum[0] / n : 0},
      {"deltaE00WhiteMean", n ? perceptualSum[1] / n : 0},
      {"deltaE00BlackP95",
       n ? perceptual[0][size_t(std::ceil(.95 * n)) - 1] : 0},
      {"deltaE00WhiteP95",
       n ? perceptual[1][size_t(std::ceil(.95 * n)) - 1] : 0},
      {"emptyCandidateFailure",
       transparentBase
           ? !refAlphaBounds.isEmpty() && candidateAlphaBounds.isEmpty()
           : refCount > 0 && candidateCount == 0},
      {"bothEmpty", transparentBase ? refAlphaBounds.isEmpty() &&
                                          candidateAlphaBounds.isEmpty()
                                    : refCount == 0 && candidateCount == 0}};
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    if (app.arguments().size() == 2 &&
        app.arguments().at(1) == "--test-metrics") {
      QTemporaryDir directory;
      require(directory.isValid(), "Cannot create metric fixture directory");
      QImage base(2, 2, QImage::Format_ARGB32);
      base.fill(Qt::transparent);
      QImage reference = base, candidate = base;
      reference.setPixel(0, 0, qRgba(255, 0, 0, 0));
      reference.setPixel(1, 0, qRgba(0, 0, 255, 255));
      candidate.setPixel(1, 0, qRgba(0, 0, 255, 255));
      auto measured = compare(base, reference, candidate,
                              directory.filePath("hidden-rgb.png"));
      require(measured["alphaCoverageIoU"].toDouble() == 1 &&
                  measured["changedPixelIoU"].toDouble() == .5 &&
                  measured["visibleChangedPixelIoU"].toDouble() == 1 &&
                  measured["maxRGBError"].toDouble() == 0 &&
                  measured["maxAlphaError"].toDouble() == 0,
              "Undefined transparent RGB was counted as painted coverage");
      candidate.setPixel(0, 1, qRgba(255, 0, 0, 255));
      measured = compare(base, reference, candidate,
                         directory.filePath("stray-visible.png"));
      require(measured["alphaCoverageIoU"].toDouble() == .5 &&
                  measured["maxAlphaError"].toDouble() == 1,
              "Stray visible candidate pixels escaped the union comparison");
      base.setPixel(0, 1, qRgba(255, 0, 0, 128));
      reference = base;
      candidate = base;
      reference.setPixel(0, 0, qRgba(255, 0, 0, 0));
      reference.setPixel(1, 0, qRgba(0, 0, 255, 96));
      candidate.setPixel(1, 0, qRgba(0, 0, 255, 96));
      measured = compare(base, reference, candidate,
                         directory.filePath("mixed-alpha-hidden-rgb.png"));
      require(!measured["transparentBase"].toBool() &&
                  measured["changedPixelIoU"].toDouble() == .5 &&
                  measured["visibleChangedPixelIoU"].toDouble() == 1 &&
                  measured["maxRGBError"].toDouble() == 0 &&
                  measured["maxAlphaError"].toDouble() == 0,
              "Hidden RGB contaminated mixed-background coverage");
      // Published numerical pairs, including neutral and hue-wrap boundaries.
      // https://www.ece.rochester.edu/~gsharma/ciede2000/
      const double pairs[][7] = {
          {50, 2.6772, -79.7751, 50, 0, -82.7485, 2.0425},
          {50, 3.1571, -77.2803, 50, 0, -82.7485, 2.8615},
          {50, 0, 0, 50, -1, 2, 2.3669},
          {50, 2.49, -.001, 50, -2.49, .0009, 7.1792},
          {50, 2.49, -.001, 50, -2.49, .0011, 7.2195},
          {50, -.001, 2.49, 50, .0011, -2.49, 4.7461}};
      for (const auto &p : pairs) {
        BrushMetrics::Lab a{p[0], p[1], p[2]}, b{p[3], p[4], p[5]};
        require(std::abs(BrushMetrics::deltaE00(a, b) - p[6]) < .0001 &&
                    std::abs(BrushMetrics::deltaE00(b, a) - p[6]) < .0001,
                "CIEDE2000 calibration failed");
        require(BrushMetrics::deltaE00(a, a) == 0,
                "Identical colors have nonzero difference");
      }
      auto black = BrushMetrics::srgbLab(0, 0, 0);
      auto white = BrushMetrics::srgbLab(1, 1, 1);
      require(std::abs(black.l) < 1e-8 && std::abs(white.l - 100) < 1e-4 &&
                  std::abs(white.a) < 1e-4 && std::abs(white.b) < 1e-4,
              "sRGB/D65 neutral calibration failed");
      return 0;
    }
    require(argc == 2, "Expected reference run directory");
    QDir root(QString::fromLocal8Bit(argv[1]));
    QFile status(root.filePath("output/oracle-status.json"));
    require(status.open(QIODevice::ReadOnly),
            "Source reference status missing");
    auto oracle = QJsonDocument::fromJson(status.readAll()).object();
    require(oracle["status"].toString() == "complete" &&
                oracle["version"].toString() == "5.3.4.1",
            "Source reference incomplete or wrong version");
    QJsonArray results;
    QStringList failedHardCases;
    QFile manifest(root.filePath("output/fixtures.json"));
    require(manifest.open(QIODevice::ReadOnly), "Fixture manifest missing");
    auto fixtures = QJsonDocument::fromJson(manifest.readAll()).object();
    auto expected = fixtures["presets"].toArray();
    require(!expected.empty(), "Empty fixture manifest");
    QStringList remaining;
    for (const auto &name : expected)
      remaining.append(name.toString());
    QImage initial(2048, 2048, QImage::Format_ARGB32);
    const QRgb colors[] = {qRgba(0, 0, 255, 255), qRgba(0, 255, 0, 128),
                           qRgba(255, 0, 0, 255), qRgba(0, 0, 0, 0)};
    for (int y = 0; y < initial.height(); ++y)
      for (int x = 0; x < initial.width(); ++x)
        initial.setPixel(x, y, colors[(x / 128) % 4]);
    require(initial.save(root.filePath("output/initial.png")),
            "Cannot save initial raster");
    const QImage stripInitial = initial;
    for (const auto &value : oracle["records"].toArray()) {
      auto source = value.toObject();
      initial = stripInitial;
      if (source["background"].toString() == "transparent")
        initial.fill(Qt::transparent);
      if (source["background"].toString() == "wet-halves")
        for (int y = 0; y < initial.height(); ++y)
          for (int x = 0; x < initial.width(); ++x)
            initial.setPixel(x, y,
                             x < source["wetBoundary"].toDouble(336)
                                 ? qRgba(0, 0, 255, 255)
                                 : qRgba(255, 0, 0, 128));
      if (source["wetPattern"].toString() == "two-pixel-red-yellow")
        for (int y = 0; y < initial.height(); ++y)
          for (int x = int(std::ceil(source["wetBoundary"].toDouble(336)));
               x < initial.width(); ++x)
            initial.setPixel(x, y,
                             (x / 2) % 2 == 0 ? qRgba(255, 0, 0, 128)
                                              : qRgba(255, 255, 0, 255));
      if (source.contains("wetThirdBoundary")) {
        const auto c = source["wetThirdRGBA8"].toArray();
        for (int y = 0; y < initial.height(); ++y)
          for (int x = source["wetThirdBoundary"].toInt(); x < initial.width();
               ++x)
            initial.setPixel(
                x, y,
                qRgba(c[0].toInt(), c[1].toInt(), c[2].toInt(), c[3].toInt()));
      }
      QString name = source["preset"].toString(), error;
      require(remaining.removeOne(name), "Unexpected or duplicate source case");
      auto imported = importBrushFile(
          root.filePath("data/krita/paintoppresets/" + name + ".kpp"), error);
      require(error.isEmpty() && imported.records.size() == 1,
              "Candidate import failed: " + error);
      Preset p = imported.records.front().preset;
      const bool sourceHard = fixtures["group"].toString() == "hardpixel" ||
                              fixtures["group"].toString() == "pixelopacity" ||
                              fixtures["group"].toString() == "pixelpressure" ||
                              (fixtures["group"].toString() == "geometry" &&
                               name.endsWith("antialias 0"));
      if (sourceHard)
        require(p.profile == "krita-5.3.4.1-hard-pixel-candidate-1",
                "Strict source hard-pixel eligibility did not select the "
                "candidate");
      const bool sourceWet =
          (fixtures["group"].toString() == "wetsteps" ||
           fixtures["group"].toString() == "wethold" ||
           fixtures["group"].toString() == "wetstate" ||
           ((fixtures["group"].toString() == "wettexture" ||
             fixtures["group"].toString() == "wettexture3") &&
            (name.contains(" smear ") ||
             source["overrides"].toObject()["SmudgeRadiusValue"].toString() ==
                 "0")));
      if (sourceWet) {
        const QString model =
            imported.records.front().provenance["sourceSmudgeModel"].toString();
        require(p.profile == "krita-5.3.4.1-" + model.toStdString() +
                                 "-candidate-1" &&
                    p.smudgeRadius == 0 && p.reload == 0,
                "Strict source wet eligibility did not select the matching "
                "candidate");
      }
      p.size.value = source["diameter"].toDouble();
      p.color = {220 / 255.0, 40 / 255.0, 30 / 255.0, 1};
      if (source.contains("foregroundRGBA8")) {
        auto color = source["foregroundRGBA8"].toArray();
        require(color.size() == 4, "Invalid fixture foreground");
        p.color = pixel(qRgba(color[0].toInt(), color[1].toInt(),
                              color[2].toInt(), color[3].toInt()));
      }
      p.opacity.value = source["opacity"].toDouble();
      p.flow.value = source["flow"].toDouble();
      Canvas canvas(initial);
      Stroke stroke(canvas, p);
      Sample first, last;
      first.x = source["start"].toArray()[0].toDouble();
      first.y = source["start"].toArray()[1].toDouble();
      first.hasPressure = true;
      first.pressure = source["pressure"].toArray()[0].toDouble();
      last = first;
      last.x = source["end"].toArray()[0].toDouble();
      last.y = source["end"].toArray()[1].toDouble();
      last.pressure = source["pressure"].toArray()[1].toDouble();
      last.time = 1;
      QElapsedTimer timer;
      timer.start();
      if (source.contains("events")) {
        auto events = source["events"].toArray();
        require(!events.empty() && events.size() <= 4096,
                "Invalid bounded event trace");
        for (const auto &value : events) {
          auto event = value.toObject();
          Sample sample;
          sample.x = event["x"].toDouble();
          sample.y = event["y"].toDouble();
          sample.pressure = event["pressure"].toDouble();
          sample.tiltX = event["tiltX"].toDouble() / 60;
          sample.tiltY = event["tiltY"].toDouble() / 60;
          sample.rotation =
              event["rotation"].toDouble() * 3.14159265358979323846 / 180;
          sample.time = event["timeUs"].toDouble() / 1000000;
          sample.hasPressure = sample.hasTilt = sample.hasRotation = true;
          require(stroke.add(sample), QString::fromStdString(stroke.error()));
        }
      } else {
        require(stroke.add(first) && stroke.add(last),
                QString::fromStdString(stroke.error()));
      }
      stroke.finish();
      double elapsed = timer.nsecsElapsed() / 1e6;
      QImage candidate = canvas.image();
      QString basename = source["image"].toString();
      QImage reference(root.filePath("output/" + basename));
      require(!reference.isNull(), "Cannot decode source image");
      require(candidate.save(root.filePath("output/native-" + basename)),
              "Cannot save candidate image");
      auto metrics =
          compare(initial, reference.convertToFormat(QImage::Format_ARGB32),
                  candidate, root.filePath("output/diff-" + basename));
      metrics["preset"] = name;
      metrics["inputCase"] = source;
      metrics["candidateProfile"] = QString::fromStdString(p.profile);
      metrics["sourceSemanticallySupported"] = false;
      metrics["sourceReferenceVerified"] = false;
      if (sourceHard) {
        const bool alphaCoverage = metrics["transparentBase"].toBool();
        auto rb =
            metrics[alphaCoverage ? "referenceAlphaBounds" : "referenceBounds"]
                .toArray();
        auto cb =
            metrics[alphaCoverage ? "candidateAlphaBounds" : "candidateBounds"]
                .toArray();
        const double boundsError =
            std::max({std::abs(rb[0].toDouble() - cb[0].toDouble()),
                      std::abs(rb[1].toDouble() - cb[1].toDouble()),
                      std::abs(rb[0].toDouble() + rb[2].toDouble() -
                               cb[0].toDouble() - cb[2].toDouble()),
                      std::abs(rb[1].toDouble() + rb[3].toDouble() -
                               cb[1].toDouble() - cb[3].toDouble())});
        bool passed =
            metrics["premultipliedRGBMAE"].toDouble() <= 2 / 255.0 &&
            metrics["alphaMAE"].toDouble() <= 1 / 255.0 &&
            metrics["alphaP99"].toDouble() <= 4 / 255.0 &&
            metrics[alphaCoverage ? "alphaCoverageIoU" : "changedPixelIoU"]
                    .toDouble() >= .98 &&
            boundsError <= std::max(1.0, source["diameter"].toDouble() * .02) &&
            !metrics["emptyCandidateFailure"].toBool();
        if (fixtures["group"].toString() == "geometry")
          passed = passed && metrics["maxRGBError"].toDouble() == 0 &&
                   metrics["maxAlphaError"].toDouble() == 0;
        metrics["hardPixelCandidatePassed"] = passed;
        metrics["boundsError"] = boundsError;
        if (!passed)
          failedHardCases.append(name);
      }
      if (sourceWet) {
        const bool passed =
            metrics["maxRGBError"].toDouble() <= 2 / 255.0 &&
            metrics["maxAlphaError"].toDouble() <= 1 / 255.0 &&
            metrics["visibleChangedPixelIoU"].toDouble() >= .98 &&
            !metrics["emptyCandidateFailure"].toBool();
        metrics["wetCandidatePassed"] = passed;
        if (!passed)
          failedHardCases.append(name);
      }
      metrics["renderMilliseconds"] = elapsed;
      metrics["dabs"] = double(stroke.dabCount());
      metrics["diagnostics"] =
          QJsonArray::fromStringList(imported.records.front().diagnostics);
      results.append(metrics);
    }
    require(remaining.empty() && results.size() == expected.size(),
            "Incomplete comparison workload");
    QFile report(root.filePath("output/comparison.json"));
    require(report.open(QIODevice::WriteOnly),
            "Cannot write comparison report");
    report.write(
        QJsonDocument(
            QJsonObject{{"status", "measured-conversions-not-qualified"},
                        {"records", results}})
            .toJson());
    std::cout << "Measured " << results.size()
              << " native conversion cases; source parity is not granted.\n";
    require(failedHardCases.empty(),
            "Experimental K5341 source-candidate fidelity gates failed: " +
                failedHardCases.join(", "));
  } catch (const std::exception &e) {
    std::cerr << "::error::Brush comparison failed: " << e.what() << '\n';
    return 1;
  }
}
