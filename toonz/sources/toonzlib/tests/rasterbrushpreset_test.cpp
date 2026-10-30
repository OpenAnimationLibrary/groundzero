#include "toonz/rasterbrushpreset.h"
#include "rasterbrushfixture.h"
#include "toonz/rasterbrushimport.h"
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QImage>
#include <QBuffer>
#include <QCryptographicHash>
#ifdef __linux__
#include <sys/resource.h>
#endif
#include <iostream>
#include <cmath>
#include <stdexcept>
#if defined(__linux__) && defined(RASTERBRUSH_SANDBOX_TESTING)
namespace RasterBrush {
int probeImportSandbox(const QString &, const QString &, const QString &);
}
#endif
using namespace RasterBrush;
using RasterBrushTest::hardPixelKpp;
#if defined(_WIN32) && defined(RASTERBRUSH_SANDBOX_TESTING)
namespace RasterBrush {
int probeWindowsImportLimits();
}
#endif
namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
QByteArray altered(const QByteArray &bytes, const QString &key,
                   const QJsonValue &value) {
  auto root = QJsonDocument::fromJson(bytes).object();
  root[key] = value;
  return QJsonDocument(root).toJson();
}
QByteArray gbr(unsigned version = 2, unsigned channels = 1) {
  QByteArray b;
  auto word = [&](unsigned n) {
    for (int i = 3; i >= 0; --i) b.append(char(n >> (8 * i)));
  };
  word(version == 1 ? 22 : 30);
  word(version);
  word(2);
  word(1);
  word(channels);
  if (version == 2) {
    word(0x47494d50);
    word(25);
  }
  b.append("x\0", 2);
  if (channels == 1) {
    b.append(char(255));
    b.append(char(0));
  } else
    b.append("\xff\0\0\xff\0\xff\0\0", 8);
  return b;
}
QByteArray sampledAbr() {
  QByteArray abr, sample;
  auto be = [](QByteArray &out, unsigned n, int length) {
    for (int i = length - 1; i >= 0; --i) out.append(char(n >> (8 * i)));
  };
  be(sample, 0, 4);
  be(sample, 25, 2);
  be(sample, 1, 1);
  for (int n : {0, 0, 1, 2}) be(sample, n, 2);
  for (int n : {0, 0, 1, 2}) be(sample, n, 4);
  be(sample, 8, 2);
  be(sample, 0, 1);
  sample.append(char(255));
  sample.append(char(0));
  be(abr, 1, 2);
  be(abr, 1, 2);
  be(abr, 2, 2);
  be(abr, sample.size(), 4);
  abr += sample;
  return abr;
}
QByteArray computedAbr(int version, int spacing = 25) {
  QByteArray bytes;
  auto be = [&](unsigned value, int count) {
    for (int i = count - 1; i >= 0; --i) bytes.append(char(value >> (8 * i)));
  };
  be(version, 2);
  be(1, 2);
  be(1, 2);
  be(14, 4);
  be(0, 4);
  for (int value : {spacing, 64, 50, -30, 75}) be(unsigned(value), 2);
  return bytes;
}
QByteArray kpp(const QString &engine = "paintbrush") {
  QImage image(2, 2, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  image.setText("version", "2.2");
  image.setText(
      "preset",
      "<Preset name=\"Original fixture\" paintopid=\"" + engine +
          "\">"
          "<param name=\"brush_definition\"><![CDATA[<Brush "
          "type=\"auto_brush\" spacing=\"0.1\" angle=\"0\" density=\"1\">"
          "<MaskGenerator id=\"default\" type=\"circle\" diameter=\"20\" "
          "ratio=\"1\" hfade=\"1\" vfade=\"1\" spikes=\"2\" "
          "antialiasEdges=\"1\"/>"
          "</Brush>]]></param><param name=\"SmudgeRateMode\">1</param>"
          "<param name=\"SmudgeRateUseNewEngine\">true</param></Preset>");
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  require(image.save(&buffer, "PNG"), "cannot create original KPP fixture");
  return bytes;
}
QByteArray externalKpp(const QString &attributes = {}, bool png = false) {
  QImage image(2, 2, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  image.setText("version", "2.2");
  image.setText(
      "preset",
      QString(
          "<Preset name=\"External fixture\" paintopid=\"paintbrush\"><param "
          "name=\"brush_definition\"><![CDATA[<Brush type=\"%1\" "
          "filename=\"%2\" scale=\"1\" spacing=\"0.25\" "
          "angle=\"0\" %3/>]]></param></Preset>")
          .arg(png ? "png_brush" : "gbr_brush",
               png ? "original.png" : "original.gbr", attributes));
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  require(image.save(&buffer, "PNG"), "cannot encode external KPP fixture");
  return bytes;
}
unsigned crc(const QByteArray &data) {
  unsigned c = 0xffffffffu;
  for (unsigned char b : data) {
    c ^= b;
    for (int i = 0; i < 8; ++i)
      c = (c >> 1) ^ (0xedb88320u & unsigned(-int(c & 1)));
  }
  return ~c;
}
QByteArray archive(
    const std::vector<std::pair<QByteArray, QByteArray>> &entries,
    bool compress) {
  QByteArray local, central;
  auto le = [](QByteArray &out, unsigned n, int width) {
    for (int i = 0; i < width; ++i) out.append(char(n >> (8 * i)));
  };
  for (const auto &entry : entries) {
    auto name = entry.first, original = entry.second, body = original;
    if (compress) {
      body = qCompress(original);
      body = body.mid(6, body.size() - 10);
    }
    unsigned offset = local.size(), checksum = crc(original),
             method = compress ? 8 : 0;
    le(local, 0x04034b50, 4);
    le(local, 20, 2);
    le(local, 0, 2);
    le(local, method, 2);
    le(local, 0, 4);
    le(local, checksum, 4);
    le(local, body.size(), 4);
    le(local, original.size(), 4);
    le(local, name.size(), 2);
    le(local, 0, 2);
    local += name;
    local += body;
    le(central, 0x02014b50, 4);
    le(central, 20, 2);
    le(central, 20, 2);
    le(central, 0, 2);
    le(central, method, 2);
    le(central, 0, 4);
    le(central, checksum, 4);
    le(central, body.size(), 4);
    le(central, original.size(), 4);
    le(central, name.size(), 2);
    le(central, 0, 2);
    le(central, 0, 2);
    le(central, 0, 2);
    le(central, 0, 2);
    le(central, 0, 4);
    le(central, offset, 4);
    central += name;
  }
  unsigned start = local.size();
  local += central;
  le(local, 0x06054b50, 4);
  le(local, 0, 2);
  le(local, 0, 2);
  le(local, entries.size(), 2);
  le(local, entries.size(), 2);
  le(local, central.size(), 4);
  le(local, start, 4);
  le(local, 0, 2);
  return local;
}
void testBundles() {
  QByteArray manifest =
      "<manifest:manifest "
      "xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" "
      "manifest:version=\"1.2\"><manifest:file-entry "
      "manifest:full-path=\"paintoppresets/original.kpp\" "
      "manifest:media-type=\"paintoppresets\"/></manifest:manifest>";
  std::vector<std::pair<QByteArray, QByteArray>> entries = {
      {"mimetype", "application/x-krita-resourcebundle"},
      {"META-INF/manifest.xml", manifest},
      {"paintoppresets/original.kpp", kpp()}};
  QString error;
  for (bool compression : {false, true}) {
    auto result = importBrush(archive(entries, compression), "bundle", error);
    require(result.complete && result.records.size() == 1, qPrintable(error));
    require(result.records[0].provenance["bundleMember"] ==
                "paintoppresets/original.kpp",
            "bundle identity lost");
  }
  auto external = entries;
  external[1].second.replace(
      "</manifest:manifest>",
      "<manifest:file-entry manifest:full-path=\"brushes/original.gbr\" "
      "manifest:media-type=\"brushes\"/></manifest:manifest>");
  external[2].second = externalKpp();
  external.push_back({"brushes/original.gbr", gbr()});
  auto resolved = importBrush(archive(external, true), "bundle", error);
  require(
      resolved.complete && resolved.records[0].preset.tip.images.size() == 1,
      qPrintable(error));
  require(resolved.records[0].preset.tip.images[0]->pixels[0].a == 1,
          "bundle dependency decoded incorrectly");
  require(!importBrush(externalKpp(), "kpp", error).complete,
          "missing external KPP resource accepted");
  auto colored          = external;
  colored.back().second = gbr(2, 4);
  colored[2].second     = externalKpp("ColorAsMask=\"1\"");
  auto gray             = importBrush(archive(colored, true), "bundle", error);
  require(gray.complete && !gray.records[0].preset.tip.images[0]->color,
          "colored GBR alpha-mask selection was ignored");
  require(std::abs(gray.records[0].preset.tip.images[0]->pixels[0].a - .7874) <
              1e-12,
          "native grayscale conversion did not retain weighted red coverage");

  QImage stamp(2, 1, QImage::Format_ARGB32);
  stamp.setPixel(0, 0, qRgba(255, 255, 255, 255));
  stamp.setPixel(1, 0, qRgba(255, 0, 0, 128));
  QByteArray png;
  QBuffer pngBuffer(&png);
  pngBuffer.open(QIODevice::WriteOnly);
  require(stamp.save(&pngBuffer, "PNG"), "cannot create original color stamp");
  auto pngBundle = colored;
  pngBundle[1].second.replace("original.gbr", "original.png");
  pngBundle.back() = {"brushes/original.png", png};
  for (int application : {0, 1}) {
    // The modern application value wins over the opposing legacy flag.
    pngBundle[2].second = externalKpp(
        QString("brushApplication=\"%1\" ColorAsMask=\"%1\"").arg(application),
        true);
    auto record = importBrush(archive(pngBundle, true), "bundle", error,
                              nullptr, 2 * (sizeof(Pixel) + 16));
    require(record.complete && record.records.size() == 1, qPrintable(error));
    auto tip = record.records[0].preset.tip.images[0];
    require(tip->color == (application == 1),
            "PNG tip application was discarded");
    const double alpha = 128 / 255.0;
    require(std::abs(tip->pixels[1].a - (application ? alpha : alpha * .7874)) <
                    1e-12 &&
                std::abs(tip->pixels[0].a - (application ? 1 : 0)) < 1e-12,
            "color-image or premultiplied mask alpha was changed");
    if (application)
      require(tip->pixels[1].r == alpha && tip->pixels[1].g == 0,
              "embedded stamp color was lost");
    auto reopened = decode(encode(record.records[0], error), error);
    require(error.isEmpty() &&
                reopened.preset.tip.images[0]->color == tip->color &&
                !reopened.referenceVerified,
            "portable stamp changed application or gained verification");
  }
  for (const QString &application :
       {QString("2"), QString("3"), QString("99")}) {
    pngBundle[2].second =
        externalKpp(QString("brushApplication=\"%1\"").arg(application), true);
    require(!importBrush(archive(pngBundle, true), "bundle", error).complete,
            "unsupported lightness/gradient tip silently converted");
  }
  auto unsafe = entries;
  unsafe.push_back({"../escape", "x"});
  require(!importBrush(archive(unsafe, false), "bundle", error).complete,
          "traversal accepted");
  unsafe = entries;
  unsafe.push_back({"MIMETYPE", "x"});
  require(!importBrush(archive(unsafe, false), "bundle", error).complete,
          "case collision accepted");
  auto damaged = archive(entries, false);
  damaged[40]  = char(damaged[40] ^ 1);
  require(!importBrush(damaged, "bundle", error).complete,
          "archive checksum ignored");
  entries.pop_back();
  require(!importBrush(archive(entries, false), "bundle", error).complete,
          "missing manifest resource accepted");
}
void testImports() {
  QString error;
  const size_t abrBytes = 2 * (sizeof(Pixel) + 1);
  require(importBrush(sampledAbr(), "abr", error, nullptr, abrBytes).complete,
          "exact ABR scratch/resource budget rejected");
  require(
      !importBrush(sampledAbr(), "abr", error, nullptr, abrBytes - 1).complete,
      "retained ABR byte masks not charged to allocation budget");
  for (int version : {1, 2}) {
    auto geometry = importBrush(computedAbr(version), "abr", error);
    require(geometry.complete && geometry.records.size() == 1,
            "computed ABR could not become a native preset");
    const auto &record = geometry.records.front();
    require(record.preset.tip.shape == Shape::Round &&
                record.preset.tip.images.empty() &&
                record.preset.size.value == 64 &&
                record.preset.aspect.value == .5 &&
                record.preset.angle.value == -30 &&
                record.preset.spacing == .25 &&
                record.preset.tip.hardness == .75 &&
                !record.provenance["sourceSemanticallySupported"].toBool(),
            "computed ABR geometry or qualification was misrepresented");
    auto restored = decode(encode(record, error), error);
    require(error.isEmpty() && restored.semanticallySupported &&
                restored.preset.tip.images.empty() &&
                restored.preset.aspect.value == .5,
            "computed native preset did not survive portable save/reopen");
    require(!importBrush(computedAbr(version, 0), "abr", error).complete,
            "unimplemented zero spacing was silently approximated");
  }
  auto numericSpelling =
      importBrush(hardPixelKpp("numericSpelling"), "kpp", error);
  require(numericSpelling.complete &&
              numericSpelling.records.front().preset.profile ==
                  "krita-5.3.4.1-hard-pixel-candidate-1",
          "equivalent numeric XML spelling changed source eligibility");
  auto hard = importBrush(hardPixelKpp(), "kpp", error);
  require(hard.complete && hard.records.size() == 1, qPrintable(error));
  const auto &hardRecord = hard.records.front();
  require(hardRecord.preset.profile == "krita-5.3.4.1-hard-pixel-candidate-1" &&
              !hardRecord.provenance["sourceSemanticallySupported"].toBool() &&
              !hardRecord.provenance["sourceReferenceVerified"].toBool(),
          "strict hard candidate selection or qualification failed");
  require(hardRecord.preset.wash, "KPP wash mode was replaced by build-up");
  auto freehand = importBrush(hardPixelKpp("freehandWash"), "kpp", error);
  require(
      freehand.complete && freehand.records.front().preset.wash &&
          freehand.records.front().preset.profile == hardRecord.preset.profile,
      "source freehand wash mode lost candidate semantics");
  auto buildup = importBrush(hardPixelKpp("buildUp"), "kpp", error);
  require(
      buildup.complete && !buildup.records.front().preset.wash &&
          buildup.records.front().preset.profile == hardRecord.preset.profile,
      "KPP build-up mode was replaced by wash");
  auto translucent = importBrush(hardPixelKpp("halfOpacity"), "kpp", error);
  require(translucent.complete &&
              translucent.records.front().preset.opacity.value == .5 &&
              translucent.records.front().preset.profile ==
                  hardRecord.preset.profile,
          "fixed source opacity failed eligibility");
  auto pressure = importBrush(hardPixelKpp("pressure"), "kpp", error);
  require(pressure.complete &&
              pressure.records.front().preset.profile ==
                  hardRecord.preset.profile &&
              pressure.records.front().preset.size.mappings.size() == 1,
          "explicit linear source pressure curve was not preserved");
  auto hardRestored = decode(encode(hardRecord, error), error);
  require(error.isEmpty() &&
              hardRestored.preset.profile == hardRecord.preset.profile,
          "hard candidate lost during portable round trip");
  for (const QString &mutation :
       {"missing", "unknown", "attribute", "wrongModeKey"}) {
    auto fallback = importBrush(hardPixelKpp(mutation), "kpp", error);
    require(fallback.complete && fallback.records.front().preset.profile ==
                                     "groundzero-native-1",
            "unclassified setting incorrectly selected source candidate");
  }
  auto rect = importBrush(hardPixelKpp("rect"), "kpp", error);
  require(rect.complete &&
              rect.records.front().preset.profile ==
                  "krita-5.3.4.1-default-rect-candidate-1" &&
              rect.records.front().preset.tip.shape == Shape::Square,
          "source default rectangle lost explicit geometry");
  auto aa = importBrush(hardPixelKpp("aa"), "kpp", error);
  require(aa.complete &&
              aa.records.front().preset.profile ==
                  "krita-5.3.4.1-default-circle-candidate-1" &&
              aa.records.front().preset.tip.antialiasEdges,
          "source default-circle AA lost explicit eligibility");
  auto soft = importBrush(hardPixelKpp("soft"), "kpp", error);
  require(
      soft.complete &&
          soft.records.front().preset.profile ==
              "krita-5.3.4.1-default-circle-candidate-1" &&
          soft.records.front().preset.tip.hardness == .5 &&
          !soft.records.front().provenance["sourceReferenceVerified"].toBool(),
      "explicit default-circle fade lost its candidate identity");
  auto unequal = importBrush(hardPixelKpp("unequalFade"), "kpp", error);
  require(unequal.complete &&
              unequal.records.front().preset.profile == "groundzero-native-1",
          "unequal fade was silently collapsed");
  auto ellipse = importBrush(hardPixelKpp("ellipse"), "kpp", error);
  require(
      ellipse.complete &&
          ellipse.records.front().preset.profile ==
              "krita-5.3.4.1-hard-pixel-candidate-1" &&
          ellipse.records.front().preset.aspect.value == .5 &&
          std::abs(ellipse.records.front().preset.angle.value + 45) < 1e-10 &&
          !ellipse.records.front()
               .provenance["sourceReferenceVerified"]
               .toBool(),
      "explicit source ellipse geometry or qualification was lost");
  for (const QString &model :
       {"legacy-smear", "legacy-dulling", "new-smear", "new-dulling"}) {
    auto result = importBrush(hardPixelKpp({}, model), "kpp", error);
    require(result.complete && result.records.size() == 1, qPrintable(error));
    const auto expected =
        "krita-5.3.4.1-" + model.toStdString() + "-candidate-1";
    require(result.records.front().preset.profile == expected &&
                result.records.front().preset.smudgeRadius == 0,
            "explicit wet model did not select matching candidate");
    auto restored = decode(encode(result.records.front(), error), error);
    require(error.isEmpty() && restored.preset.profile == expected &&
                !restored.provenance["sourceReferenceVerified"].toBool(),
            "wet candidate portable qualification mismatch");
    for (const QString &flow : {"zeroFlow", "halfFlow"}) {
      auto inactive = importBrush(hardPixelKpp(flow, model), "kpp", error);
      require(inactive.complete &&
                  inactive.records.front().preset.profile == expected &&
                  inactive.records.front().preset.flow.value ==
                      (flow == "zeroFlow" ? 0 : .5),
              "static source wet flow lost its value or candidate identity");
    }
    auto pressure =
        importBrush(hardPixelKpp("wetPressure", model), "kpp", error);
    require(pressure.complete &&
                pressure.records.front().preset.profile == expected,
            "linear wet pressure controls lost strict eligibility");
    auto pressureRoundTrip =
        decode(encode(pressure.records.front(), error), error);
    require(error.isEmpty() &&
                pressureRoundTrip.preset.pickupControl.mappings.size() == 1 &&
                pressureRoundTrip.preset.colorRateControl.mappings.size() == 1,
            "wet pressure controls lost portable round trip");
    for (const QString &mutation : {"alphaOff", "lengthOff"}) {
      auto flags = importBrush(hardPixelKpp(mutation, model), "kpp", error);
      require(flags.complete &&
                  flags.records.front().preset.profile ==
                      "krita-5.3.4.1-" + model.toStdString() + "-candidate-1",
              "source wet switch lost strict eligibility");
      auto restored = decode(encode(flags.records.front(), error), error);
      require(
          error.isEmpty() &&
              restored.preset.sourceSmearAlpha == (mutation != "alphaOff") &&
              restored.preset.sourceSmudgeLengthEnabled ==
                  (mutation != "lengthOff"),
          "source wet switches lost portable round trip");
    }
    for (const QString &mutation :
         {"radius", "unknown", "missing", "aa", "ellipse"}) {
      auto fallback = importBrush(hardPixelKpp(mutation, model), "kpp", error);
      const bool inactiveRadius =
          mutation == "radius" && model.endsWith("smear");
      require(fallback.complete &&
                  fallback.records.front().preset.profile ==
                      (mutation == "radius" ? expected : "groundzero-native-1"),
              "wet setting eligibility differs from active/inactive contract");
      if (mutation == "radius" && model.endsWith("dulling")) {
        auto portable = decode(encode(fallback.records.front(), error), error);
        require(
            error.isEmpty() && portable.preset.profile == expected &&
                portable.preset.smudgeRadius == .01 &&
                !portable.provenance["sourceReferenceVerified"].toBool(),
            "source Dulling radius lost portable identity or qualification");
      }
      if (inactiveRadius)
        require(fallback.records.front().preset.smudgeRadius == 0 &&
                    fallback.records.front()
                            .provenance["inactiveSourceSmudgeRadiusPercent"]
                            .toDouble() == 1,
                "inactive source Smear radius was applied or discarded");
    }
  }
  auto dry = importBrush(kpp(), "kpp", error);
  require(dry.complete, qPrintable(error));
  require(dry.records[0].preset.tip.antialiasEdges,
          "generated KPP antialias choice was discarded");
  require(!dry.records[0].provenance["sourceSemanticallySupported"].toBool(),
          "KPP approximation claimed exact");
  auto wet = importBrush(kpp("colorsmudge"), "kpp", error);
  require(wet.complete, qPrintable(error));
  require(wet.records[0].preset.wet == Wet::Dulling,
          "native wet conversion missing");
  auto corrupt                = kpp();
  corrupt[corrupt.size() - 1] = char(corrupt.back() ^ 1);
  require(!importBrush(corrupt, "kpp", error).complete, "PNG CRC ignored");
  require(!importBrush(kpp("hairybrush"), "kpp", error).complete,
          "unsupported KPP engine accepted");
  for (unsigned version : {1u, 2u})
    for (unsigned channels : {1u, 4u}) {
      auto result = importBrush(gbr(version, channels), "gbr", error);
      require(result.complete && result.records.size() == 1, qPrintable(error));
      const auto &r = result.records[0];
      require(!r.referenceVerified, "foreign verification invented");
      require(r.preset.tip.images[0]->pixels[0].a == 1, "GBR mask polarity");
      require(r.preset.tip.images[0]->pixels[1].a == 0,
              "GBR transparent pixel");
      require(r.preset.size.value == 2 && r.preset.aspect.value == 1,
              "GBR aspect");
    }
  QByteArray pipe =
      "Sequence\n2 ncells:2 step:25 dim:1 cols:2 rows:1 rank0:2 "
      "sel0:incremental\n";
  pipe += gbr();
  pipe += gbr();
  auto result = importBrush(pipe, "gih", error);
  require(result.complete && result.records[0].preset.tip.images.size() == 2,
          qPrintable(error));
  pipe.replace("sel0:incremental", "sel0:random");
  result = importBrush(pipe, "gih", error);
  require(result.complete && result.records[0].preset.tip.randomSequence,
          qPrintable(error));
  pipe.replace("sel0:random", "sel0:angular");
  require(!importBrush(pipe, "gih", error).complete,
          "unsupported selector accepted");
  auto data = gbr();
  data.chop(1);
  require(!importBrush(data, "gbr", error).complete, "truncated GBR accepted");
  data    = gbr();
  data[7] = 3;
  require(!importBrush(data, "gbr", error).complete,
          "unsupported GBR version accepted");
  data    = gbr();
  data[8] = 127;
  require(!importBrush(data, "gbr", error).complete, "oversized GBR accepted");
}
void testPackage() {
  Record r;
  r.preset.name = "Original brush";
  r.diagnostics << "Original conversion note: source behavior is unverified.";
  r.provenance  = {{"license", "CC0-1.0"}, {"origin", "original test fixture"}};
  auto image    = std::make_shared<Image>();
  image->width  = 2;
  image->height = 2;
  image->color  = true;
  image->pixels = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {0, 0, 0, 0}};
  r.preset.tip.shape          = Shape::Bitmap;
  r.preset.tip.antialiasEdges = true;
  r.preset.tip.images.push_back(image);
  r.preset.texture.image = image;
  r.preset.texture.depth = .5;
  r.preset.size.mappings.push_back({Sensor::Rotation, {{0, .2}, {1, 1}}, 1});
  r.preset.pickupControl.mappings.push_back(
      {Sensor::Pressure, {{0, 0}, {1, 1}}, 1});
  r.preset.background          = {.2, .1, 0, .5};
  r.preset.backgroundMix.value = .25;
  r.preset.hue.value           = 180;
  r.preset.hue.mappings.push_back(
      {Sensor::RandomStroke, {{0, .2}, {1, .8}}, 1});
  r.preset.hue.mappings.front().interpolation = Interpolation::NaturalCubic;
  r.preset.countControl.mappings.push_back(
      {Sensor::DabIndex, {{0, 1}, {1, 0}}, 20, Interpolation::NaturalCubic});
  r.preset.angle.mappings.push_back(
      {Sensor::InitialDirection, {{0, 0}, {1, 1}}, 1});
  r.preset.scatterBothAxes = false;
  r.preset.secondary.count = 3;
  r.preset.secondary.flipX = r.preset.secondary.flipY = true;
  r.preset.saturation.value                           = .7;
  r.preset.brightness.value                           = 1.2;
  r.preset.seed = UINT64_C(18446744073709551615);
  QString error;
  auto bytes = encode(r, error);
  require(!bytes.isEmpty(), qPrintable(error));
  std::atomic_bool cancelled(true);
  auto stopped = importBrush(bytes, "gzbrush", error, &cancelled);
  require(stopped.records.empty() && !stopped.complete &&
              error.contains("cancelled"),
          "cancelled import produced installable records");
  auto restored = decode(bytes, error);
  require(error.isEmpty() && restored.semanticallySupported, qPrintable(error));
  require(restored.preset.tip.antialiasEdges,
          "antialias setting lost on reopen");
  require(!restored.preset.scatterBothAxes &&
              restored.preset.secondary.count == 3 &&
              restored.preset.secondary.flipX &&
              restored.preset.secondary.flipY &&
              restored.preset.countControl.mappings.front().sensor ==
                  Sensor::DabIndex &&
              restored.preset.countControl.mappings.front().scale == 20 &&
              restored.preset.angle.mappings.front().sensor ==
                  Sensor::InitialDirection,
          "native count, initial-direction or scatter settings lost on reopen");
  decode(bytes, error, 4 * sizeof(Pixel) - 1);
  require(error.contains("before allocation"),
          "remaining package budget checked after allocation");
  require(restored.diagnostics == r.diagnostics,
          "conversion notes were lost on package reopening");
  decode(altered(bytes, "diagnostics", QJsonArray{42}), error);
  require(!error.isEmpty(), "non-text conversion note accepted");
  decode(altered(bytes, "diagnostics", QJsonArray{QString(16385, 'x')}), error);
  require(!error.isEmpty(), "oversized conversion note accepted");
  auto older = QJsonDocument::fromJson(bytes).object();
  older.remove("diagnostics");
  auto olderSettings = older["settings"].toObject();
  olderSettings.remove("countControl");
  olderSettings.remove("scatterBothAxes");
  auto olderSecondary = olderSettings["secondary"].toObject();
  for (const auto &key : {"count", "flipX", "flipY"})
    olderSecondary.remove(key);
  olderSettings["secondary"] = olderSecondary;
  auto olderTip              = olderSettings["tip"].toObject();
  olderTip.remove("antialiasEdges");
  olderSettings["tip"] = olderTip;
  older["settings"]    = olderSettings;
  auto oldRecord       = decode(QJsonDocument(older).toJson(), error);
  require(error.isEmpty() && oldRecord.semanticallySupported &&
              oldRecord.diagnostics.empty() &&
              oldRecord.preset.scatterBothAxes &&
              oldRecord.preset.countControl.value == 1 &&
              oldRecord.preset.countControl.mappings.empty() &&
              oldRecord.preset.secondary.count == 1 &&
              !oldRecord.preset.secondary.flipX &&
              !oldRecord.preset.secondary.flipY &&
              !oldRecord.preset.tip.antialiasEdges,
          "older native package without notes stopped loading");
  require(restored.preset.pickupControl.mappings.size() == 1,
          "wet dynamics lost in package");
  require(restored.preset.hue.mappings.size() == 1 &&
              restored.preset.hue.mappings.front().interpolation ==
                  Interpolation::NaturalCubic &&
              restored.preset.background.a == .5 &&
              restored.preset.brightness.value == 1.2,
          "color dynamics lost in package");
  require(!restored.referenceVerified, "unearned reference verification");
  require(restored.preset.seed == r.preset.seed, "seed precision lost");
  require(restored.preset.tip.images[0]->pixels[0].r == 1, "image lost");
  require(restored.preset.tip.images[0] == restored.preset.texture.image,
          "resource not deduplicated");
  require(encode(restored, error) == bytes, "package roundtrip changed");
  decode(altered(bytes, "version", 2), error);
  require(!error.isEmpty(), "future version accepted");
  auto root                        = QJsonDocument::fromJson(bytes).object();
  auto settings                    = root["settings"].toObject();
  settings["unrecognizedDynamics"] = true;
  root["settings"]                 = settings;
  decode(QJsonDocument(root).toJson(), error);
  require(!error.isEmpty(), "unknown behavior ignored");
  root           = QJsonDocument::fromJson(bytes).object();
  root["assets"] = QJsonObject();
  decode(QJsonDocument(root).toJson(), error);
  require(!error.isEmpty(), "missing resource accepted");
  root                         = QJsonDocument::fromJson(bytes).object();
  auto assets                  = root["assets"].toObject();
  auto a                       = assets.begin().value().toObject();
  a["color"]                   = false;
  assets[assets.begin().key()] = a;
  root["assets"]               = assets;
  decode(QJsonDocument(root).toJson(), error);
  require(!error.isEmpty(), "hash mismatch accepted");
  QTemporaryDir dir;
  require(dir.isValid(), "temporary directory unavailable");
  auto path = install(dir.path(), r, error);
  require(!path.isEmpty(), qPrintable(error));
  require(install(dir.path(), r, error) == path,
          "duplicate install changed identity");
  auto saved = load(path, error);
  require(saved.semanticallySupported, qPrintable(error));
  QFile f(path);
  require(f.open(QIODevice::WriteOnly), "cannot damage fixture");
  f.write("damaged");
  f.close();
  require(install(dir.path(), r, error).isEmpty(),
          "conflicting install overwritten");
  r.preset.tip.shape = Shape(99);
  require(encode(r, error).isEmpty(), "invalid enum serialized");
  r.preset.tip.shape = Shape::Bitmap;
  r.preset.color     = {1, 0, 0, .5};
  require(encode(r, error).isEmpty(), "invalid premultiplication accepted");
}
void testCollection() {
  QTemporaryDir directory;
  require(directory.isValid(), "temporary collection directory unavailable");
  Record first, second;
  first.preset.name        = "Original first";
  second.preset.name       = "Original second";
  second.preset.size.value = 36;
  QString error;
  auto invalid              = second;
  invalid.preset.size.value = -1;
  require(installCollection(directory.path(), {first, invalid}, error).empty(),
          "partially valid collection installed");
  require(QDir(directory.path())
              .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)
              .empty(),
          "failed collection left staged or installed files");
  std::atomic_bool cancelled(true);
  require(
      installCollection(directory.path(), {first, second}, error, &cancelled)
          .empty(),
      "cancelled collection installed");
  require(QDir(directory.path())
              .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)
              .empty(),
          "cancelled collection left files");
  auto paths = installCollection(directory.path(), {first, second}, error);
  require(error.isEmpty() && paths.size() == 2, qPrintable(error));
  require(load(paths[0], error).preset.name == first.preset.name &&
              load(paths[1], error).preset.size.value == 36,
          "collection preset content changed");
  require(installCollection(directory.path(), {first, second}, error) == paths,
          "collection reimport changed identity");
  QFile damaged(paths[1]);
  require(damaged.open(QIODevice::WriteOnly),
          "cannot damage collection fixture");
  damaged.write("damaged");
  damaged.close();
  require(installCollection(directory.path(), {first, second}, error).empty(),
          "conflicting collection overwritten");
  require(load(paths[0], error).preset.name == first.preset.name,
          "conflicting collection modified another preset");
}
void testWorkerWire() {
  QTemporaryDir directory;
  require(directory.isValid(), "cannot create worker wire fixture");
  Record record;
  auto image              = std::make_shared<Image>();
  image->width            = 2;
  image->height           = 1;
  image->color            = true;
  image->pixels           = {{128.0 / 255, 0, 0, 128.0 / 255}, {0, 1, 0, 1}};
  record.preset.tip.shape = Shape::Bitmap;
  record.preset.tip.images.push_back(image);
  // Shared resources are written and charged only once per record.
  record.preset.texture.image = image;
  QString error;
  auto bytes = encodeWorkerRecord(record, directory.path(), error);
  require(!bytes.isEmpty() && error.isEmpty(), qPrintable(error));
  const size_t budget = 2 * (sizeof(Pixel) + 8);
  auto restored = decodeWorkerRecord(bytes, directory.path(), error, budget);
  require(error.isEmpty(), qPrintable(error));
  auto restoredImage = restored.preset.tip.images.front();
  require(restoredImage == restored.preset.texture.image &&
              std::abs(restoredImage->pixels[0].a - 128.0 / 255) < 1e-7 &&
              restoredImage->pixels[1].g == 1,
          "worker wire changed color, alpha, or resource sharing");
  decodeWorkerRecord(bytes, directory.path(), error, budget - 1);
  require(error.contains("before allocation"), "worker wire ignored budget");
  decode(bytes, error);
  require(!error.isEmpty(), "worker record accepted as portable preset");
  auto portable = encode(record, error);
  decodeWorkerRecord(portable, directory.path(), error);
  require(!error.isEmpty(), "portable codec accepted on worker return path");
  auto root        = QJsonDocument::fromJson(bytes).object();
  auto assets      = root["assets"].toObject();
  const QString id = assets.keys().front();
  auto badAssets   = assets;
  badAssets[id] =
      QJsonObject{{"png", "foreign codec payload"}, {"color", true}};
  decodeWorkerRecord(altered(bytes, "assets", badAssets), directory.path(),
                     error);
  require(!error.isEmpty(), "worker return accepted a codec payload");
  const QString path = QDir(directory.path()).filePath(id + ".rgba8");
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "cannot read wire resource");
  const QByteArray original = file.readAll();
  file.close();
  auto replace = [&](const QByteArray &data) {
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                file.write(data) == data.size(),
            "cannot mutate wire resource");
    file.close();
  };
  for (int kind = 0; kind < 4; ++kind) {
    QByteArray damaged = original;
    if (kind == 0) damaged.resize(7);
    if (kind == 1) damaged[0] = char(255);
    if (kind == 2) damaged.chop(1);
    if (kind == 3) damaged[8] = char(0);
    replace(damaged);
    decodeWorkerRecord(bytes, directory.path(), error);
    require(!error.isEmpty(), "malformed worker image accepted");
  }
  replace(original);
