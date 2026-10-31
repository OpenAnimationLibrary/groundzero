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
#include <QRegularExpression>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <utility>

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
void applyViewRotation(Preset &preset, const QJsonObject &source) {
  if (!source.contains("brushRotationDegrees"))
    return;
  const auto modifier = source["brushRotationDegrees"];
  require(modifier.isDouble() && std::isfinite(modifier.toDouble()) &&
              std::abs(modifier.toDouble()) <= 36000,
          "Invalid source view brush rotation");
  // Imported fixed KPP angles already use raster-coordinate sign. Positive
  // source View rotation is counterclockwise: the original asymmetric8x8
  // resource's right edge becomes its top edge in the90degree capture.
  preset.angle.value -= modifier.toDouble();
}
void testViewRotation() {
  auto tip = std::make_shared<Image>();
  tip->width = tip->height = 3;
  tip->color = true;
  tip->pixels.resize(9);
  tip->pixels[5] = {1, 0, 0,
                    1}; // An off-center mark exposes rotation direction.
  auto render = [&](double serializedAngle, double viewAngle) {
    Preset preset;
    preset.tip.shape = Shape::Bitmap;
    preset.tip.images = {tip};
    preset.size.value = 12;
    preset.angle.value = serializedAngle;
    applyViewRotation(preset, QJsonObject{{"brushRotationDegrees", viewAngle}});
    QImage blank(64, 64, QImage::Format_ARGB32);
    blank.fill(Qt::transparent);
    Canvas canvas(blank);
    Stroke stroke(canvas, preset);
    Sample sample;
    sample.x = sample.y = 32;
    require(stroke.add(sample) && stroke.finish(),
            QString::fromStdString(stroke.error()));
    return canvas.image();
  };
  auto center = [](const QImage &image) {
    double mass = 0, x = 0, y = 0;
    for (int row = 0; row < image.height(); ++row)
      for (int col = 0; col < image.width(); ++col) {
        const double a = qAlpha(image.pixel(col, row));
        mass += a;
        x += (col + .5) * a;
        y += (row + .5) * a;
      }
    require(mass > 0, "Empty view-rotation regression stamp");
    return std::make_pair(x / mass, y / mass);
  };
  const auto plain = render(0, 0), quarter = render(0, 90);
  const auto p = center(plain), q = center(quarter),
             oblique = center(render(0, 40));
  require(p.first > 34 && std::abs(p.second - 32) < .1 && q.second < 30 &&
              std::abs(q.first - 32) < .1 && plain != quarter &&
              oblique.first > 33 && oblique.second < 31,
          "Source view rotation did not rotate asymmetric candidate "
          "counterclockwise");
  require(render(-37, 53) == quarter && render(23, -17) == render(40, 0),
          "View rotation replaced or mis-signed the serialized brush angle");
}
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
      testViewRotation();
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
    std::set<int> stochasticReplicates;
    QStringList failedHardCases;
    QFile manifest(root.filePath("output/fixtures.json"));
    require(manifest.open(QIODevice::ReadOnly), "Fixture manifest missing");
    auto fixtures = QJsonDocument::fromJson(manifest.readAll()).object();
    if (fixtures["group"].toString() == "gihstatistics" ||
        fixtures["group"].toString() == "gihlabels") {
      const auto statistics = oracle["resourceStatistics"].toObject();
      require(statistics["strokes"].toInt() == 100 &&
                  statistics["sourceSequences"].toArray().size() == 100 &&
                  statistics["observedDabs"].toInt() > 0 &&
                  statistics["seedParityClaimed"].isBool() &&
                  !statistics["seedParityClaimed"].toBool(),
              "Incomplete or mislabeled random GIH acquisition");
    }
    auto expected = fixtures["presets"].toArray();
    require(!expected.empty(), "Empty fixture manifest");
    if (fixtures["group"].toString() == "tabletsensors")
      require(expected.size() == 188,
              "Incomplete tablet sensor fixture matrix");
    if (fixtures["group"].toString() == "spacingdynamics")
      require(expected.size() == 112,
              "Incomplete spacing sensor fixture matrix");
    if (fixtures["group"].toString() == "mirrortips")
      require(expected.size() == 42, "Incomplete mirror fixture matrix");
    if (fixtures["group"].toString() == "wethsv")
      require(expected.size() == 96, "Incomplete wet HSV acquisition matrix");
    if (fixtures["group"].toString() == "hsvpressure")
      require(expected.size() == 24, "Incomplete pressure HSV heldout matrix");
    if (fixtures["group"].toString() == "hsvchroma")
      require(expected.size() == 36,
              "Incomplete low-chroma HSV heldout matrix");
    if (fixtures["group"].toString() == "hsvorder")
      require(expected.size() == 24,
              "Incomplete order-discriminating HSV matrix");
    if (fixtures["group"].toString() == "hsvjoint")
      require(expected.size() == 36, "Incomplete joint HSV acquisition matrix");
    if (fixtures["group"].toString() == "hsvheldout")
      require(expected.size() == 54,
              "Incomplete heldout HSV acquisition matrix");
    if (fixtures["group"].toString() == "hsvdynamics")
      require(expected.size() == 72, "Incomplete HSV acquisition matrix");
    if (fixtures["group"].toString() == "distancesensors")
      require(expected.size() == 32, "Incomplete distance acquisition matrix");
    if (fixtures["group"].toString() == "gihcadence")
      require(expected.size() == 144,
              "Incomplete GIH cadence acquisition matrix");
    if (fixtures["group"].toString() == "maskwash" ||
        fixtures["group"].toString() == "maskbuildup")
      require(expected.size() == 156,
              "Incomplete masked-brush acquisition matrix");
    if (fixtures["group"].toString() == "airbrushtimed")
      require(expected.size() == 56, "Incomplete paced airbrush workload");
    if (fixtures["group"].toString() == "timedsensors")
      require(expected.size() == 28, "Incomplete paced sensor workload");
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
      if (fixtures["group"].toString() == "hsvjoint" ||
          fixtures["group"].toString() == "hsvorder" ||
          fixtures["group"].toString() == "hsvchroma") {
        const auto evidence = source["sensorEvidence"].toObject();
        require(evidence["deliveryValuesMatchAuthored"].toBool() &&
                    evidence["deliveryInterface"].toString() ==
                        "Node.paintLine" &&
                    evidence["deliveredContactEvents"].toArray().size() == 2 &&
                    source["jointHsvProbe"]
                        .toObject()["operationOrderAssumed"]
                        .isBool() &&
                    !source["jointHsvProbe"]
                         .toObject()["operationOrderAssumed"]
                         .toBool(),
                "Missing independent joint HSV acquisition evidence");
        const auto controls = evidence["effectiveControls"].toObject();
        const auto requested = source["overrides"].toObject();
        for (const QString &channel :
             {QString("h"), QString("s"), QString("v")})
          for (const QString &suffix :
               {QString("UseCurve"), QString("UseSameCurve"), QString("Value"),
                QString("curveMode"), QString("commonCurve")})
            require(controls[channel + suffix] == requested[channel + suffix],
                    "Joint HSV control readback differs");
        const auto foreground =
            evidence["effectiveForegroundDisplayRGBA8"].toArray();
        const auto authored = source["foregroundRGBA8"].toArray();
        require(foreground.size() == 4 && authored.size() == 4,
                "Missing joint HSV foreground readback");
        for (int c = 0; c < 4; ++c)
          require(std::abs(foreground[c].toInt() - authored[c].toInt()) <= 1,
                  "Joint HSV foreground readback differs");
      }
      if (fixtures["group"].toString() == "hsvpressure") {
        const auto evidence = source["sensorEvidence"].toObject();
        const auto controls = evidence["effectiveControls"].toObject();
        const QString target =
            source["sensorProbe"].toObject()["target"].toString();
        require((target == "h" || target == "s" || target == "v") &&
                    evidence["deliveryValuesMatchAuthored"].toBool() &&
                    evidence["deliveryInterface"].toString() ==
                        "Node.paintLine" &&
                    controls[target + "commonCurve"].toString() == "0,0;1,1;",
                "Missing dynamic HSV source input/readback");
        const auto foreground =
            evidence["effectiveForegroundDisplayRGBA8"].toArray();
        const auto authored = source["foregroundRGBA8"].toArray();
        require(foreground.size() == 4 && authored.size() == 4,
                "Missing dynamic HSV foreground readback");
        for (int c = 0; c < 4; ++c)
          require(std::abs(foreground[c].toInt() - authored[c].toInt()) <= 1,
                  "Dynamic HSV foreground differs");
      }
      const bool wetHsvCase = fixtures["group"].toString() == "wethsv";
      if (wetHsvCase) {
        const auto evidence = source["sensorEvidence"].toObject();
        const auto controls = evidence["effectiveControls"].toObject();
        const auto spec = source["wetHsvProbe"].toObject();
        const auto requested = source["overrides"].toObject();
        require(evidence["deliveryValuesMatchAuthored"].toBool() &&
                    evidence["deliveryInterface"].toString() ==
                        "Node.paintLine" &&
                    evidence["deliveredContactEvents"].toArray().size() == 2 &&
                    spec["colorRate"].toDouble() == .25 &&
                    requested["ColorRateValue"].toString() == "0.25",
                "Missing wet HSV input/control evidence");
        const QString target = spec["channel"].toString();
        require(target == "h" || target == "s" || target == "v",
                "Invalid wet HSV channel");
        for (const QString &channel :
             {QString("h"), QString("s"), QString("v")})
          require(controls["Pressure" + channel] ==
                      requested["Pressure" + channel],
                  "Wet HSV enabled state differs");
        require(controls[target + "commonCurve"] ==
                    requested[target + "commonCurve"],
                "Wet HSV fixed curve differs");
        const auto foreground =
            evidence["effectiveForegroundDisplayRGBA8"].toArray();
        const auto authored = source["foregroundRGBA8"].toArray();
        require(foreground.size() == 4 && authored.size() == 4,
                "Missing wet HSV foreground readback");
        for (int c = 0; c < 4; ++c)
          require(std::abs(foreground[c].toInt() - authored[c].toInt()) <= 1,
                  "Wet HSV foreground readback differs");
      }
      const bool maskCase = fixtures["group"].toString() == "maskwash" ||
                            fixtures["group"].toString() == "maskbuildup";
      if (maskCase) {
        const auto evidence = source["maskEvidence"].toObject();
        require(
            evidence["nestedMetadataVerified"].toBool() &&
                evidence["deliveryVerified"].toBool() &&
                !evidence["effectiveControls"].toObject().empty() &&
                evidence["externalResourceDependencies"].isArray() &&
                evidence["externalResourceDependencies"].toArray().empty() &&
                evidence["sourceParityQualified"].isBool() &&
                !evidence["sourceParityQualified"].toBool(),
            "Missing qualified nested masking metadata or line evidence");
        const auto controls = evidence["effectiveControls"].toObject();
        const auto requested = source["overrides"].toObject();
        for (auto it = requested.begin(); it != requested.end(); ++it)
          if (!it.key().endsWith("brush_definition"))
            require(controls[it.key()] == it.value(),
                    "Masked acquisition effective setting differs");
        require(!evidence["data"].toString().isEmpty() &&
                    evidence["rect"].toArray().size() == 4,
                "Missing masked acquisition region");
        if (source["maskProbe"].toObject()["mode"].toString() == "off")
          require(evidence["nonzeroAlphaPixels"].toInt() > 0,
                  "Unmasked paired source control did not paint");
      }
      const bool sensorCase =
          fixtures["group"].toString() == "tabletsensors" ||
          fixtures["group"].toString() == "spacingdynamics" ||
          fixtures["group"].toString() == "mirrortips" ||
          fixtures["group"].toString() == "hsvdynamics" ||
          fixtures["group"].toString() == "hsvheldout" ||
          fixtures["group"].toString() == "distancesensors";
      QJsonArray deliveredSensorEvents;
      if (sensorCase) {
        const auto evidence = source["sensorEvidence"].toObject();
        require(evidence["deliveryValuesMatchAuthored"].isBool() &&
                    evidence["deliveryValuesMatchAuthored"].toBool() &&
                    evidence["effectiveControls"].isObject() &&
                    !evidence["effectiveControls"].toObject().empty(),
                "Missing effective sensor controls or delivered values");
        if (fixtures["group"].toString() == "mirrortips") {
          const auto controls = evidence["effectiveControls"].toObject();
          const auto mirror = source["mirrorProbe"].toObject();
          require(evidence["mirrorAxisValuesVerified"].toBool() &&
                      controls["HorizontalMirrorEnabled"].toString() ==
                          (mirror["horizontal"].toBool() ? "true" : "false") &&
                      controls["VerticalMirrorEnabled"].toString() ==
                          (mirror["vertical"].toBool() ? "true" : "false"),
                  "Missing effective Mirror-axis evidence");
        }
        if (fixtures["group"].toString() == "hsvdynamics" ||
            fixtures["group"].toString() == "hsvheldout" ||
            fixtures["group"].toString() == "distancesensors") {
          const auto foreground =
              evidence["effectiveForegroundDisplayRGBA8"].toArray();
          const auto requested = source["foregroundRGBA8"].toArray();
          require(foreground.size() == 4 && requested.size() == 4,
                  "Missing source foreground readback");
          for (int c = 0; c < 4; ++c)
            require(std::abs(foreground[c].toInt() - requested[c].toInt()) <= 1,
                    "Source foreground readback differs");
        }
        const auto authored = source["events"].toArray();
        deliveredSensorEvents = evidence["deliveredContactEvents"].toArray();
        require(authored.size() == 33 &&
                    deliveredSensorEvents.size() == authored.size(),
                "Incomplete sensor contact trace");
        const double timeBase =
            deliveredSensorEvents[0].toObject()["timeMs"].toDouble();
        for (int i = 0; i < authored.size(); ++i) {
          const auto wanted = authored[i].toObject();
          const auto actual = deliveredSensorEvents[i].toObject();
          const auto image = actual["image"].toArray(),
                     tilt = actual["tilt"].toArray();
          require(image.size() == 2 && tilt.size() == 2,
                  "Incomplete delivered tablet vector");
          for (const auto &pair :
               {std::make_pair(image[0].toDouble(), wanted["x"].toDouble()),
                std::make_pair(image[1].toDouble(), wanted["y"].toDouble()),
                std::make_pair(tilt[0].toDouble(), wanted["tiltX"].toDouble()),
                std::make_pair(tilt[1].toDouble(), wanted["tiltY"].toDouble()),
                std::make_pair(actual["pressure"].toDouble(),
                               wanted["pressure"].toDouble()),
                std::make_pair(actual["rotation"].toDouble(),
                               wanted["rotation"].toDouble())})
            require(std::isfinite(pair.first) &&
                        std::abs(pair.first - pair.second) <= 1e-7,
                    "Candidate/source sensor inputs differ");
          require(
              std::abs((actual["timeMs"].toDouble() - timeBase) -
                       wanted["timeUs"].toDouble() / 1000) <= .500001,
              "Delivered sensor clock differs beyond millisecond quantization");
        }
        const auto spec = source["sensorProbe"].toObject();
        bool zeroEndpoint = spec["target"].toString() == "Ratio" &&
                            spec["sensor"].toString() == "declination" &&
                            spec["enabled"].toBool() &&
                            source["diameter"].toDouble() == 24 &&
                            source["spacing"].toDouble() == 1;
        for (const auto &value : deliveredSensorEvents) {
          const auto event = value.toObject();
          const auto tilt = event["tilt"].toArray();
          zeroEndpoint = zeroEndpoint && tilt[0].toDouble() == 60 &&
                         tilt[1].toDouble() == 0 &&
                         event["pressure"].toDouble() == 1;
        }
        require(evidence["knownZeroRatioEndpoint"].isBool() &&
                    evidence["knownZeroRatioEndpoint"].toBool() ==
                        zeroEndpoint &&
                    source["expectUnchanged"].toBool() == zeroEndpoint &&
                    (zeroEndpoint ? evidence["nonzeroAlphaPixels"].toInt() == 0
                                  : evidence["nonzeroAlphaPixels"].toInt() > 0),
                "Sensor raster disagrees with qualified zero endpoint or "
                "nonempty requirement");
      }
      initial = stripInitial;
      if (source["background"].toString() == "wet-drift")
        for (int y = 0; y < initial.height(); ++y)
          for (int x = 0; x < initial.width(); ++x) {
            const bool red = (x / 64) % 2 == 0;
            initial.setPixel(x, y, qRgba(red ? 255 : 0, 0, red ? 0 : 255,
                                        std::min(255, y * 255 / 1280)));
          }
      if (source["background"].toString() == "transparent")
        initial.fill(Qt::transparent);
      if (source["background"].toString() == "blend-stripes") {
        const QRgb swatches[] = {
            qRgba(32, 160, 224, 255),  qRgba(192, 64, 96, 128),
            qRgba(128, 128, 128, 255), qRgba(224, 192, 32, 64),
            qRgba(64, 224, 160, 255),  qRgba(160, 32, 224, 192),
            qRgba(0, 0, 0, 0),         qRgba(255, 255, 255, 255)};
        for (int y = 0; y < initial.height(); ++y)
          for (int x = 0; x < initial.width(); ++x)
            initial.setPixel(x, y, swatches[(x / 8) % 8]);
      }
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
      if (source["wetPattern"].toString() == "opaque-black-white-column")
        for (int y = 0; y < initial.height(); ++y)
          for (int x = 0; x < initial.width(); ++x)
            initial.setPixel(x, y,
                             x >= source["wetBoundary"].toDouble(416)
                                 ? qRgba(0, 0, 0, 0)
                             : x == 256 + source["wetKernelOffset"].toInt()
                                 ? qRgba(255, 255, 255, 255)
                                 : qRgba(0, 0, 0, 255));
      if (source["wetPattern"].toString() == "opaque-black-white-row")
        for (int y = 0; y < initial.height(); ++y)
          for (int x = 0; x < initial.width(); ++x)
            initial.setPixel(x, y,
                             x >= source["wetBoundary"].toDouble(384)
                                 ? qRgba(0, 0, 0, 0)
                             : y == 1024 + source["wetKernelOffset"].toInt()
                                 ? qRgba(255, 255, 255, 255)
                                 : qRgba(0, 0, 0, 255));
      if (source["wetPattern"].toString() == "opaque-black-white-point") {
        const int pointX = 256 + source["wetKernelOffset"].toInt(),
                  pointY = 1024 + source["wetKernelOffsetY"].toInt();
        for (int y = 0; y < initial.height(); ++y)
          for (int x = 0; x < initial.width(); ++x)
            initial.setPixel(
                x, y,
                x >= source["wetBoundary"].toDouble() ? qRgba(0, 0, 0, 0)
                : x == pointX && y == pointY ? qRgba(255, 255, 255, 255)
                                             : qRgba(0, 0, 0, 255));
      }
      if (source["wetPattern"].toString() == "opaque-black-white-points") {
        const int base = source["wetKernelComplement"].toBool() ? 255 : 0;
        for (int y = 0; y < initial.height(); ++y)
          for (int x = 0; x < initial.width(); ++x)
            initial.setPixel(x, y,
                             x >= source["wetBoundary"].toDouble()
                                 ? qRgba(0, 0, 0, 0)
                                 : qRgba(base, base, base, 255));
        for (const auto &value : source["wetKernelPoints"].toArray()) {
          const auto point = value.toArray();
          initial.setPixel(256 + point[0].toInt(), 1024 + point[1].toInt(),
                           qRgba(255 - base, 255 - base, 255 - base, 255));
        }
      }
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
      const bool resourceCase =
          fixtures["group"].toString() == "bitmaptips" ||
          fixtures["group"].toString() == "gihstatistics" ||
          fixtures["group"].toString() == "gihlabels" ||
          fixtures["group"].toString() == "gihcadence" ||
          (sensorCase && source["resourceProbe"].isObject());
      QString package = "data/krita/paintoppresets/" + name + ".kpp";
      if (resourceCase) {
        package = source["candidatePackage"].toString();
        require(QRegularExpression(
                    "^output/resource-packages/resource-[0-9]{3}\\.bundle$")
                    .match(package)
                    .hasMatch(),
                "Invalid original resource package path");
        require(source["resourceResolution"].isObject() &&
                    source["resourceReadback"].isObject(),
                "Missing actual resource resolution/readback evidence");
      }
      auto imported = importBrushFile(root.filePath(package), error);
      require(error.isEmpty() && imported.records.size() == 1,
              "Candidate import failed: " + error);
      Preset p = imported.records.front().preset;
      const bool textureGate = fixtures["group"].toString() == "texturemasks";
      const bool sourceHsv = fixtures["group"].toString() == "hsvdynamics" ||
                             fixtures["group"].toString() == "hsvheldout" ||
                             fixtures["group"].toString() == "hsvpressure" ||
                             fixtures["group"].toString() == "hsvjoint" ||
                             fixtures["group"].toString() == "hsvorder" ||
                             fixtures["group"].toString() == "hsvchroma";
      const bool sourceHard =
          maskCase || source["sourceBarrelCandidate"].toBool(false) ||
          sourceHsv || (textureGate && name.contains("dry legacy")) ||
          fixtures["group"].toString() == "hardpixel" ||
          fixtures["group"].toString() == "pixelopacity" ||
          fixtures["group"].toString() == "pixelopacitytablet" ||
          fixtures["group"].toString() == "pixelcontrols" ||
          fixtures["group"].toString() == "pixelpressure" ||
          fixtures["group"].toString() == "pixelsmallpressure" ||
          fixtures["group"].toString() == "pixelshape" ||
          fixtures["group"].toString() == "pixelprecision" ||
          fixtures["group"].toString() == "pixelcurves" ||
          (fixtures["group"].toString() == "pixeltransfer" &&
           source["variant"].toString().endsWith("Node control")) ||
          (fixtures["group"].toString() == "pixelcurveknots" &&
           !source["variant"].toString().contains("disabled")) ||
          (fixtures["group"].toString() == "pixelblend" &&
           p.tip.shape == Shape::Round && p.tip.hardness == 1 &&
           !p.tip.antialiasEdges) ||
          (fixtures["group"].toString() == "geometry" &&
           name.endsWith("antialias 0"));
      if (sourceHard)
        require(p.profile == "krita-5.3.4.1-hard-pixel-candidate-1",
                "Strict source hard-pixel eligibility did not select the "
                "candidate");
      if (maskCase) {
        const auto settings = source["overrides"].toObject();
        const bool active =
            fixtures["group"].toString() == "maskwash" &&
            settings["MaskingBrush/Enabled"].toString() == "true";
        require(p.sourceMasked.enabled == active &&
                    p.sourceMasked.flow ==
                        settings["MaskingBrush/Preset/FlowValue"]
                            .toString()
                            .toDouble() &&
                    p.sourceMasked.opacity ==
                        settings["MaskingBrush/Preset/OpacityValue"]
                            .toString()
                            .toDouble(),
                "Source mask importer lost separate flow/opacity or Wash "
                "eligibility");
      }
      if (sourceHsv) {
        const auto fields = source["overrides"].toObject();
        for (int channel = 0; channel < 3; ++channel) {
          const QString prefix = QString("hsv").mid(channel, 1);
          const bool enabled = fields["Pressure" + prefix].toString() == "true";
          const bool legacy =
              p.sourceHsvChannel == SourceHsvChannel(channel + 1);
          require(enabled == (legacy || p.sourceHsvV2[channel].enabled),
                  "Source HSV import lost enabled channel");
          if (!enabled)
            continue;
          const auto points = fields[prefix + "commonCurve"].toString().split(
              ';', Qt::SkipEmptyParts);
          require(points.size() == 2, "Unsupported source HSV fixture curve");
          bool ok0 = false, ok1 = false;
          const double first = points[0].section(',', 1, 1).toDouble(&ok0),
                       last = points[1].section(',', 1, 1).toDouble(&ok1);
          require(ok0 && ok1, "Invalid source HSV fixture curve");
          const auto &control = p.sourceHsvV2[channel].control;
          if (first == last)
            require(legacy ? p.sourceHsvValue == first
                           : control.mappings.empty() && control.value == first,
                    "Source HSV import lost fixed control");
          else
            require(!legacy && first == 0 && last == 1 && control.value == 1 &&
                        control.mappings.size() == 1 &&
                        control.mappings[0].sensor == Sensor::Pressure,
                    "Source HSV import lost measured pressure control");
        }
      }
      const bool sourceSoft =
          (fixtures["group"].toString() == "pixelblend" &&
           (p.tip.shape == Shape::Square || p.tip.hardness < 1 ||
            p.tip.antialiasEdges)) ||
          ((fixtures["group"].toString() == "pixelsoft" ||
            fixtures["group"].toString() == "pixelsoftsize" ||
            fixtures["group"].toString() == "pixelrectsize") &&
           (name.contains("soft rect ") ||
            (name.contains("soft circle ") &&
             (p.tip.hardness < 1 || p.tip.antialiasEdges)))) ||
          ((fixtures["group"].toString() == "geometry" ||
            fixtures["group"].toString() == "pixelaarange") &&
           p.tip.antialiasEdges);
      if (sourceSoft)
        require(p.profile == (p.tip.shape == Shape::Square
                                  ? "krita-5.3.4.1-default-rect-candidate-1"
                                  : "krita-5.3.4.1-default-circle-candidate-1"),
                "Strict source default-mask eligibility did not select "
                "candidate: " +
                    name);
      const bool kernelCount = fixtures["group"].toString() == "wetkernelcount";
      const double kernelDiameter = source["diameter"].toDouble();
      const double kernelRadius = source["overrides"]
                                      .toObject()["SmudgeRadiusValue"]
                                      .toString()
                                      .toDouble();
      const bool kernelCountGate =
          kernelCount && kernelDiameter == std::floor(kernelDiameter) &&
          (name.contains("dulling new") || kernelRadius <= 100);
      if (kernelCount)
        require(
            source["sourceCandidateGate"].isBool() &&
                source["sourceCandidateGate"].toBool() == kernelCountGate,
            "Kernel probe qualification metadata disagrees with eligibility");
      if (textureGate && name.contains("dulling new"))
        require(p.profile == "groundzero-native-1",
                "Unmeasured new Dulling texture gained source eligibility");
      const bool sourceWet =
          (textureGate && !name.contains("dry legacy") &&
           !name.contains("dulling new")) ||
          (fixtures["group"].toString() == "wetsteps" ||
           fixtures["group"].toString() == "wethold" ||
           fixtures["group"].toString() == "wetstate" ||
           fixtures["group"].toString() == "wetswitches" ||
           fixtures["group"].toString() == "wetcombined" ||
           fixtures["group"].toString() == "wetpressure" ||
           fixtures["group"].toString() == "wetopacitycombo" ||
           fixtures["group"].toString() == "wetboundary" || kernelCountGate ||
           fixtures["group"].toString() == "wettransfer" ||
           ((fixtures["group"].toString() == "wetkernel" ||
             fixtures["group"].toString() == "wetkernelscale" ||
             fixtures["group"].toString() == "wettexture" ||
             fixtures["group"].toString() == "wettexture3") &&
            (name.contains(" smear ") ||
             source["overrides"].toObject()["SmudgeRadiusValue"].toString() ==
                 "0" ||
             name.contains("dulling new") ||
             (name.contains("dulling legacy") &&
              source["overrides"]
                      .toObject()["SmudgeRadiusValue"]
                      .toString()
                      .toDouble() <= 100))));
      if (sourceWet) {
        const QString model =
            imported.records.front().provenance["sourceSmudgeModel"].toString();
        require(p.profile == "krita-5.3.4.1-" + model.toStdString() +
                                 "-candidate-1" &&
                    p.smudgeRadius ==
                        ((model == "legacy-dulling" || model == "new-dulling")
                             ? source["overrides"]
                                       .toObject()["SmudgeRadiusValue"]
                                       .toString()
                                       .toDouble() /
                                   100
                             : 0) &&
                    p.reload == 0,
                "Strict source wet eligibility did not select the matching "
                "candidate");
      }
      applyViewRotation(p, source);
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
      if (fixtures["group"].toString() == "gihstatistics" ||
          fixtures["group"].toString() == "gihlabels") {
        const auto probe = source["resourceProbe"].toObject();
        const double replicate = probe["replicate"].toDouble(-1);
        require(replicate >= 0 && replicate < 100 &&
                    replicate == std::floor(replicate) &&
                    stochasticReplicates.insert(int(replicate)).second &&
                    p.tip.randomSequence && p.tip.images.size() == 3,
                "Invalid stochastic candidate replicate or sequence");
        // Distinct reproducible native streams, not 100 replays of seed 1.
        // This does not presume any correspondence to the source RNG/seed.
        p.seed =
            UINT64_C(0x9e3779b97f4a7c15) * std::uint64_t(int(replicate) + 1);
      }
      if (fixtures["group"].toString() == "gihcadence") {
        const auto spec = source["cadenceProbe"].toObject();
        const int spacing = spec["spacing"].toInt(-1);
        require((spacing == 2 || spacing == 3 || spacing == 4) &&
                    spec["slots"].toInt() == 32 &&
                    source["diameter"].toDouble() == 8 &&
                    p.spacing == spacing && spec["replicate"].toInt(-1) >= 0 &&
                    spec["replicate"].toInt(-1) < 4 && p.tip.images.size() == 3,
                "Invalid bounded GIH cadence candidate settings");
        p.seed = UINT64_C(0x9e3779b97f4a7c15) *
                 std::uint64_t(spec["replicate"].toInt() + 1);
      }
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
      if (fixtures["group"].toString() == "gihcadence") {
        const auto spec = source["cadenceProbe"].toObject();
        const QString mode = spec["mode"].toString();
        const int step = 8 * spec["spacing"].toInt();
        const int end = 16 + 31 * step + 1;
        QJsonArray expectedPaths;
        if (mode == "discrete") {
          for (int i = 0; i < 32; ++i)
            expectedPaths.append(QJsonArray{QJsonArray{16 + i * step, 1024},
                                            QJsonArray{17 + i * step, 1024}});
        } else {
          require(mode == "line" || mode == "path-endpoints" ||
                      mode == "path-dabs" || mode == "path-halves" ||
                      mode == "path-offgrid",
                  "Unknown GIH cadence mode");
          const int increment = mode == "path-dabs"      ? step
                                : mode == "path-halves"  ? step / 2
                                : mode == "path-offgrid" ? 17
                                                         : end;
          QJsonArray path;
          for (int x = 16; x < end; x += increment)
            path.append(QJsonArray{x, 1024});
          path.append(QJsonArray{end, 1024});
          expectedPaths.append(path);
        }
        const auto delivered = source["deliveryReport"].toObject();
        require(source["cadencePaths"].toArray() == expectedPaths &&
                    delivered["cadencePaths"].toArray() == expectedPaths &&
                    delivered["strokeCount"].toInt() == expectedPaths.size() &&
                    delivered["mode"].toString() ==
                        (mode == "line" || mode == "discrete"
                             ? "Node.paintLine"
                             : "Node.paintPath"),
                "GIH source cadence delivery differs from authored paths");
        const auto labels =
            source["resourceReadback"].toObject()["frameSelections"].toArray();
        require(labels.size() == 32,
                "Missing bounded source cadence frame labels");
        for (const auto &label : labels)
          require(label.isNull() ||
                      (label.isDouble() && label.toDouble() >= 0 &&
                       label.toDouble() <= 2 &&
                       label.toDouble() == std::floor(label.toDouble())),
                  "Invalid source cadence label");
        for (int i = 0; i < expectedPaths.size(); ++i) {
          const auto path = expectedPaths[i].toArray();
          Preset separate = p;
          separate.seed ^= UINT64_C(0xd1b54a32d192ed03) * std::uint64_t(i + 1);
          Stroke dab(canvas, separate);
          Stroke &target = mode == "discrete" ? dab : stroke;
          for (int j = 0; j < path.size(); ++j) {
            const auto point = path[j].toArray();
            Sample sample = first;
            sample.x = point[0].toDouble();
            sample.y = point[1].toDouble();
            sample.time = j / 240.;
            require(target.add(sample), QString::fromStdString(target.error()));
          }
          if (mode == "discrete")
            require(dab.finish(), QString::fromStdString(dab.error()));
        }
      } else if (fixtures["group"].toString() == "wetdrift") {
        const auto spec = source["driftProbe"].toObject();
        const int count = spec["nominalDabs"].toInt();
        require(count == 1 || count == 10 || count == 100 || count == 1000 || count == 10000,
                "Invalid drift checkpoint");
        auto center = [](int index) {
          const int row = index / 128, column = index % 128;
          return QPoint(8 + 16 * (row % 2 ? 127 - column : column), 8 + 16 * row);
        };
        std::vector<QPoint> points{center(0)};
        for (int i = 1; i < count; ++i) {
          if (i > 1 && center(i) - center(i - 1) == center(i - 1) - center(i - 2))
            points.back() = center(i);
          else points.push_back(center(i));
        }
        points.push_back(center(count - 1) + (center(count) - center(count - 1)) / 16);
        QJsonArray expectedPath;
        for (const auto &point : points) expectedPath.append(QJsonArray{point.x(), point.y()});
        const auto delivery = source["deliveryReport"].toObject();
        const auto evidence = source["driftEvidence"].toObject();
        require(source["sourcePath"].toArray() == expectedPath &&
                    delivery["vertices"].toArray() == expectedPath &&
                    delivery["mode"].toString() == "Node.paintPath" &&
                    delivery["singleContinuousStroke"].toBool() &&
                    evidence["observedDepositCells"].toInt() == count - (spec["calibration"].toBool() ? 0 : 1) &&
                    evidence["maximumCellOffset"].toInt() <= 5,
                "Missing continuous drift geometry/count evidence");
        const QString model = spec["model"].toString();
        require(p.profile == ("krita-5.3.4.1-" +
                    (spec["calibration"].toBool() ? QString("hard-pixel") : model) + "-candidate-1").toStdString(),
                "Drift imported a native conversion instead of source candidate");
        for (size_t i = 0; i < points.size(); ++i) {
          Sample event = first;
          event.x = points[i].x(); event.y = points[i].y();
          event.time = double(i) / 240;
          require(stroke.add(event), QString::fromStdString(stroke.error()));
        }
      } else if (fixtures["group"].toString() == "gihlabels") {
        const auto path = source["sourcePath"].toArray();
        const auto delivery = source["deliveryReport"].toObject();
        require(path.size() == 96 && delivery["vertices"].toArray() == path &&
                    delivery["mode"].toString() == "Node.paintPath" &&
                    delivery["singleContinuousStroke"].toBool(),
                "Missing bounded continuous source path evidence");
        for (int i = 0; i < 96; ++i) {
          const int row = i / 2;
          const int x = ((row % 2 == 0) == (i % 2 == 0)) ? 16 : 2032;
          const int y = 16 + row * 32;
          const auto point = path[i].toArray();
          require(point.size() == 2 && point[0].toInt() == x &&
                      point[1].toInt() == y,
                  "Source path geometry differs from fixed label fixture");
          Sample sample = first;
          sample.x = x;
          sample.y = y;
          sample.time = i / 240.0;
          require(stroke.add(sample), QString::fromStdString(stroke.error()));
        }
      } else if (source.contains("events")) {
        auto events = source["events"].toArray();
        if (sensorCase) {
          events = {};
          const double timeBase =
              deliveredSensorEvents[0].toObject()["timeMs"].toDouble();
          for (const auto &value : deliveredSensorEvents) {
            const auto delivered = value.toObject();
            const auto image = delivered["image"].toArray(),
                       tilt = delivered["tilt"].toArray();
            events.append(QJsonObject{
                {"x", image[0]},
                {"y", image[1]},
                {"pressure", delivered["pressure"]},
                {"tiltX", tilt[0]},
                {"tiltY", tilt[1]},
                {"rotation", delivered["rotation"]},
                {"timeUs",
                 (delivered["timeMs"].toDouble() - timeBase) * 1000}});
          }
        }
        if (source.contains("timedProbe")) {
          const auto evidence = source["timedEvidence"].toObject();
          require(evidence["actualTimingVerified"].toBool() &&
                      evidence["sourceParityQualified"].isBool() &&
                      !evidence["sourceParityQualified"].toBool(),
                  "Missing measured paced source timing");
          auto contacts = evidence["deliveredContacts"].toArray();
          require(contacts.size() == source["events"].toArray().size() &&
                      (contacts.size() == 1 || contacts.size() == 17 ||
                       contacts.size() == 33 || contacts.size() == 65),
                  "Incomplete paced source contacts");
          const auto spec = source["timedProbe"].toObject();
          const double duration = spec["durationUs"].toDouble();
          const double maxLateness = spec["maxLatenessUs"].toDouble();
          const bool strictTiming =
              duration == 375000 || duration == 1125000 ||
              duration == 1250000 || duration == 2250000 ||
              spec["target"].toString().startsWith("speed-cadence-");
          require(maxLateness == (strictTiming ? 20000 : 250000),
                  "Unexpected paced lateness contract");
          require(duration == 250000 || duration == 375000 ||
                      duration == 1000000 || duration == 1125000 ||
                      duration == 1250000 || duration == 2250000,
                  "Unexpected paced duration");
          const auto discarded = evidence["discardedTimingAttempts"].toArray();
          require(evidence["discardedTimingAttempts"].isArray() &&
                      discarded.size() <= 1,
                  "Too many discarded timing acquisitions");
          for (const auto &item : discarded) {
            const auto attempt = item.toObject();
            const auto detail = attempt["detail"].toObject();
            const QString phase = detail["phase"].toString();
            const int index = detail["index"].toInt(-1);
            const auto authoredEvents = source["events"].toArray();
            require(attempt["attempt"].toInt() == 1 &&
                        attempt["reason"].toString() == "timing-only" &&
                        attempt["cleanup"].toString() == "release-and-leave" &&
                        (phase == "contact" || phase == "release") &&
                        index >= 0 && index <= authoredEvents.size() &&
                        (phase != "contact" || index < authoredEvents.size()) &&
                        (phase != "release" || index == authoredEvents.size()),
                    "Invalid timing-only discarded acquisition");
            const double planned =
                phase == "release"
                    ? duration
                    : authoredEvents[index].toObject()["timeUs"].toDouble();
            const double elapsed = detail["elapsedUs"].toDouble();
            const double late = detail["latenessUs"].toDouble();
            require(std::isfinite(elapsed) && elapsed == std::floor(elapsed) &&
                        detail["authoredUs"].toDouble() == planned &&
                        detail["boundUs"].toDouble() == maxLateness &&
                        late == elapsed - planned &&
                        (late > maxLateness || late < -2000),
                    "Discarded acquisition was inside the timing contract");
          }
          const double origin =
              contacts[0].toObject()["monotonicUs"].toDouble();
          const auto authored = source["events"].toArray();
          for (int i = 0; i < contacts.size(); ++i) {
            const auto contact = contacts[i].toObject();
            const double elapsed = contact["monotonicUs"].toDouble() - origin;
            const double planned = authored[i].toObject()["timeUs"].toDouble();
            require(std::isfinite(elapsed) && elapsed >= planned - 2000 &&
                        elapsed <= planned + maxLateness &&
                        contact["pressure"].toDouble() == 1,
                    "Paced contact failed actual elapsed-time bounds");
          }
          auto release = evidence["release"].toObject();
          const double held = release["monotonicUs"].toDouble() - origin;
          require(std::isfinite(held) && held >= duration - 2000 &&
                      held <= duration + maxLateness,
                  "Paced release failed actual elapsed-time bounds");
          release["pressure"] = 1;
          contacts.append(release);
          events = QJsonArray();
          for (const auto &value : contacts) {
            const auto delivered = value.toObject();
            const auto image = delivered["image"].toArray();
            require(image.size() == 2, "Incomplete paced source coordinates");
            events.append(QJsonObject{
                {"x", image[0]},
                {"y", image[1]},
                {"pressure", delivered["pressure"]},
                {"tiltX", 0},
                {"tiltY", 0},
                {"rotation", 0},
                {"timeUs", delivered["monotonicUs"].toDouble() - origin}});
          }
        }
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
      if (!stroke.finish())
        require(false, QString::fromStdString(stroke.error()));
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
      if (source.contains("timedProbe")) {
        metrics["timedAcquisitionInputsValidated"] = true;
        metrics["sourceParityQualified"] = false;
        metrics["candidateTimingConvention"] =
            "Actual monotonic source contacts plus held final position until "
            "measured release; native conversion only";
      }
      metrics["candidateProfile"] = QString::fromStdString(p.profile);
      if (fixtures["group"].toString() == "gihstatistics") {
        const auto rect = source["resourceProbe"].toObject()["rect"].toArray();
        require(rect.size() == 4, "Missing stochastic region");
        const int left = rect[0].toInt(), top = rect[1].toInt(),
                  width = rect[2].toInt(), height = rect[3].toInt();
        require(left >= 0 && top >= 0 && width > 0 && width <= 2048 &&
                    height > 0 && height <= 128 &&
                    left + width <= candidate.width() &&
                    top + height <= candidate.height(),
                "Invalid bounded stochastic region");
        QByteArray alpha;
        alpha.reserve(width * height);
        for (int y = 0; y < candidate.height(); ++y)
          for (int x = 0; x < candidate.width(); ++x) {
            const auto a = qAlpha(candidate.pixel(x, y));
            if (x >= left && x < left + width && y >= top && y < top + height)
              alpha.append(char(a));
            else
              require(a == 0, "Candidate random stroke escaped fixed region");
          }
        const int slotCount =
            source["resourceProbe"].toObject()["expectedSlots"].toInt();
        const int diameter = source["diameter"].toInt();
        require(slotCount == 32 && diameter == 8,
                "Unexpected stochastic frame-label fixture geometry");
        QJsonArray selections;
        const int radius = diameter / 2 + 2;
        for (int slot = 0; slot < slotCount; ++slot) {
          const int cx = 256 + slot * diameter * 4;
          quint64 sums[3] = {0, 0, 0};
          for (int y = 1024 - radius; y <= 1024 + radius; ++y)
            for (int x = cx - radius; x <= cx + radius; ++x) {
              const auto color = candidate.pixel(x, y);
              const auto alphaValue = qAlpha(color);
              sums[0] += qRed(color) * alphaValue;
              sums[1] += qGreen(color) * alphaValue;
              sums[2] += qBlue(color) * alphaValue;
            }
          const int winner = int(std::max_element(sums, sums + 3) - sums);
          if (!sums[winner]) {
            selections.append(QJsonValue(QJsonValue::Null));
          } else {
            for (int channel = 0; channel < 3; ++channel)
              require(
                  channel == winner || sums[channel] == 0,
                  "Candidate GIH frame is not an original primary-color tip");
            selections.append(winner);
          }
        }
        metrics["stochasticFrameSelections"] = selections;
        metrics["stochasticAlpha"] = QJsonObject{
            {"rect", rect},
            {"encoding", "zlib-base64-alpha8-row-major"},
            {"data", QString::fromLatin1(qCompress(alpha).mid(4).toBase64())}};
      }
      if (fixtures["group"].toString() == "gihlabels") {
        QJsonArray labels;
        for (int row = 0; row < 48; ++row)
          for (int col = 0; col < 64; ++col) {
            const int cx = 16 + 32 * (row % 2 == 0 ? col : 63 - col);
            const int cy = 16 + 32 * row;
            quint64 sums[3] = {0, 0, 0};
            for (int y = cy - 6; y <= cy + 6; ++y)
              for (int x = cx - 6; x <= cx + 6; ++x) {
                const auto c = candidate.pixel(x, y);
                sums[0] += qRed(c) * qAlpha(c);
                sums[1] += qGreen(c) * qAlpha(c);
                sums[2] += qBlue(c) * qAlpha(c);
              }
            const int winner = int(std::max_element(sums, sums + 3) - sums);
            if (!sums[winner])
              labels.append(QJsonValue(QJsonValue::Null));
            else {
              for (int c = 0; c < 3; ++c)
                require(c == winner || sums[c] == 0,
                        "Long candidate label cell mixes primary colors");
              labels.append(winner);
            }
          }
        metrics["stochasticFrameSelections"] = labels;
      }
      if (fixtures["group"].toString() == "gihcadence") {
        QJsonArray labels;
        for (int slot = 0; slot < 32; ++slot) {
          const int cx =
              16 +
              slot * 8 * source["cadenceProbe"].toObject()["spacing"].toInt();
          quint64 sums[3] = {0, 0, 0};
          for (int y = 1018; y <= 1030; ++y)
            for (int x = cx - 6; x <= cx + 6; ++x) {
              const auto color = candidate.pixel(x, y);
              sums[0] += qRed(color) * qAlpha(color);
              sums[1] += qGreen(color) * qAlpha(color);
              sums[2] += qBlue(color) * qAlpha(color);
            }
          const int winner = int(std::max_element(sums, sums + 3) - sums);
          if (!sums[winner])
            labels.append(QJsonValue(QJsonValue::Null));
          else {
            for (int channel = 0; channel < 3; ++channel)
              require(channel == winner || sums[channel] == 0,
                      "Candidate cadence cell mixes frame colors");
            labels.append(winner);
          }
        }
        metrics["candidateFrameSelections"] = labels;
        metrics["cadenceQualification"] =
            "Exploratory frame-advancement acquisition; no fitted source RNG";
      }
      metrics["candidateSeed"] = QString::number(qulonglong(p.seed));
      metrics["sourceSemanticallySupported"] = false;
      metrics["sourceReferenceVerified"] = false;
      if (sourceHard || sourceSoft) {
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
            metrics[alphaCoverage ? "alphaCoverageIoU"
                                  : "visibleChangedPixelIoU"]
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
      if (sourceHsv) {
        // Full-canvas union-region metrics, including alpha and missing paint;
        // no center-pixel-only qualification or dilution by empty canvas.
        const double colorTolerance =
            p.sourceHsvChannel == SourceHsvChannel::Saturation ? 1 / 255.0 : 0;
        // Both hard masks are binary-alpha and RGB8. Compare integer code
        // differences, avoiding floating subtraction placing exactly one byte
        // microscopically above 1/255. Alpha and geometry gates are unchanged.
        const int byteError =
            int(std::floor(metrics["maxRGBError"].toDouble() * 255 + .5));
        const bool passed =
            metrics["hardPixelCandidatePassed"].toBool() &&
            byteError <=
                (p.sourceHsvChannel == SourceHsvChannel::Saturation ? 1 : 0) &&
            metrics["maxAlphaError"].toDouble() == 0;
        metrics["fixedHsvCandidatePassed"] = passed;
        metrics["fixedHsvMaxRGBByteError"] = byteError;
        metrics["fixedHsvMaxRGBTolerance"] = colorTolerance;
        if (!passed && !failedHardCases.contains(name))
          failedHardCases.append(name);
      }
      if (wetHsvCase) {
        const auto spec = source["wetHsvProbe"].toObject();
        const QString channel = spec["channel"].toString();
        const auto expectedChannel =
            !spec["enabled"].toBool() ? SourceHsvChannel::Off
            : channel == "h"          ? SourceHsvChannel::Hue
            : channel == "s"          ? SourceHsvChannel::Saturation
                                      : SourceHsvChannel::Value;
        const bool eligible =
            p.profile ==
                ("krita-5.3.4.1-" + spec["model"].toString() + "-candidate-1")
                    .toStdString() &&
            p.sourceHsvChannel == expectedChannel &&
            (expectedChannel == SourceHsvChannel::Off ||
             p.sourceHsvValue == spec["value"].toDouble());
        const bool passed =
            eligible && metrics["maxRGBError"].toDouble() <= 2 / 255.0 &&
            metrics["maxAlphaError"].toDouble() <= 1 / 255.0 &&
            metrics["visibleChangedPixelIoU"].toDouble() >= .98 &&
            !metrics["emptyCandidateFailure"].toBool();
        metrics["wetHsvSourceEligible"] = eligible;
        metrics["wetHsvCandidatePassed"] = passed;
        metrics["wetHsvQualification"] =
            "Full union-region RGBA and source eligibility; native conversion "
            "cannot pass";
        if (!passed && !failedHardCases.contains(name))
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
      if (maskCase) {
        metrics["maskedAcquisitionInputsValidated"] = true;
        metrics["sourceParityQualified"] = false;
      }
      if (sensorCase) {
        metrics["sensorAcquisitionInputsValidated"] = true;
        metrics["candidateInputConvention"] =
            "delivered Qt tilt degrees / 60; barrel degrees to radians; "
            "delivered relative milliseconds to seconds (native application "
            "convention)";
        metrics["sensorSourceParityQualified"] = false;
      }
      if (fixtures["group"].toString() == "wetdrift") {
        const auto spec = source["driftProbe"].toObject();
        const int count = spec["nominalDabs"].toInt();
        bool passed = stroke.dabCount() == size_t(count);
        if (spec["calibration"].toBool())
          passed = passed && metrics["maxAlphaError"].toDouble() == 0 &&
                   metrics["maxRGBError"].toDouble() == 0;
        else
          passed = passed && metrics["premultipliedRGBMAE"].toDouble() <= 3 / 255.0 &&
                   metrics["alphaMAE"].toDouble() <= 2 / 255.0 &&
                   metrics["deltaE00BlackMean"].toDouble() <= 3 &&
                   metrics["deltaE00WhiteMean"].toDouble() <= 3 &&
                   metrics["deltaE00BlackP95"].toDouble() <= 6 &&
                   metrics["deltaE00WhiteP95"].toDouble() <= 6;
        metrics["driftCheckpoint"] = count;
        metrics["driftModel"] = spec["model"];
        metrics["driftCheckpointPassed"] = passed;
        metrics["sourceCountQualification"] =
            "Observable deposits and dry path controls; internal source dab counter unavailable";
        if (!passed && !failedHardCases.contains(name)) failedHardCases.append(name);
      }
      metrics["renderMilliseconds"] = elapsed;
      metrics["dabs"] = double(stroke.dabCount());
      metrics["diagnostics"] =
          QJsonArray::fromStringList(imported.records.front().diagnostics);
      results.append(metrics);
    }
    require(remaining.empty() && results.size() == expected.size(),
            "Incomplete comparison workload");
    require((fixtures["group"].toString() != "gihstatistics" &&
             fixtures["group"].toString() != "gihlabels") ||
                stochasticReplicates.size() == 100,
            "Incomplete independent stochastic candidate workload");
    if (fixtures["group"].toString() == "wetdrift") {
      for (const QString &model : {QString("legacy-smear"), QString("legacy-dulling"),
                                   QString("new-smear"), QString("new-dulling")}) {
        double thousand = -1, tenThousand = -1;
        for (const auto &value : results) {
          const auto row = value.toObject();
          if (row["driftModel"].toString() != model) continue;
          if (row["driftCheckpoint"].toInt() == 1000) thousand = row["premultipliedRGBMAE"].toDouble();
          if (row["driftCheckpoint"].toInt() == 10000) tenThousand = row["premultipliedRGBMAE"].toDouble();
        }
        if (thousand < 0 || tenThousand < 0 || tenThousand > thousand + 1 / 255.0)
          failedHardCases.append("Source drift growth: " + model);
      }
    }
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