#ifndef _WIN32
  const QString target = directory.filePath("symlink-target");
  require(file.rename(target) && QFile::link(target, path),
          "cannot create symlink wire fixture");
  decodeWorkerRecord(bytes, directory.path(), error);
  require(!error.isEmpty(), "worker image symlink accepted");
#endif
}
int oversizedWorkerFixture(const QString &directory) {
  // A hostile worker result: each package fits its own resource limit, but
  // retaining both would require 320 MiB of float image pixels.
  QString error;
  Record record;
  auto root =
      QJsonDocument::fromJson(encodeWorkerRecord(record, directory, error))
          .object();
  require(error.isEmpty(), "cannot prepare worker budget fixture");
  for (int package = 0; package < 2; ++package) {
    QJsonObject assets;
    QJsonArray references;
    for (int index = 0; index < package + 2; ++index) {
      QByteArray raw(8 + 2048 * 1024 * 4, '\0');
      raw[2] = 8;
      raw[6] = 4;
      for (int pixel = 11; pixel < raw.size(); pixel += 4)
        raw[pixel] = char(17 + 34 * (index + package * 2));
      QString identity = QString::fromLatin1(
          QCryptographicHash::hash(raw + "mask", QCryptographicHash::Sha256)
              .toHex());
      QFile resource(QDir(directory).filePath(identity + ".rgba8"));
      require(resource.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                  resource.write(raw) == raw.size(),
              "cannot write raw worker fixture");
      assets[identity] = QJsonObject{{"rgba8", identity}, {"color", false}};
      references.append(identity);
    }
    auto settings    = root["settings"].toObject();
    auto tip         = settings["tip"].toObject();
    tip["shape"]     = "bitmap";
    tip["images"]    = references;
    settings["tip"]  = tip;
    root["settings"] = settings;
    root["assets"]   = assets;
    QFile file(QDir(directory).filePath(QString::number(package) + ".gzbrush"));
    auto bytes = QJsonDocument(root).toJson();
    require(
        file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
        "cannot write worker budget package");
  }
  QFile manifest(QDir(directory).filePath("result.json"));
  QByteArray bytes = QJsonDocument(QJsonObject{{"protocol", 2},
                                               {"count", 2},
                                               {"diagnostics", QJsonArray{}}})
                         .toJson();
  require(manifest.open(QIODevice::WriteOnly) &&
              manifest.write(bytes) == bytes.size(),
          "cannot write worker budget manifest");
  return 0;
}
QString workerExecutable() {
#ifdef RASTERBRUSH_SEPARATE_WORKER_TEST
  const QString path = QDir(QCoreApplication::applicationDirPath())
                           .filePath(
                               "rasterbrushworker_test"
#ifdef _WIN32
                               ".exe"
#endif
                           );
  require(QFileInfo::exists(path), "uninstrumented sandbox worker missing");
  return path;
#else
  return QCoreApplication::applicationFilePath();
#endif
}
void testWorkerPeakBudget() {
  QTemporaryDir directory;
  QString error;
  auto result =
      importBrushProcess(workerExecutable(),
                         directory.filePath("oversized-worker.fixture"), error);
  require(
      result.records.empty() && !result.complete &&
          error.contains("before allocation"),
      "multi-package result did not reject before decoding the excess image");
#ifdef __linux__
  rusage usage{};
  require(getrusage(RUSAGE_SELF, &usage) == 0,
          "cannot measure parent peak RSS");
  std::cout << "::notice::Brush import parent peak RSS: " << usage.ru_maxrss
            << " KiB\n";
  // Includes Qt, allocator and codec overhead, separately from the 256 MiB
  // resource budget. Run in its own non-sanitized CI process for valid RSS.
  require(usage.ru_maxrss < 320 * 1024,
          "parent import exceeded the qualified 320 MiB total-RSS envelope");
#endif
}
void testWorkerProcess() {
#if defined(_WIN32) && defined(RASTERBRUSH_SANDBOX_TESTING)
  {
    QProcess process;
    process.start(workerExecutable(), {"--test-windows-worker-limits"});
    const bool finished = process.waitForFinished(15000);
    const QString detail =
        QString("Windows worker limits: finished=%1 exit=%2 stderr=%3")
            .arg(finished)
            .arg(process.exitCode())
            .arg(QString::fromUtf8(process.readAllStandardError()).left(2048));
    require(finished && process.exitStatus() == QProcess::NormalExit &&
                process.exitCode() == 0,
            detail.toUtf8().constData());
  }
#endif

#if defined(__linux__) && defined(RASTERBRUSH_SANDBOX_TESTING)
  {
    QTemporaryDir root, output;
    require(root.isValid() && output.isValid(), "sandbox fixture unavailable");
    QFile input(root.filePath("input")), forbidden(root.filePath("private"));
    require(input.open(QIODevice::WriteOnly) && input.write("allowed") == 7 &&
                forbidden.open(QIODevice::WriteOnly) &&
                forbidden.write("private") == 7,
            "sandbox fixture write failed");
    input.close();
    forbidden.close();
    QProcess process;
    process.start(workerExecutable(),
                  {"--test-sandbox-worker", input.fileName(), output.path(),
                   forbidden.fileName()});
    const bool finished = process.waitForFinished(10000);
    const QString detail =
        QString("Linux sandbox probe: finished=%1 exit=%2 status=%3 stderr=%4")
            .arg(finished)
            .arg(process.exitCode())
            .arg(int(process.exitStatus()))
            .arg(QString::fromUtf8(process.readAllStandardError()).left(2048));
    require(finished && process.exitStatus() == QProcess::NormalExit &&
                process.exitCode() == 0 &&
                QFileInfo::exists(output.filePath("sandbox-passed")),
            detail.toUtf8().constData());
    require(
        forbidden.open(QIODevice::ReadOnly) && forbidden.readAll() == "private",
        "sandbox modified an unrelated file");
  }
#endif
  QTemporaryDir directory;
  require(directory.isValid(), "worker fixture directory unavailable");
  QString input = directory.filePath(
              QString::fromUtf8("\xE7\xAD\x86-original.gbr")),
          error;
  {
    QFile file(input);
    require(file.open(QIODevice::WriteOnly), "cannot write worker input");
    auto bytes = gbr();
    require(file.write(bytes) == bytes.size(), "short worker fixture write");
  }
  {
    const QString pngPath = directory.filePath("editor-tip.png");
    QImage source(2, 1, QImage::Format_ARGB32);
    source.setPixel(0, 0, qRgba(255, 0, 0, 128));
    source.setPixel(1, 0, qRgba(0, 255, 0, 255));
    require(source.save(pngPath, "PNG"), "cannot save editor tip fixture");
    auto image = importBrushProcess(workerExecutable(), pngPath, error);
    require(error.isEmpty() && image.complete && image.records.size() == 1 &&
                image.records.front().preset.tip.images.size() == 1,
            "worker PNG tip import failed");
    const auto tip = image.records.front().preset.tip.images.front();
    require(tip->color && tip->width == 2 && tip->height == 1 &&
                std::abs(tip->pixels[0].a - 128 / 255.0) < 1e-12 &&
                tip->pixels[0].r == tip->pixels[0].a && tip->pixels[1].g == 1,
            "worker PNG tip lost color or alpha");
    auto denied = importBrushProcess(workerExecutable(), pngPath, error,
                                     nullptr, sizeof(Pixel));
    require(!error.isEmpty() && !denied.complete && denied.records.empty(),
            "editor remaining budget was not enforced before PNG loading");
  }
  auto result = importBrushProcess(workerExecutable(), input, error);
  require(error.isEmpty() && result.complete && result.records.size() == 1,
          "worker-process GBR import failed");
  require(!result.records[0].referenceVerified && !result.diagnostics.empty(),
          "worker import lost conversion status");
  {
    QFile file(input);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "cannot replace worker input");
    file.write("invalid");
  }
  result = importBrushProcess(workerExecutable(), input, error);
  require(!error.isEmpty() && result.records.empty(),
          "malformed worker import returned partial records");
  std::atomic_bool cancelled(true);
  result = importBrushProcess(workerExecutable(), input, error, &cancelled);
  require(result.records.empty() && error.isEmpty(),
          "cancelled worker import published a result");
  cancelled.store(false);
  std::unique_ptr<QThread> cancellation(QThread::create([&cancelled]() {
    QThread::msleep(250);
    cancelled.store(true);
  }));
  cancellation->start();
  QElapsedTimer timer;
  timer.start();
  result = importBrushProcess(workerExecutable(),
                              directory.filePath("blocking-worker.fixture"),
                              error, &cancelled);
  cancellation->wait();
  require(result.records.empty() && error.isEmpty() && timer.elapsed() < 10000,
          "cancellation did not terminate the active import process");
  result = importBrushProcess(directory.filePath("missing-executable"), input,
                              error);
  require(result.records.empty() && !error.isEmpty(),
          "worker startup failure was ignored");
}
void writeCorpus(const QString &directory) {
  require(QDir().mkpath(directory), "cannot create original fuzz corpus");
  auto save = [&](const char *name, int format, const QByteArray &bytes) {
    QFile file(QDir(directory).filePath(QString::fromLatin1(name)));
    require(file.open(QIODevice::WriteOnly), "cannot write original corpus");
    QByteArray seed(1, char(format));
    seed += bytes;
    require(file.write(seed) == seed.size(), "short corpus write");
  };
  QString error;
  Record record;
  record.preset.opacity.mappings.push_back({Sensor::Pressure,
                                            {{0, 0}, {.5, 1}, {1, 0}},
                                            1,
                                            Interpolation::NaturalCubic});
  save("native", 0, encode(record, error));
  require(error.isEmpty(), "cannot encode native corpus");
  record.preset.count = 4;
  record.preset.countControl.mappings.push_back(
      {Sensor::DabIndex, {{0, 1}, {1, 0}}, 32});
  record.preset.angle.mappings.push_back(
      {Sensor::InitialDirection, {{0, 0}, {1, 1}}, 1});
  record.preset.scatterBothAxes = false;
  save("native-count-direction", 0, encode(record, error));
  require(error.isEmpty(), "cannot encode count/direction corpus");
  {
    QTemporaryDir wire;
    Record record;
    auto image   = std::make_shared<Image>();
    image->width = image->height = 1;
    image->pixels                = {{0, 0, 0, 1}};
    record.preset.tip.shape      = Shape::Bitmap;
    record.preset.tip.images.push_back(image);
    save("worker-metadata", 6, encodeWorkerRecord(record, wire.path(), error));
    require(error.isEmpty(), "cannot encode worker wire corpus");
    save("worker-rgba8", 7, QByteArray::fromHex("0000000100000001000000ff"));
    save("worker-oversized-header", 7,
         QByteArray::fromHex("ffffffffffffffff000000ff"));
  }
  save("pixel-kpp", 1, kpp());
  save("smudge-kpp", 1, kpp("colorsmudge"));
  save("gray-gbr", 2, gbr());
  save("rgba-gbr", 2, gbr(2, 4));
  save("sequence-gih", 3,
       QByteArray(
           "Original\n2 ncells:2 step:25 dim:1 rank0:2 sel0:incremental\n") +
           gbr() + gbr());
  QByteArray manifest =
      "<manifest:manifest "
      "xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" "
      "manifest:version=\"1.2\"><manifest:file-entry "
      "manifest:full-path=\"paintoppresets/original.kpp\" "
      "manifest:media-type=\"paintoppresets\"/></manifest:manifest>";
  save("bundle", 4,
       archive({{"mimetype", "application/x-krita-resourcebundle"},
                {"META-INF/manifest.xml", manifest},
                {"paintoppresets/original.kpp", kpp()}},
               true));
  manifest.replace(
      "</manifest:manifest>",
      "<manifest:file-entry manifest:full-path=\"brushes/original.gbr\" "
      "manifest:media-type=\"brushes\"/></manifest:manifest>");
  for (int application : {0, 1}) {
    save(
        application ? "bundle-color-stamp" : "bundle-color-mask", 4,
        archive(
            {{"mimetype", "application/x-krita-resourcebundle"},
             {"META-INF/manifest.xml", manifest},
             {"paintoppresets/original.kpp",
              externalKpp(QString("brushApplication=\"%1\"").arg(application))},
             {"brushes/original.gbr", gbr(2, 4)}},
            true));
  }
  save("sample-abr", 5, sampledAbr());
  save("computed-abr-v1", 5, computedAbr(1));
  save("computed-abr-v2", 5, computedAbr(2));
  // Original v6 descriptor-only seed: version 16, Brsh class, enab=true.
  // No tip is expected; fuzzing still traverses the bounded typed decoder.
  save("descriptor-abr", 5,
       QByteArray::fromHex("000600013842494d6465736300000021"
                           "0000001000000000000000004272736800000001"
                           "00000000656e6162626f6f6c01"));
}
}  // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
#if defined(_WIN32) && defined(RASTERBRUSH_SANDBOX_TESTING)
  if (argc == 2 && app.arguments().at(1) == "--test-windows-worker-limits")
    return probeWindowsImportLimits();
  if (argc == 2 && app.arguments().at(1) == "--test-worker-ping") return 0;
#endif

#if defined(__linux__) && defined(RASTERBRUSH_SANDBOX_TESTING)
  if (argc == 5 && app.arguments().at(1) == "--test-sandbox-worker")
    return probeImportSandbox(app.arguments().at(2), app.arguments().at(3),
                              app.arguments().at(4));
#endif
  if (argc == 4 &&
      QString::fromLocal8Bit(argv[1]) == "--raster-brush-import-worker") {
    if (app.arguments().at(2).endsWith("oversized-worker.fixture"))
      return oversizedWorkerFixture(app.arguments().at(3));
    // Test-only blocked decoder; the parent must kill it after cancellation.
    if (QString::fromLocal8Bit(argv[2]).endsWith("blocking-worker.fixture"))
      QThread::sleep(30);
    return runImportWorker(app.arguments().at(2), app.arguments().at(3));
  }
  try {
    if (argc == 2 && app.arguments().at(1) == "--test-worker-budget") {
      testWorkerPeakBudget();
      return 0;
    }
    testPackage();
    testImports();
    testBundles();
    testCollection();
    testWorkerWire();
    testWorkerProcess();
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--write-corpus")
      writeCorpus(QString::fromLocal8Bit(argv[2]));
    std::cout << "Native brush package tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
