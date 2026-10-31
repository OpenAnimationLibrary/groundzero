#include "toonz/rasterbrushpreset.h"
#include "rasterbrushfixture.h"
#include "../rasterbrushresponse.h"
#include "../rasterbrushtestobserver.h"
#include "toonz/rasterbrushimport.h"
#include <QCoreApplication>
#include <cstring>
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
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#endif
#ifdef __linux__
#include <sys/resource.h>
#endif
#include <iostream>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#if defined(__linux__) && defined(RASTERBRUSH_SANDBOX_TESTING)
namespace RasterBrush {
int probeImportSandbox(const QString &, const QString &, const QString &);
}
#endif
#if defined(__APPLE__) && defined(RASTERBRUSH_SANDBOX_TESTING)
namespace RasterBrush {
int probeMacWorkerTransport();
}
#endif
using namespace RasterBrush;
using RasterBrushTest::hardPixelKpp;
#if defined(_WIN32) && defined(RASTERBRUSH_SANDBOX_TESTING)
namespace RasterBrush {
int probeWindowsImportLimits();
int probeWindowsImportSandbox(const QString &, const QString &, const QString &,
                              const QString &);
int probeWindowsImportSandboxWorker(const QString &, const QString &,
                                    const QString &, quint16);
}  // namespace RasterBrush
#endif
namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void finishStroke(RasterBrush::Stroke &stroke) {
  const bool success = stroke.finish();
  require(success, stroke.error().c_str());
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
void testGihSelectionLifecycle() {
  // Original full-color frames make the selected index visible in the raster;
  // identical frames cannot qualify ordering or portable selector preservation.
  QByteArray frames;
  for (int channel = 0; channel < 3; ++channel) {
    QByteArray frame = gbr(2, 4);
    for (int pixel = 0; pixel < 2; ++pixel)
      for (int component = 0; component < 4; ++component)
        frame[frame.size() - 8 + pixel * 4 + component] =
            char(component == channel || component == 3 ? 255 : 0);
    frames += frame;
  }
  const QByteArray header =
      "Colors\n3 ncells:3 step:500 dim:1 cols:3 rows:1 rank0:3 ";
  struct Canvas : Surface {
    std::vector<Pixel> pixels = std::vector<Pixel>(512 * 32);
    int width() const override { return 512; }
    int height() const override { return 32; }
    bool prepare(int x0, int y0, int x1, int y1) override {
      return x0 >= 0 && y0 >= 0 && x1 <= width() && y1 <= height();
    }
    Pixel read(int x, int y) const override { return pixels.at(y * 512 + x); }
    void write(int x, int y, const Pixel &p) override {
      pixels.at(y * 512 + x) = p;
    }
    double coverage(int, int) const override { return 1; }
  };
  QString error;
  for (bool random : {false, true}) {
    const QByteArray selector = random ? "random" : "incremental";
    auto imported             = importBrush(
        header + "selection:" + selector + "\n" + frames, "gih", error);
    require(imported.complete && imported.records.size() == 1,
            qPrintable(error));
    const auto &record = imported.records.front();
    require(record.preset.profile == "groundzero-native-1" && record.decoded &&
                !record.referenceVerified &&
                !record.provenance["sourceSemanticallySupported"].toBool() &&
                !record.provenance["sourceReferenceVerified"].toBool(),
            "GIH conversion invented source qualification");
    const auto restored = decode(encode(record, error), error);
    require(error.isEmpty() && restored.preset.tip.images.size() == 3 &&
                restored.preset.tip.randomSequence == random,
            "GIH portable frame selection changed");
    auto render = [&](const Preset &preset, bool split) {
      Canvas canvas;
      Stroke stroke(canvas, preset);
      Sample point;
      point.x = 10.5;
      point.y = 10.5;
      require(stroke.add(point), stroke.error().c_str());
      if (split) {
        point.x    = 160.5;
        point.time = .5;
        require(stroke.add(point), stroke.error().c_str());
      }
      point.x    = 321.5;
      point.time = 1;
      require(stroke.add(point), stroke.error().c_str());
      require(stroke.dabCount() == 32, "GIH fixture dab count changed");
      finishStroke(stroke);
      std::vector<int> selected;
      for (int dab = 0; dab < 32; ++dab) {
        const Pixel p = canvas.read(10 + dab * 10, 10);
        require(p.a > .999, "GIH frame lost color/alpha during sampling");
        const int index = p.r > .999 ? 0 : p.g > .999 ? 1 : p.b > .999 ? 2 : -1;
        require(index >= 0, "GIH selected an invalid or blended frame");
        selected.push_back(index);
      }
      return selected;
    };
    const auto selected = render(record.preset, false);
    require(selected == render(restored.preset, true) &&
                selected == render(record.preset, false),
            "GIH selection changed after reopen, restart or event partition");
    if (random) {
      Preset resolved            = restored.preset;
      resolved.varySeedPerStroke = true;
      require(selected == render(resolved, false) &&
                  selected == render(resolved, true),
              "raw renderer replaced its resolved seed using host policy");
      Preset otherSeed = restored.preset;
      ++otherSeed.seed;
      require(selected != render(otherSeed, false), "GIH random seed ignored");
      for (int frame = 0; frame < 3; ++frame)
        require(std::find(selected.begin(), selected.end(), frame) !=
                    selected.end(),
                "GIH random sequence never selected an original frame");
    } else
      for (size_t dab = 0; dab < selected.size(); ++dab)
        require(selected[dab] == int(dab % 3),
                "GIH incremental sequence did not wrap");
  }
  for (const QByteArray &parameters :
       {QByteArray("3 ncells:3 step:25 dim:2 rank0:3 sel0:incremental"),
        QByteArray("3 ncells:2 step:25 dim:1 rank0:3 sel0:incremental"),
        QByteArray("3 ncells:3 step:25 dim:1 rank0:2 sel0:incremental"),
        QByteArray(
            "3 ncells:3 step:25 dim:1 rank0:999999999999 sel0:incremental"),
        QByteArray("3 ncells:3 step:25 dim:1 rank0:3 rank0:3 sel0:incremental"),
        QByteArray("3 ncells:3 step:nan dim:1 rank0:3 sel0:incremental"),
        QByteArray("3 ncells:3 step:25 dim:1 rank0:3 sel0:random "
                   "selection:incremental"),
        QByteArray("3 ncells:3 step:25 dim:1 rank0:3 sel0:pressure")})
    require(
        !importBrush("Malformed\n" + parameters + "\n" + frames, "gih", error)
             .complete,
        "GIH malformed or unsupported selection metadata accepted");
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
// Original pixels/names and values; observed ABR6.2 typed record layout.
QByteArray boundAbr(bool missing = false, bool duplicate = false,
                    int version = 6, bool badUnit = false, bool dual = false,
                    bool dynamics = true, const char *extraFlag = nullptr) {
  auto be = [](quint64 value, int n = 4) {
    QByteArray out;
    for (int i = n - 1; i >= 0; --i) out.append(char(value >> (i * 8)));
    return out;
  };
  auto id   = [&](QByteArray value) { return be(value.size()) + value; };
  auto text = [&](QString value) {
    QByteArray out = be(value.size());
    for (auto c : value) out += be(c.unicode(), 2);
    return out;
  };
  auto field = [&](const char *key, const char *type, QByteArray value) {
    return id(key) + type + value;
  };
  auto unit = [&](double value, const char *suffix) {
    quint64 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return QByteArray(suffix) + be(bits, 8);
  };
  auto object = [&](const char *klass, int count, QByteArray fields) {
    return text("") + id(klass) + be(count) + fields;
  };
  auto section = [&](const char *tag, QByteArray data) {
    QByteArray out = QByteArray("8BIM") + tag + be(data.size()) + data;
    while (out.size() % 4) out += char(0);
    return out;
  };
  QByteArray sample(301, char(0));
  const QByteArray sampleId("00000000-0000-0000-0000-000000000001");
  sample[0] = char(sampleId.size());
  sample.replace(1, sampleId.size(), sampleId);
  for (int v : {0, 0, 1, 2}) sample += be(v);
  sample += be(8, 2) + char(0) + char(255) + char(64);
  QByteArray samples = be(sample.size()) + sample;
  while (samples.size() % 4) samples += char(0);
  if (duplicate) samples += samples;
  QByteArray presets;
  for (int index = 0; index < 2; ++index) {
    const auto shape = object(
        "sampledBrush", 9,
        field("Dmtr", "UntF", unit(24 + index, badUnit ? "#Prc" : "#Pxl")) +
            field("Angl", "UntF", unit(-20, "#Ang")) +
            field("Rndn", "UntF", unit(75, "#Prc")) +
            field("Spcn", "UntF", unit(30, "#Prc")) +
            field("Intr", "bool", QByteArray(1, char(1))) +
            field("flipX", "bool", QByteArray(1, char(1))) +
            field("flipY", "bool", QByteArray(1, char(0))) +
            field("Nm  ", "TEXT", text("Original sample")) +
            field("sampledData", "TEXT",
                  text(missing ? "missing" : QString::fromLatin1(sampleId))));
    auto fields =
        field("Nm  ", "TEXT",
              text(index ? "Original second" : "Original first")) +
        field("Brsh", "Objc", shape) +
        field("useTipDynamics", "bool", QByteArray(1, char(dynamics)));
    if (extraFlag) fields += field(extraFlag, "bool", QByteArray(1, char(1)));
    if (dual)
      fields += field(
          "dualBrush", "Objc",
          object("dualBrush", 2,
                 field("useDualBrush", "bool", QByteArray(1, char(1))) +
                     field("BlnM", "enum", id("BlnM") + id("colorBurn"))));
    presets +=
        QByteArray("Objc") +
        object("brushPreset", (dual ? 4 : 3) + (extraFlag ? 1 : 0), fields);
  }
  const auto descriptor =
      be(16) + object("null", 1, field("Brsh", "VlLs", be(2) + presets));
  return be(version, 2) + be(2, 2) + section("samp", samples) +
         section("patt", {}) + section("desc", descriptor);
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
void testSourceBitmapImport() {
  QString error;
  for (const QString &variant :
       {QString("unit"), QString("double"), QString("fractional"),
        QString("downscale"), QString("rotation"), QString("unknown"),
        QString("color"), QString("color-image"), QString("wash"),
        QString("gih-incremental"), QString("gih-random"), QString("png-mask"),
        QString("png-mask-unknown"), QString("mirror-x"), QString("mirror-y"),
        QString("mirror-both"), QString("mirror-disabled"),
        QString("mirror-unknown")}) {
    const bool sequence = variant.startsWith("gih-");
    QImage image =
        QImage::fromData(RasterBrushTest::hardPixelKpp(
                             variant == "wash" ? "freehandWash" : "buildUp"),
                         "PNG");
    QString xml          = image.text("preset");
    const QString marker = "<param name=\"brush_definition\">";
    const int begin      = xml.indexOf(marker) + marker.size();
    const int end        = xml.indexOf("</param>", begin);
    require(begin >= marker.size() && end > begin,
            "bitmap preset fixture missing definition");
    QString definition =
        "<Brush type=\"gbr_brush\" BrushVersion=\"2\" "
        "filename=\"original.gbr\" "
        "scale=\"%1\" spacing=\"4\" angle=\"0\" useAutoSpacing=\"0\" "
        "autoSpacingCoeff=\"1\" ColorAsMask=\"1\" preserveLightness=\"0\"/>";
    definition         = definition.arg(variant == "double"       ? "2"
                                        : variant == "fractional" ? "1.5"
                                        : variant == "downscale"  ? ".75"
                                                                  : "1");
    const bool pngMask = variant.startsWith("png-mask");
    if (pngMask) {
      definition.replace("gbr_brush", "png_brush");
      definition.replace("original.gbr", "original.png");
    }
    if (variant == "rotation")
      definition.replace("angle=\"0\"", "angle=\"1.5707963267948966\"");
    if (variant == "color-image" || sequence)
      definition.replace("ColorAsMask=\"1\"", "ColorAsMask=\"0\"");
    if (variant == "unknown" || variant == "png-mask-unknown")
      definition.replace("<Brush ", "<Brush unimplemented=\"1\" ");
    if (sequence) definition.replace("original.gbr", "original.gih");
    xml.replace(begin, end - begin, definition.toHtmlEscaped());
    if (variant.startsWith("mirror-")) {
      QString settings;
      auto field = [&](const QString &key, const QString &value) {
        settings +=
            "<param name=\"" + key + "\">" + value.toHtmlEscaped() + "</param>";
      };
      field("PressureMirror", variant == "mirror-disabled" ? "false" : "true");
      field("HorizontalMirrorEnabled",
            variant == "mirror-y" ? "false" : "true");
      field("VerticalMirrorEnabled", variant == "mirror-x" ? "false" : "true");
      field("MirrorUseCurve", "true");
      field("MirrorUseSameCurve", "true");
      field("MirrorValue", "1");
      field("MirrorcurveMode", "0");
      field("MirrorcommonCurve", "0,0;1,1;");
      field("MirrorSensor",
            variant == "mirror-unknown"
                ? "<params id=\"xtilt\"><curve>0,0;1,1;</curve></params>"
                : "<params id=\"pressure\"><curve>0,0;1,1;</curve></params>");
      require(xml.count("</Preset>") == 1,
              "missing bitmap fixture preset root");
      xml.replace("</Preset>", settings + "</Preset>");
    }
    image.setText("preset", xml);
    QByteArray preset;
    QBuffer output(&preset);
    output.open(QIODevice::WriteOnly);
    require(image.save(&output, "PNG"), "cannot encode source bitmap fixture");
    QByteArray manifest =
        "<manifest:manifest "
        "xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" "
        "manifest:version=\"1.2\"><manifest:file-entry "
        "manifest:full-path=\"paintoppresets/original.kpp\" "
        "manifest:media-type=\"paintoppresets\"/><manifest:file-entry "
        "manifest:full-path=\"brushes/original.gbr\" "
        "manifest:media-type=\"brushes\"/></manifest:manifest>";
    QByteArray resource =
        gbr(2, variant.startsWith("color") || sequence ? 4 : 1);
    if (sequence) {
      manifest.replace("original.gbr", "original.gih");
      resource =
          "Original sequence\n3 ncells:3 step:400 dim:1 cols:1 rows:1 "
          "cellwidth:2 cellheight:1 rank0:3 selection:" +
          (variant == "gih-random" ? QByteArray("random\n")
                                   : QByteArray("incremental\n"));
      for (int frame = 0; frame < 3; ++frame) {
        QByteArray image = gbr(2, 4);
        for (int pixel = 0; pixel < 2; ++pixel)
          for (int component = 0; component < 4; ++component)
            image[image.size() - 8 + pixel * 4 + component] =
                char(component == frame || component == 3 ? 255 : 0);
        resource += image;
      }
    }
    if (pngMask) {
      QImage original(2, 2, QImage::Format_ARGB32);
      original.fill(qRgba(47, 131, 219, 0));
      original.setPixel(0, 0, qRgba(47, 131, 219, 255));
      original.setPixel(0, 1, qRgba(127, 61, 19, 128));
      resource.clear();
      QBuffer encoded(&resource);
      encoded.open(QIODevice::WriteOnly);
      require(original.save(&encoded, "PNG"),
              "cannot encode original PNG mask");
      manifest.replace("original.gbr", "original.png");
    }
    const auto imported =
        importBrush(archive({{"mimetype", "application/x-krita-resourcebundle"},
                             {"META-INF/manifest.xml", manifest},
                             {"paintoppresets/original.kpp", preset},
                             {sequence  ? QByteArray("brushes/original.gih")
                              : pngMask ? QByteArray("brushes/original.png")
                                        : QByteArray("brushes/original.gbr"),
                              resource}},
                            true),
                    "bundle", error);
    require(error.isEmpty() && imported.records.size() == 1,
            "source bitmap bundle failed");
    const bool candidate =
        variant == "unit" || variant == "double" || variant == "fractional" ||
        variant == "rotation" || variant == "color-image" ||
        variant == "png-mask" || variant == "gih-incremental" ||
        (variant.startsWith("mirror-") && variant != "mirror-unknown");
    const auto &record = imported.records.front();
    require(
        (record.preset.profile ==
         (pngMask ? "krita-5.3.4.1-fixed-bitmap-mask-candidate-1"
                  : "krita-5.3.4.1-fixed-bitmap-candidate-1")) == candidate,
        "source bitmap admission ignored exact settings or resource origin");
    if (pngMask) {
      require(record.preset.tip.images[0]->color == candidate,
              "PNG mask lost original RGBA or native fallback conversion");
      if (candidate)
        require(std::abs(record.preset.tip.images[0]->pixels[0].r -
                         47 / 255.0) < 1e-12,
                "PNG source mask was converted before sampling");
    }
    if (!candidate) continue;
    const auto restored = decode(encode(record, error), error);
    require(
        error.isEmpty() && restored.decoded &&
            restored.preset.profile == record.preset.profile &&
            restored.preset.tip.images[0]->width == 2 &&
            restored.preset.tip.images[0]->pixels[0].a == 1 &&
            restored.preset.tip.images[0]->pixels[1].a == (sequence ? 1 : 0),
        "source bitmap codec changed candidate/resources");
    if (variant.startsWith("mirror-")) {
      const auto &mirror = restored.preset.sourceMirror;
      require(mirror.enabled == (variant != "mirror-disabled"),
              "source Mirror enable changed on portable roundtrip");
      if (mirror.enabled)
        require(mirror.horizontal == (variant != "mirror-y") &&
                    mirror.vertical == (variant != "mirror-x") &&
                    mirror.control.mappings.size() == 1 &&
                    mirror.control.mappings[0].sensor == Sensor::Pressure,
                "source Mirror axes/control changed on portable roundtrip");
    }
    const auto portable = importBrush(
        encodePortable(record, ResourcePackaging::PrivateCopy, error),
        "gzbrush", error);
    require(error.isEmpty() && portable.records.size() == 1 &&
                portable.records[0].preset.profile == record.preset.profile &&
                !record.provenance["sourceReferenceVerified"].toBool(),
            "source bitmap portable reopen lost profile or invented parity");
    if (sequence) {
      require(restored.preset.tip.images.size() == 3 &&
                  !restored.preset.tip.randomSequence,
              "Incremental GIH portable order changed");
      struct Canvas : Surface {
        std::vector<Pixel> pixels = std::vector<Pixel>(512 * 32);
        int width() const override { return 512; }
        int height() const override { return 32; }
        bool prepare(int x0, int y0, int x1, int y1) override {
          return x0 >= 0 && y0 >= 0 && x1 <= width() && y1 <= height();
        }
        Pixel read(int x, int y) const override {
          return pixels.at(y * 512 + x);
        }
        void write(int x, int y, const Pixel &p) override {
          pixels.at(y * 512 + x) = p;
        }
        double coverage(int, int) const override { return 1; }
      };
      for (bool endpointAtSlot : {false, true})
        for (bool split : {false, true}) {
          Canvas canvas;
          Stroke stroke(canvas, portable.records[0].preset);
          Sample point;
          point.x = 10;
          point.y = 10;
          require(stroke.add(point), stroke.error().c_str());
          if (split) {
            point.x    = 133;
            point.time = .5;
            require(stroke.add(point), stroke.error().c_str());
          }
          point.x    = endpointAtSlot ? 258 : 259;
          point.time = 1;
          require(stroke.add(point) && stroke.finish(), stroke.error().c_str());
          const int count = endpointAtSlot ? 31 : 32;
          require(stroke.dabCount() == size_t(count),
                  "Source GIH count/endpoints changed");
          if (endpointAtSlot)
            require(canvas.read(258, 10).a == 0,
                    "Source GIH painted the excluded final endpoint");
          for (int dab = 0; dab < count; ++dab) {
            const Pixel pixel       = canvas.read(10 + 8 * dab, 10);
            const double channels[] = {pixel.r, pixel.g, pixel.b};
            require(
                pixel.a > 0 && channels[dab % 3] > .99 * pixel.a &&
                    channels[(dab + 1) % 3] == 0 &&
                    channels[(dab + 2) % 3] == 0,
                "Source GIH did not advance per dab or restart at frame zero");
          }
        }
    }
  }
}
void testAirbrushCandidateCodec() {
  QString error;
  Record record;
  record.decoded                      = true;
  record.semanticallySupported        = true;
  record.preset.profile               = "krita-5.3.4.1-airbrush-candidate-1";
  record.preset.size.value            = 16;
  record.preset.spacing               = 1;
  record.preset.opacity.value         = .1;
  record.preset.airbrushRate          = 20;
  record.preset.airbrushIgnoreSpacing = true;
  const auto bytes =
      encodePortable(record, ResourcePackaging::PrivateCopy, error);
  const auto restored = decode(bytes, error);
  require(
      error.isEmpty() && restored.preset.profile == record.preset.profile &&
          restored.preset.airbrushIgnoreSpacing &&
          restored.preset.airbrushRate == 20 && !restored.referenceVerified &&
          !restored.provenance["sourceReferenceVerified"].toBool(),
      "Explicit airbrush candidate portable state lost or falsely qualified");
  auto root                         = QJsonDocument::fromJson(bytes).object();
  auto settings                     = root["settings"].toObject();
  settings["airbrushIgnoreSpacing"] = "true";
  root["settings"]                  = settings;
  decode(QJsonDocument(root).toJson(), error);
  require(!error.isEmpty(), "Malformed airbrush ignore-spacing type accepted");
  settings.remove("airbrushIgnoreSpacing");
  root["settings"]     = settings;
  const auto defaulted = decode(QJsonDocument(root).toJson(), error);
  require(
      error.isEmpty() && !defaulted.preset.airbrushIgnoreSpacing,
      "Missing optional airbrush field failed to preserve distance default");
  record.preset.profile = "groundzero-native-1";
  require(!validate(record.preset).empty(),
          "Source-only ignore-spacing silently accepted by native profile");
}
void testBundles() {
  testAirbrushCandidateCodec();
  testSourceBitmapImport();
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
  // Each procedural preset retains a texture but no bitmap primary tip. The
  // second decode must receive the budget remaining after the first texture.
  auto textured = entries;
  textured[1].second.replace(
      "</manifest:manifest>",
      "<manifest:file-entry manifest:full-path=\"paintoppresets/second.kpp\" "
      "manifest:media-type=\"paintoppresets\"/></manifest:manifest>");
  textured[2].second = hardPixelKpp("texture-valid");
  textured.push_back({"paintoppresets/second.kpp", textured[2].second});
  const size_t texturePixels    = 16 * 16;
  const size_t oneTextureDecode = texturePixels * (sizeof(Pixel) + 16);
  const auto texturedBytes      = archive(textured, true);
  const auto tight =
      importBrush(texturedBytes, "bundle", error, nullptr, oneTextureDecode);
  require(!tight.complete && tight.records.empty() && !error.isEmpty(),
          "bundle reused retained texture memory allowance for next decode");
  const auto enough =
      importBrush(texturedBytes, "bundle", error, nullptr,
                  texturePixels * sizeof(Pixel) + oneTextureDecode);
  require(enough.complete && enough.records.size() == 2 && error.isEmpty() &&
              enough.records[0].preset.texture.image &&
              enough.records[1].preset.texture.image,
          "bundle rejected correctly budgeted retained textures");
  // Source pattern lookup is type-scoped, including hash matches. A brush
  // PNG with identical bytes must neither satisfy nor make a pattern ambiguous.
  QImage linkedPreset = QImage::fromData(hardPixelKpp("texture-valid"), "PNG");
  QString linkedXml   = linkedPreset.text("preset");
  const QString patternStart = "<param name=\"Texture/Pattern/Pattern\">";
  const int patternAt        = linkedXml.indexOf(patternStart);
  const int patternEnd       = linkedXml.indexOf("</param>", patternAt);
  require(patternAt >= 0 && patternEnd > patternAt,
          "missing original texture fixture");
  const QByteArray patternBytes = QByteArray::fromBase64(
      linkedXml
          .mid(patternAt + patternStart.size(),
               patternEnd - patternAt - patternStart.size())
          .toLatin1());
  linkedXml.remove(patternAt, patternEnd + 8 - patternAt);
  linkedXml.replace(
      "</Preset>",
      "<param "
      "name=\"Texture/Pattern/PatternFileName\">linked.png</param></Preset>");
  for (bool useHash : {false, true}) {
    QString xml = linkedXml;
    if (!useHash) {
      const int start =
          xml.indexOf("<param name=\"Texture/Pattern/PatternMD5Sum\">");
      const int end = xml.indexOf("</param>", start);
      require(start >= 0 && end > start, "missing texture checksum fixture");
      xml.remove(start, end + 8 - start);
    }
    linkedPreset.setText("preset", xml);
    QByteArray presetBytes;
    QBuffer output(&presetBytes);
    output.open(QIODevice::WriteOnly);
    require(linkedPreset.save(&output, "PNG"),
            "cannot encode linked texture fixture");
    for (int scenario = 0; scenario < 4; ++scenario) {
      auto linked      = entries;
      linked[2].second = presetBytes;
      auto resource    = [&](const QByteArray &path, const QByteArray &kind) {
        linked[1].second.replace("</manifest:manifest>",
                                    "<manifest:file-entry manifest:full-path=\"" +
                                        path + "\" manifest:media-type=\"" + kind +
                                        "\"/></manifest:manifest>");
        linked.push_back({path, patternBytes});
      };
      resource("assets/linked.png", scenario == 0 ? "brushes" : "patterns");
      if (scenario >= 2)
        resource("other/linked.png", scenario == 2 ? "brushes" : "patterns");
      const auto result = importBrush(archive(linked, true), "bundle", error);
      if (scenario == 0 || scenario == 3)
        require(!error.isEmpty() && result.records.empty(),
                "wrong-kind or ambiguous pattern dependency accepted");
      else
        require(error.isEmpty() && result.records.size() == 1 &&
                    result.records.front().preset.texture.image,
                "pattern type lookup confused identical brush resource");
    }
  }
  // Original fixture follows Krita's manifest writer: namespaced tags contain
  // text labels on each resource, independently of the brush preset name.
  auto tagged = entries;
  tagged[1].second.replace(
      "manifest:media-type=\"paintoppresets\"/>",
      "manifest:media-type=\"paintoppresets\"><manifest:tags>"
      "<manifest:tag> Ink &amp; wash </manifest:tag>"
      "<manifest:tag>水彩</manifest:tag><manifest:tag>Ink</manifest:tag>"
      "<manifest:tag>Ink</manifest:tag></manifest:tags></manifest:file-entry>");
  const QJsonArray expectedTags{QString(" Ink & wash "),
                                QString::fromUtf8("水彩"), QString("Ink"),
                                QString("Ink")};
  for (bool compression : {false, true}) {
    auto imported = importBrush(archive(tagged, compression), "bundle", error);
    require(imported.complete && imported.records.size() == 1,
            qPrintable(error));
    const auto original = imported.records.front();
    const auto metadata = original.provenance["bundleMetadata"].toObject();
    require(metadata["version"].toInt() == 1 &&
                metadata["presetTags"].toArray() == expectedTags &&
                metadata["resourceTags"].toArray().size() == 1,
            "bundle tag labels or association lost");
    const auto bytes = encode(original, error);
    require(error.isEmpty(), qPrintable(error));
    const auto native = decode(bytes, error);
    require(error.isEmpty() && native.provenance["bundleMetadata"] == metadata,
            "native preset tag roundtrip failed");
    for (auto mode :
         {ResourcePackaging::ManifestOnly, ResourcePackaging::PermittedOnly,
          ResourcePackaging::PrivateCopy}) {
      const auto portable = encodePortable(original, mode, error);
      require(error.isEmpty() && !portable.isEmpty(), qPrintable(error));
      // This original preset has no external images, so all packaging modes
      // reopen.
      const auto reopened = decode(portable, error);
      require(
          error.isEmpty() && reopened.provenance["bundleMetadata"] == metadata,
          "portable preset tag roundtrip failed");
    }
    QTemporaryDir workerDirectory;
    require(workerDirectory.isValid(), "cannot create tag worker fixture");
    const auto wire =
        encodeWorkerRecord(original, workerDirectory.path(), error);
    require(error.isEmpty(), qPrintable(error));
    const auto workerRecord =
        decodeWorkerRecord(wire, workerDirectory.path(), error);
    require(error.isEmpty() &&
                workerRecord.provenance["bundleMetadata"] == metadata,
            "worker protocol lost bundle tags");
    const auto filename = workerDirectory.filePath("tags.gzbrush");
    require(save(filename, original, error), qPrintable(error));
    const auto loaded = load(filename, error);
    require(error.isEmpty() && loaded.provenance["bundleMetadata"] == metadata,
            "saved preset lost bundle tags");
    auto forged                         = original;
    auto invalid                        = metadata;
    invalid["version"]                  = 2;
    forged.provenance["bundleMetadata"] = invalid;
    require(encode(forged, error).isEmpty() && !error.isEmpty(),
            "unknown bundle metadata version accepted");
    auto malformedNative     = QJsonDocument::fromJson(bytes).object();
    auto malformedProvenance = malformedNative["provenance"].toObject();
    malformedProvenance["bundleMetadata"] = invalid;
    malformedNative["provenance"]         = malformedProvenance;
    const auto rejected =
        decode(QJsonDocument(malformedNative).toJson(), error);
    require(!error.isEmpty() && !rejected.decoded,
            "decoder accepted unknown bundle metadata version");
    auto resources                      = metadata["resourceTags"].toArray();
    auto entry                          = resources[0].toObject();
    entry["member"]                     = "paintoppresets/../../private";
    resources[0]                        = entry;
    invalid                             = metadata;
    invalid["resourceTags"]             = resources;
    forged.provenance["bundleMetadata"] = invalid;
    require(encodePortable(forged, ResourcePackaging::PrivateCopy, error)
                    .isEmpty() &&
                !error.isEmpty(),
            "unsafe bundle member metadata exported");
  }
  for (const QByteArray &badTag :
       {QByteArray(
            "<manifest:tag><manifest:tag>nested</manifest:tag></manifest:tag>"),
        QByteArray("<tag xmlns=\"wrong\">namespace</tag>"),
        QByteArray("<manifest:tag>") + QByteArray(257, 'x') +
            "</manifest:tag>"}) {
    auto malformed = entries;
    malformed[1].second.replace(
        "manifest:media-type=\"paintoppresets\"/>",
        QByteArray("manifest:media-type=\"paintoppresets\"><manifest:tags>") +
            badTag + "</manifest:tags></manifest:file-entry>");
    require(!importBrush(archive(malformed, false), "bundle", error).complete &&
                !error.isEmpty(),
            "unsupported bundle tag structure discarded");
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
  auto wrongTipKind = external;
  wrongTipKind[1].second.replace("manifest:media-type=\"brushes\"",
                                 "manifest:media-type=\"patterns\"");
  const auto wrongTip =
      importBrush(archive(wrongTipKind, true), "bundle", error);
  require(!error.isEmpty() && wrongTip.records.empty(),
          "pattern-only resource satisfied brush-tip dependency");
  auto resourceTagged = external;
  resourceTagged[1].second.replace("manifest:media-type=\"brushes\"/>",
                                   "manifest:media-type=\"brushes\"><manifest:"
                                   "tags><manifest:tag>Tips</manifest:tag>"
                                   "</manifest:tags></manifest:file-entry>");
  const auto withResourceTags =
      importBrush(archive(resourceTagged, true), "bundle", error);
  require(withResourceTags.complete, qPrintable(error));
  const auto resourceMetadata =
      withResourceTags.records[0].provenance["bundleMetadata"].toObject();
  const auto resourceLabels = resourceMetadata["resourceTags"].toArray();
  require(
      resourceMetadata["presetTags"].toArray().isEmpty() &&
          resourceLabels.size() == 1 &&
          resourceLabels[0].toObject()["member"] == "brushes/original.gbr" &&
          resourceLabels[0].toObject()["tags"].toArray() ==
              QJsonArray{QString("Tips")},
      "dependency tags lost or incorrectly promoted to preset tags");
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
void testSourceMaskMalformedCodec() {
  QString error;
  const auto imported =
      importBrush(RasterBrushTest::maskedPixelKpp(QString()), "kpp", error);
  require(imported.records.size() == 1 && error.isEmpty(),
          "masked codec fixture import failed");
  const auto bytes = encode(imported.records.front(), error);
  require(!bytes.isEmpty() && error.isEmpty(), "masked codec fixture failed");
  const auto root     = QJsonDocument::fromJson(bytes).object();
  const auto settings = root["settings"].toObject();
  const auto original = settings["sourceMaskedV1"].toObject();
  require(original.size() == 9, "masked codec fixture has unexpected fields");
  auto reject = [&](const QJsonValue &value) {
    auto changed               = root;
    auto controls              = settings;
    controls["sourceMaskedV1"] = value;
    changed["settings"]        = controls;
    const auto rejected        = decode(QJsonDocument(changed).toJson(), error);
    require(!error.isEmpty() && !rejected.decoded,
            "malformed source mask codec fields accepted");
  };
  for (auto field = original.begin(); field != original.end(); ++field) {
    auto missing = original;
    missing.remove(field.key());
    reject(missing);
    auto wrongType         = original;
    wrongType[field.key()] = QJsonArray{field.value()};
    reject(wrongType);
    auto nullField         = original;
    nullField[field.key()] = QJsonValue(QJsonValue::Null);
    reject(nullField);
  }
  auto extra                = original;
  extra["futureActiveMask"] = true;
  reject(extra);
  auto unknownMode    = original;
  unknownMode["mode"] = "unimplemented-mask-mode";
  reject(unknownMode);
  reject(QJsonArray{});
  reject(QJsonValue(QJsonValue::Null));
  const auto valid = decode(bytes, error);
  require(error.isEmpty() && valid.decoded && valid.preset.sourceMasked.enabled,
          "malformed decode poisoned subsequent valid mask decode");
}
void testSourceMaskImport() {
  struct Canvas : Surface {
    std::vector<Pixel> pixels = std::vector<Pixel>(128 * 64);
    int width() const override { return 128; }
    int height() const override { return 64; }
    bool prepare(int x0, int y0, int x1, int y1) override {
      return x0 >= 0 && y0 >= 0 && x1 <= width() && y1 <= height();
    }
    Pixel read(int x, int y) const override { return pixels.at(y * 128 + x); }
    void write(int x, int y, const Pixel &p) override {
      pixels.at(y * 128 + x) = p;
    }
  };
  QString error;
  for (const QString &variant :
       {QString(), QString("subtract"), QString("buildUp"), QString("dynamic"),
        QString("unknown")}) {
    const auto imported =
        importBrush(RasterBrushTest::maskedPixelKpp(variant), "kpp", error);
    require(imported.records.size() == 1, "masked KPP import failed");
    const auto &record   = imported.records.front();
    const bool supported = variant != "dynamic" && variant != "unknown";
    require((record.preset.profile == "krita-5.3.4.1-hard-pixel-candidate-1") ==
                supported,
            "masked source eligibility ignored active unknown settings");
    if (!supported) continue;
    const auto restored = decode(encode(record, error), error);
    require(
        error.isEmpty() && restored.preset.sourceMasked.opacity == .5 &&
            restored.preset.sourceMasked.flow == .5 &&
            restored.preset.sourceMasked.enabled == (variant != "buildUp") &&
            !record.provenance["sourceReferenceVerified"].toBool(),
        "source mask portable settings or qualification changed");
    const auto portable = importBrush(
        encodePortable(record, ResourcePackaging::PrivateCopy, error),
        "gzbrush", error);
    require(portable.records.size() == 1 &&
                portable.records.front().preset.sourceMasked.enabled ==
                    restored.preset.sourceMasked.enabled,
            "source mask portable export lost source fields");
    Canvas canvas;
    Stroke stroke(canvas, restored.preset);
    Sample a;
    a.x      = 32;
    a.y      = 32;
    Sample b = a;
    b.x      = 33;
    b.time   = 1;
    require(stroke.add(a) && stroke.add(b) && stroke.finish(),
            "imported source mask runtime failed");
    const double expected = variant == "buildUp"    ? 1
                            : variant == "subtract" ? 191 / 255.
                                                    : 64 / 255.;
    require(
        std::abs(canvas.read(32, 32).a - expected) < 1e-12,
        "imported source mask did not execute post-accumulation opacity/flow");
  }
}
void testImports() {
  testSourceMaskMalformedCodec();
  testSourceMaskImport();
  QString error;
  const size_t abrBytes = 2 * (sizeof(Pixel) + 1);
  require(importBrush(sampledAbr(), "abr", error, nullptr, abrBytes).complete,
          "exact ABR scratch/resource budget rejected");
  require(
      !importBrush(sampledAbr(), "abr", error, nullptr, abrBytes - 1).complete,
      "retained ABR byte masks not charged to allocation budget");
  auto bound =
      importBrush(boundAbr(false, false, 6, false, true), "abr", error);
  require(bound.complete && bound.records.size() == 2,
          "ABR6.2 presets sharing one sample were collapsed");
  require(bound.records[0].preset.tip.images[0] ==
              bound.records[1].preset.tip.images[0],
          "ABR preset aliases duplicated decoded image memory");
  for (int i = 0; i < 2; ++i) {
    const auto &r = bound.records[i];
    require(r.preset.name == (i ? "Original second" : "Original first") &&
                r.preset.size.value == 24 + i && r.preset.angle.value == -20 &&
                r.preset.aspect.value == .75 && r.preset.spacing == .3 &&
                r.preset.flipX && !r.preset.flipY &&
                !r.preset.secondary.enabled &&
                !r.provenance["sourceSemanticallySupported"].toBool() &&
                !r.provenance["sourceReferenceVerified"].toBool() &&
                r.provenance["sourcePresetOrdinal"].toInt() == i + 1 &&
                r.provenance["conversionKind"].toString() == "partial-native" &&
                !r.diagnostics.join(" ").contains("Tip extraction only") &&
                r.diagnostics.join(" ").contains("dual brush is not applied"),
            "ABR static conversion or unsupported dual qualification lost");
    const auto restored = decode(encode(r, error), error);
    require(error.isEmpty() && restored.preset.name == r.preset.name &&
                restored.provenance["sourceDescriptorTupleV1"] ==
                    r.provenance["sourceDescriptorTupleV1"],
            "ABR typed source settings lost on internal native reopen");
  }
  auto staticOnly =
      importBrush(boundAbr(false, false, 6, false, false, false), "abr", error);
  require(staticOnly.complete &&
              staticOnly.records[0].provenance["conversionKind"].toString() ==
                  "static-native",
          "Explicitly disabled dynamics prevented static classification");
  for (const char *flag :
       {"brushProjection", "toolOptions", "brushGroup", "futureUnknownFlag"}) {
    const auto active = importBrush(
        boundAbr(false, false, 6, false, false, true, flag), "abr", error);
    require(active.complete &&
                active.records[0].provenance["conversionKind"].toString() ==
                    "partial-native" &&
                active.records[0].diagnostics.join(" ").contains(flag),
            "Unapplied ABR active/unknown field was presented as complete "
            "static conversion");
  }
  require(!importBrush(boundAbr(true), "abr", error).complete,
          "missing ABR sample binding accepted");
  require(!importBrush(boundAbr(false, true), "abr", error).complete,
          "ambiguous ABR sample binding accepted");
  require(!importBrush(boundAbr(false, false, 6, true), "abr", error).complete,
          "ABR static units silently reinterpreted");
  auto newer = importBrush(boundAbr(false, false, 7), "abr", error);
  require(newer.complete && newer.records.size() == 1 &&
              !newer.records[0].provenance.contains("sourceSampleBinding") &&
              newer.records[0].provenance["conversionKind"].toString() ==
                  "tip-only",
          "ABR6.2 format evidence incorrectly generalized to later versions");
  // Portable export retains the safe classification, not opaque source data.
  for (auto original : {bound.records.front(), newer.records.front()}) {
    original.provenance["sourceDescriptorTupleV1"] =
        "private source alias bytes";
    const auto package =
        encodePortable(original, ResourcePackaging::PrivateCopy, error);
    require(error.isEmpty(), "ABR portable classification export failed");
    const auto restored = importBrush(package, "gzbrush", error);
    require(
        error.isEmpty() && restored.complete && restored.records.size() == 1 &&
            restored.records[0].provenance["conversionKind"] ==
                original.provenance["conversionKind"] &&
            !restored.records[0].provenance.contains(
                "sourceDescriptorTupleV1") &&
            !restored.records[0].provenance["sourceReferenceVerified"].toBool(),
        "Portable ABR classification lost or private descriptor leaked");
    original.provenance["conversionKind"] = "arbitrary private classification";
    const auto unknown                    = importBrush(
        encodePortable(original, ResourcePackaging::PrivateCopy, error),
        "gzbrush", error);
    require(unknown.complete &&
                !unknown.records[0].provenance.contains("conversionKind"),
            "Portable export retained an unrecognized classification string");
  }
  for (int version : {1, 2}) {
    auto geometry = importBrush(computedAbr(version), "abr", error);
    require(geometry.complete && geometry.records.size() == 1,
            "computed ABR could not become a native preset");
    const auto &record = geometry.records.front();
    require(
        record.preset.tip.shape == Shape::Round &&
            record.preset.tip.images.empty() &&
            record.preset.size.value == 64 &&
            record.preset.aspect.value == .5 &&
            record.preset.angle.value == -30 && record.preset.spacing == .25 &&
            record.preset.tip.hardness == .75 &&
            record.provenance["conversionKind"].toString() == "static-native" &&
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
  // Original fixed-channel source metadata, with disabled and unsupported
  // variants exercising eligibility rather than merely round-tripping fields.
  for (const QString &hsvModel :
       {QString(), QString("legacy-smear"), QString("new-smear"),
        QString("legacy-dulling"), QString("new-dulling")})
    for (const QString &target : {QString("h"), QString("s"), QString("v")})
      for (int variant = 0; variant < 5; ++variant) {
        QImage image =
            QImage::fromData(hardPixelKpp("buildUp", hsvModel), "PNG");
        QString xml = image.text("preset"), extra;
        auto param  = [&](const QString &key, const QString &value) {
          extra += "<param name=\"" + key + "\">" + value.toHtmlEscaped() +
                   "</param>";
        };
        for (const QString &key : {QString("h"), QString("s"), QString("v")})
          param("Pressure" + key,
                key == target && variant != 1 ? "true" : "false");
        const QString curve = variant == 2   ? "0,0.2;1,0.8;"
                              : variant == 4 ? "0,0;1,1;"
                                             : "0,0.6;1,0.6;";
        param(target + "UseCurve", "true");
        param(target + "UseSameCurve", "true");
        param(target + "Value", "1");
        param(target + "curveMode", "0");
        param(target + "commonCurve", curve);
        param(target + "Sensor",
              "<params id=\"" +
                  QString(variant == 3 ? "rotation" : "pressure") +
                  "\"><curve>" + curve + "</curve></params>");
        xml.replace("</Preset>", extra + "</Preset>");
        image.setText("preset", xml);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        require(image.save(&buffer, "PNG"),
                "source HSV fixture encoding failed");
        const auto imported = importBrush(bytes, "kpp", error);
        require(imported.records.size() == 1, "source HSV import lost preset");
        const auto &record = imported.records.front();
        const bool candidate =
            record.preset.profile ==
            ("krita-5.3.4.1-" +
             (hsvModel.isEmpty() ? QString("hard-pixel") : hsvModel) +
             "-candidate-1")
                .toStdString();
        require(
            candidate == (variant < 2 || (hsvModel.isEmpty() && variant == 4)),
            "source HSV eligibility admitted unsupported controls");
        if (candidate) {
          const auto expected = (variant == 1 || variant == 4)
                                    ? SourceHsvChannel::Off
                                : target == "h" ? SourceHsvChannel::Hue
                                : target == "s" ? SourceHsvChannel::Saturation
                                                : SourceHsvChannel::Value;
          require(record.preset.sourceHsvChannel == expected,
                  "source HSV enabled state lost");
          const auto restored = decode(encode(record, error), error);
          require(error.isEmpty() &&
                      restored.preset.sourceHsvChannel == expected &&
                      restored.preset.sourceHsvValue ==
                          record.preset.sourceHsvValue,
                  "source HSV portable lifecycle lost versioned settings");
          const int index = target == "h" ? 0 : target == "s" ? 1 : 2;
          require(restored.preset.sourceHsvV2[index].enabled == (variant == 4),
                  "source HSV V2 portable enabled state lost");
          if (variant == 4) {
            const auto &control = restored.preset.sourceHsvV2[index].control;
            require(control.value == 1 && control.mappings.size() == 1 &&
                        control.mappings[0].sensor == Sensor::Pressure &&
                        control.mappings[0].curve.size() == 2 &&
                        control.mappings[0].curve[1].y == 1,
                    "source HSV V2 portable control lost");
            // A forged periodic pressure field used to bypass the generic
            // dynamic validator and wrap full pressure from one back to zero.
            auto periodic = restored;
            periodic.preset.sourceHsvV2[index].control.mappings[0].periodic =
                true;
            require(encode(periodic, error).isEmpty() && !error.isEmpty(),
                    "periodic source HSV pressure encoded");
            auto document =
                QJsonDocument::fromJson(encode(restored, error)).object();
            require(error.isEmpty(), "valid source HSV failed encoding");
            auto settings       = document["settings"].toObject();
            auto controls       = settings["sourceHsvControlsV2"].toArray();
            auto channel        = controls[index].toObject();
            auto dynamic        = channel["control"].toObject();
            auto mappings       = dynamic["mappings"].toArray();
            auto mapping        = mappings[0].toObject();
            mapping["periodic"] = true;
            mappings[0]         = mapping;
            dynamic["mappings"] = mappings;
            channel["control"]  = dynamic;
            controls[index]     = channel;
            settings["sourceHsvControlsV2"] = controls;
            document["settings"]            = settings;
            const auto malformed =
                decode(QJsonDocument(document).toJson(), error);
            require(!malformed.decoded && !error.isEmpty(),
                    "decoder admitted periodic source HSV pressure");
            error.clear();
            auto conflict                    = restored;
            conflict.preset.sourceHsvChannel = SourceHsvChannel::Hue;
            require(encode(conflict, error).isEmpty(),
                    "conflicting HSV versions encoded");
            conflict = restored;
            conflict.preset.sourceHsvV2[(index + 1) % 3] =
                conflict.preset.sourceHsvV2[index];
            const auto joint = decode(encode(conflict, error), error);
            require(error.isEmpty() &&
                        joint.preset.sourceHsvV2[index].enabled &&
                        joint.preset.sourceHsvV2[(index + 1) % 3].enabled,
                    "joint pressure HSV portable controls lost");
            conflict.preset.sourceHsvV2[(index + 1) % 3]
                .control.mappings[0]
                .sensor = Sensor::TiltX;
            require(encode(conflict, error).isEmpty(),
                    "unqualified joint tilt HSV encoded");
            error.clear();
          }
        }
      }
  {
    auto joint =
        importBrush(hardPixelKpp("buildUp"), "kpp", error).records.front();
    joint.preset.sourceHsvV2[1] = {true, Dynamic{.65, 0, {}}};
    joint.preset.sourceHsvV2[2] = {true, Dynamic{.85, 0, {}}};
    const auto restored         = decode(encode(joint, error), error);
    require(error.isEmpty() && restored.preset.sourceHsvV2[1].enabled &&
                restored.preset.sourceHsvV2[1].control.value == .65 &&
                restored.preset.sourceHsvV2[2].control.value == .85 &&
                restored.preset.sourceHsvChannel == SourceHsvChannel::Off,
            "joint HSV portable controls lost");
  }
  auto numericSpelling =
      importBrush(hardPixelKpp("numericSpelling"), "kpp", error);
  require(numericSpelling.complete &&
              numericSpelling.records.front().preset.profile ==
                  "krita-5.3.4.1-hard-pixel-candidate-1",
          "equivalent numeric XML spelling changed source eligibility");
  for (const QString &model :
       {QString(), QString("legacy-smear"), QString("legacy-dulling"),
        QString("new-smear")}) {
    auto texture =
        importBrush(hardPixelKpp("texture-valid", model), "kpp", error);
    require(
        error.isEmpty() && texture.records.size() == 1 &&
            texture.records.front().preset.profile != "groundzero-native-1" &&
            texture.records.front().preset.texture.image,
        "fixed grayscale source texture lost eligibility");
    const auto &record = texture.records.front();
    auto restored      = decode(encode(record, error), error);
    require(error.isEmpty() && restored.preset.sourceTextureSoftTexturing &&
                restored.preset.texture.image &&
                restored.preset.texture.image->pixels[4].a == 85 / 255.0 &&
                restored.provenance["sourceTextureSha256"] ==
                    record.provenance["sourceTextureSha256"],
            "source grayscale texture or identity lost portable round trip");
  }
  for (const QString &mutation : {"texture-invalid", "texture-checksum"}) {
    auto invalid = importBrush(hardPixelKpp(mutation), "kpp", error);
    require(!error.isEmpty() && invalid.records.empty(),
            "malformed source texture accepted");
  }
  auto textureOverBudget = importBrush(hardPixelKpp("texture-valid"), "kpp",
                                       error, nullptr, 256 * sizeof(Pixel) - 1);
  require(!error.isEmpty() && textureOverBudget.records.empty(),
          "source texture decoded beyond remaining allocation budget");
  auto unknownTextureState =
      importBrush(hardPixelKpp("texture-valid", "new-dulling"), "kpp", error);
  require(error.isEmpty() && unknownTextureState.records.size() == 1 &&
              unknownTextureState.records.front().preset.profile ==
                  "groundzero-native-1",
          "unmeasured texture state was labeled source compatible");
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
  for (const QString &variant : {"pressureRange", "pressureSensorRange"}) {
    auto affine = importBrush(hardPixelKpp(variant), "kpp", error);
    require(affine.complete && affine.records.front().preset.profile ==
                                   hardRecord.preset.profile,
            "explicit two-point source pressure curve lost eligibility");
    auto restored = decode(encode(affine.records.front(), error), error);
    require(error.isEmpty() && restored.preset.size.mappings.size() == 1 &&
                restored.preset.size.mappings.front().curve.size() == 2 &&
                restored.preset.size.mappings.front().curve[0].y == .25 &&
                restored.preset.size.mappings.front().curve[1].y == .75 &&
                !restored.provenance["sourceReferenceVerified"].toBool(),
            "affine source pressure endpoints or qualification lost on reopen");
  }
  for (const QString attributes :
       {"duration=\"1000\" periodic=\"0\"", "duration=\"1000\" periodic=\"1\"",
        "duration=\"2000\" periodic=\"0\"", "periodic=\"0\""}) {
    QImage image           = QImage::fromData(hardPixelKpp("pressure"), "PNG");
    QString xml            = image.text("preset");
    const QString original = QString("id=\"pressure\"").toHtmlEscaped();
    require(xml.count(original) == 1,
            "Time fixture has no unique serialized pressure sensor");
    xml.replace(original,
                (QString("id=\"time\" ") + attributes).toHtmlEscaped());
    image.setText("preset", xml);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
            "Cannot author original time-curve fixture");
    auto importedTime = importBrush(bytes, "kpp", error);
    require(error.isEmpty() && importedTime.complete &&
                importedTime.records.front().preset.profile ==
                    "groundzero-native-1" &&
                !importedTime.records.front()
                     .provenance["sourceReferenceVerified"]
                     .toBool(),
            "Time conversion improperly admitted a source profile");
    const auto &maps     = importedTime.records.front().preset.size.mappings;
    const bool supported = attributes.startsWith("duration=\"1000\"");
    const bool periodic  = attributes.endsWith("periodic=\"1\"");
    require(supported ? maps.size() == 1 && maps[0].sensor == Sensor::Time &&
                            maps[0].scale == 1 && maps[0].periodic == periodic
                      : maps.empty(),
            "Time conversion guessed absent or unmeasured period semantics");
    if (supported) {
      auto reopened =
          decode(encode(importedTime.records.front(), error), error);
      require(
          error.isEmpty() && reopened.preset.size.mappings.size() == 1 &&
              reopened.preset.size.mappings[0].sensor == Sensor::Time &&
              reopened.preset.size.mappings[0].scale == 1 &&
              reopened.preset.size.mappings[0].periodic == periodic,
          "Native time cycle/unit conversion did not survive preset reopen");
    }
  }
  // Unknown source curve declarations must not be qualified merely because
  // the permissive native decoder can extract an identity point list.
  for (const QString variant :
       {"pressure", "wetOpacityPressure", "wetFlowDisabled"}) {
    const QString model = variant == "pressure" ? "" : "legacy-dulling";
    for (const QString replacement :
         {"<curve interpolation=\"unknown\">0,0;1,1;</curve>",
          "<curve>0,0;1,1;</curve><curve>0,0;1,1;</curve>",
          "active-setting<curve>0,0;1,1;</curve>"}) {
      QImage source;
      require(source.loadFromData(hardPixelKpp(variant, model), "PNG"),
              "cannot load original sensor eligibility fixture");
      QString xml = source.text("preset");
      const QString original =
          QString("<curve>0,0;1,1;</curve>").toHtmlEscaped();
      require(xml.contains(original), "sensor fixture has no original curve");
      xml.replace(original, replacement.toHtmlEscaped());
      source.setText("preset", xml);
      QByteArray bytes;
      QBuffer buffer(&bytes);
      buffer.open(QIODevice::WriteOnly);
      require(source.save(&buffer, "PNG"),
              "cannot encode sensor eligibility fixture");
      const auto imported = importBrush(bytes, "kpp", error);
      require(
          imported.complete && imported.records.size() == 1 &&
              imported.records.front().preset.profile == "groundzero-native-1",
          "unknown active sensor declaration entered a source candidate");
    }
  }
  for (const QString variant :
       {"wetOpacityPressure", "wetFlowPressure", "wetFlowDisabled"}) {
    auto transfer = importBrush(hardPixelKpp(variant), "kpp", error);
    require(
        transfer.complete && transfer.records.size() == 1 &&
            transfer.records.front().preset.profile ==
                "krita-5.3.4.1-hard-pixel-candidate-1",
        "measured Pixel identity pressure transfer lost candidate eligibility");
    const auto &preset = transfer.records.front().preset;
    const auto &control =
        variant == "wetOpacityPressure" ? preset.opacity : preset.flow;
    require(control.mappings.size() == 1,
            "Pixel UseCurve was incorrectly disabled by retained Pressure "
            "metadata");
    const auto restored =
        decode(encode(transfer.records.front(), error), error);
    require(
        error.isEmpty() && restored.preset.profile == preset.profile &&
            restored.preset.opacity.mappings.size() ==
                preset.opacity.mappings.size() &&
            restored.preset.flow.mappings.size() == preset.flow.mappings.size(),
        "Pixel pressure transfer lost on portable round trip");
  }
  for (const QString target : {"Opacity", "Flow"})
    for (bool enabledFlag : {false, true})
      for (bool local : {false, true}) {
        QImage image =
            QImage::fromData(hardPixelKpp("wet" + target + "Pressure"), "PNG");
        QString xml = image.text("preset");
        if (!enabledFlag) {
          const QString flag =
              "<param name=\"Pressure" + target + "\">true</param>";
          require(xml.count(flag) == 1,
                  "missing transfer pressure flag control");
          xml.replace(flag,
                      "<param name=\"Pressure" + target + "\">false</param>");
        }
        xml.replace("0,0;1,1;", "0,0;.5,1;1,0;");
        if (local)
          xml.replace(
              "<param name=\"" + target + "UseSameCurve\">true</param>",
              "<param name=\"" + target + "UseSameCurve\">false</param>");
        image.setText("preset", xml);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        require(image.save(&buffer, "PNG"),
                "cannot encode transfer curve fixture");
        auto imported = importBrush(bytes, "kpp", error);
        require(imported.complete && imported.records.size() == 1 &&
                    imported.records.front().preset.profile ==
                        "krita-5.3.4.1-hard-pixel-candidate-1",
                "source pressure spline lost candidate eligibility");
        auto restored = decode(encode(imported.records.front(), error), error);
        const auto &control = target == "Opacity" ? restored.preset.opacity
                                                  : restored.preset.flow;
        require(
            error.isEmpty() && control.mappings.size() == 1 &&
                control.mappings.front().interpolation ==
                    Interpolation::NaturalCubic &&
                control.mappings.front().curve.size() == 3,
            "source pressure spline lost interpolation on portable round trip");
      }
  for (const QString axis : {"xtilt", "ytilt"}) {
    QImage image = QImage::fromData(hardPixelKpp("barrel"), "PNG");
    QString xml  = image.text("preset");
    xml.replace("id=&quot;rotation&quot;", "id=&quot;" + axis + "&quot;");
    image.setText("preset", xml);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    require(image.save(&buffer, "PNG"), "cannot encode tilt sensor fixture");
    auto imported = importBrush(bytes, "kpp", error);
    require(imported.complete && imported.records.size() == 1 &&
                imported.records.front().preset.profile ==
                    "krita-5.3.4.1-hard-pixel-candidate-1",
            "symmetric tilt sensor lost candidate eligibility");
    auto restored = decode(encode(imported.records.front(), error), error);
    require(error.isEmpty() && restored.preset.angle.mappings.size() == 1 &&
                restored.preset.angle.mappings.front().sensor ==
                    (axis == "xtilt" ? Sensor::TiltX : Sensor::TiltY),
            "symmetric tilt sensor lost portable identity");
  }
  auto barrel = importBrush(hardPixelKpp("barrel"), "kpp", error);
  require(barrel.complete &&
              barrel.records.front().preset.profile ==
                  "krita-5.3.4.1-hard-pixel-candidate-1" &&
              barrel.records.front().preset.angle.value == 360 &&
              barrel.records.front().preset.angle.mappings.size() == 1 &&
              barrel.records.front().preset.angle.mappings.front().sensor ==
                  Sensor::Rotation,
          "source barrel identity mapping lost eligibility or units");
  auto barrelCopy = decode(encode(barrel.records.front(), error), error);
  require(
      error.isEmpty() && barrelCopy.preset.angle.value == 360 &&
          barrelCopy.preset.angle.mappings.front().sensor == Sensor::Rotation,
      "portable source barrel mapping lost units");
  auto anisotropic =
      importBrush(hardPixelKpp("barrelAnisotropic"), "kpp", error);
  require(anisotropic.complete && anisotropic.records.front().preset.profile ==
                                      "groundzero-native-1",
          "unmeasured rotating anisotropic spacing entered source candidate");
  auto spline = importBrush(hardPixelKpp("pressureSpline"), "kpp", error);
  require(
      spline.complete &&
          spline.records.front().preset.profile ==
              "krita-5.3.4.1-hard-pixel-candidate-1" &&
          spline.records.front().preset.size.mappings.front().interpolation ==
              Interpolation::NaturalCubic &&
          !spline.records.front()
               .provenance["sourceReferenceVerified"]
               .toBool(),
      "measured multi-knot source size curve lost cubic candidate mapping");
  auto splineRestored = decode(encode(spline.records.front(), error), error);
  require(error.isEmpty() &&
              splineRestored.preset.size.mappings.front().interpolation ==
                  Interpolation::NaturalCubic,
          "source cubic interpolation lost on portable round trip");
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
  for (const QString mode : {"normal", "multiply", "screen", "overlay"}) {
    auto dry = importBrush(hardPixelKpp("blend-" + mode), "kpp", error);
    require(
        dry.complete &&
            dry.records.front().preset.profile ==
                "krita-5.3.4.1-hard-pixel-candidate-1" &&
            !dry.records.front().provenance["sourceReferenceVerified"].toBool(),
        "explicit Pixel blend did not retain unverified candidate identity");
    auto reopened = decode(encode(dry.records.front(), error), error);
    require(error.isEmpty() &&
                reopened.preset.blend == dry.records.front().preset.blend,
            "Pixel blend lost on portable round trip");
  }
  for (const QString model :
       {"legacy-smear", "new-smear", "legacy-dulling", "new-dulling"}) {
    for (const QString mutation :
         {"blend-multiply", "blend-screen", "blend-overlay", "erase"}) {
      auto wet = importBrush(hardPixelKpp(mutation, model), "kpp", error);
      require(!wet.complete && wet.records.empty() && !error.isEmpty(),
              "Pixel blend eligibility leaked into wet source profiles");
      require(error.contains("Color Smudge supports only Normal"),
              "unsupported wet blend did not explain the preservation failure");
    }
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
  auto smallAA = importBrush(hardPixelKpp("smallAA"), "kpp", error);
  require(smallAA.complete &&
              smallAA.records.front().preset.profile ==
                  "krita-5.3.4.1-default-circle-candidate-1" &&
              smallAA.records.front().preset.size.value == 4 &&
              !smallAA.records.front()
                   .provenance["sourceReferenceVerified"]
                   .toBool(),
          "small hard-AA eligibility or provenance changed");
  for (const QString mutation : {"smallAAfractional", "smallAAsoft"}) {
    auto fallback = importBrush(hardPixelKpp(mutation), "kpp", error);
    require(fallback.complete && fallback.records.front().preset.profile ==
                                     "groundzero-native-1",
            "unmeasured small-AA setting admitted");
  }
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
    auto translucent =
        importBrush(hardPixelKpp("halfOpacity", model), "kpp", error);
    require(translucent.complete &&
                translucent.records.front().preset.profile == expected &&
                translucent.records.front().preset.opacity.value == .5,
            "source wet static opacity lost value or candidate identity");
    auto opacityRestored =
        decode(encode(translucent.records.front(), error), error);
    require(error.isEmpty() && opacityRestored.preset.profile == expected &&
                opacityRestored.preset.opacity.value == .5,
            "source wet static opacity lost portable round trip");
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
    for (const QString &mutation : {"alphaOff", "lengthOff", "colorOff"}) {
      auto flags = importBrush(hardPixelKpp(mutation, model), "kpp", error);
      require(flags.complete &&
                  flags.records.front().preset.profile ==
                      "krita-5.3.4.1-" + model.toStdString() + "-candidate-1",
              "source wet switch lost strict eligibility");
      auto restored = decode(encode(flags.records.front(), error), error);
      require(
          error.isEmpty() &&
              restored.preset.sourceSmearAlpha == (mutation != "alphaOff") &&
              restored.preset.sourceColorRateEnabled ==
                  (mutation != "colorOff") &&
              restored.preset.sourceSmudgeLengthEnabled ==
                  (mutation != "lengthOff"),
          "source wet switches lost portable round trip");
    }
    for (const QString &mutation :
         {"wetOpacityPressure", "wetFlowPressure", "wetFlowDisabled"}) {
      auto controlled =
          importBrush(hardPixelKpp(mutation, model), "kpp", error);
      require(controlled.complete &&
                  controlled.records.front().preset.profile == expected,
              "measured wet transfer curve lost strict candidate eligibility");
      auto portable = decode(encode(controlled.records.front(), error), error);
      require(error.isEmpty() && portable.preset.profile == expected,
              "wet transfer curve lost portable candidate identity");
      if (mutation == "wetOpacityPressure")
        require(portable.preset.opacity.mappings.size() == 1,
                "wet opacity pressure curve was discarded");
      else
        require(portable.preset.flow.mappings.size() ==
                        (mutation == "wetFlowPressure" ? 1u : 0u) &&
                    portable.provenance["sourceInactiveFlowCurve"].toString() ==
                        "identity-pressure" &&
                    portable.provenance["sourceInactiveFlowPressureEnabled"]
                            .toBool() == (mutation == "wetFlowPressure"),
                "inactive wet flow curve/checkbox provenance was discarded");
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
  for (const QString &shape : {QString("circle"), QString("rect")}) {
    QImage image = QImage::fromData(kpp(), "PNG");
    QString xml  = image.text("preset");
    xml.replace("id=\"default\"", "id=\"gauss\"");
    xml.replace("type=\"circle\"", "type=\"" + shape + "\"");
    image.setText("preset", xml);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    require(image.save(&buffer, "PNG"), "Gaussian fixture encoding failed");
    auto converted = importBrush(bytes, "kpp", error);
    require(converted.complete && converted.records.size() == 1,
            "Gaussian native conversion failed");
    const auto &record = converted.records.front();
    require(
        record.preset.tip.shape == Shape::Gaussian &&
            record.provenance["conversionKind"] == "partial-native" &&
            record.provenance["sourceGeneratedMaskV1"].toObject()["shape"] ==
                shape &&
            record.provenance["conversionReport"].toString().contains(
                "native Gaussian approximation"),
        "Gaussian source approximation lost explicit provenance");
    auto reopened = decode(encode(record, error), error);
    require(error.isEmpty() && reopened.provenance["sourceGeneratedMaskV1"] ==
                                   record.provenance["sourceGeneratedMaskV1"],
            "Gaussian source metadata lost on portable roundtrip");
  }
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
void testPortability() {
  Record original;
  original.preset.name = "Original portable fixture";
  auto color           = std::make_shared<Image>();
  color->width         = 2;
  color->height        = 1;
  color->color         = true;
  color->pixels        = {{128.0 / 255, 0, 0, 128.0 / 255}, {0, 1, 0, 1}};
  auto mask            = std::make_shared<Image>();
  mask->width = mask->height = 1;
  mask->pixels               = {{0, 0, 0, 1}};
  original.preset.tip.shape  = Shape::Bitmap;
  original.preset.tip.images.push_back(color);
  original.preset.texture.image = mask;
  original.provenance = {{"privatePath", "/private-brush-fixture/source"},
                         {"sourceReferenceVerified", true}};
  original.diagnostics << "Original conversion has unverified source dynamics."
                       << "Source /private-brush-fixture/source";
  QString error;
  auto inventory = resources(original, error);
  require(error.isEmpty() && inventory.size() == 2,
          "portable inventory lost a resource");
  for (const auto &resource : inventory)
    require(resource.redistribution == "unknown",
            "unknown rights were inferred");
  const auto manifest =
      encodePortable(original, ResourcePackaging::ManifestOnly, error);
  auto info = inspectPortable(manifest, error);
  require(error.isEmpty() && info.portable && info.resources.size() == 2 &&
              std::none_of(info.resources.begin(), info.resources.end(),
                           [](const ResourceInfo &r) { return r.embedded; }),
          "manifest-only export embedded an image");
  auto missing = importBrush(manifest, "gzbrush", error);
  require(!error.isEmpty() && missing.records.empty() && !missing.complete,
          "unresolved manifest became an installable preset");
  original.resourceRights[inventory[0].hash] =
      QJsonObject{{"redistribution", "permitted"}, {"license", "CC0-1.0"}};
  original.resourceRights[inventory[1].hash] =
      QJsonObject{{"redistribution", "restricted"},
                  {"license", "Original test declaration"}};
  const auto permitted =
      encodePortable(original, ResourcePackaging::PermittedOnly, error);
  info = inspectPortable(permitted, error);
  require(
      error.isEmpty() &&
          std::count_if(info.resources.begin(), info.resources.end(),
                        [](const ResourceInfo &r) { return r.embedded; }) == 1,
      "permitted-only export included restricted resources");
  const auto backup =
      encodePortable(original, ResourcePackaging::PrivateCopy, error);
  auto restored = decode(backup, error);
  require(error.isEmpty() && !restored.referenceVerified &&
              restored.resourceRights == original.resourceRights &&
              !backup.contains("/private-brush-fixture/") &&
              restored.diagnostics.empty() &&
              !restored.provenance["sourceReferenceVerified"].toBool(),
          "private portability lost declarations or exported private metadata");

  for (const QString &note :
       {QString::fromLatin1(R"(\\server\share\brush.png)"),
        QString::fromLatin1(R"(source=\\?\C:\brushes\brush.png)"),
        QString::fromLatin1(R"(source: \\?\UNC\server\share\brush.png)"),
        QString::fromLatin1(R"(source=C:\brushes\brush.png)"),
        QString::fromLatin1(R"(source:C:/brushes/brush.png)"),
        QString::fromLatin1(R"(C:/brushes/brush.png)"),
        QString::fromLatin1(R"(source=/private-brush-fixture/source)"),
        QString::fromLatin1(R"(source=\rooted\brush.png)"),
        QString::fromLatin1("FILE:///private-brush-fixture/source"),
        QString::fromLatin1("~/brushes/source")}) {
    Record privateNote      = original;
    privateNote.diagnostics = QStringList{note};
    privateNote.resourceRights[inventory[0].hash] =
        QJsonObject{{"redistribution", "permitted"}, {"license", note}};
    const auto filteredBytes =
        encodePortable(privateNote, ResourcePackaging::PrivateCopy, error);
    const auto filtered = decode(filteredBytes, error);
    require(error.isEmpty() && filtered.diagnostics.empty() &&
                filtered.resourceRights[inventory[0].hash]
                    .toObject()["license"]
                    .toString()
                    .isEmpty(),
            "portable export disclosed a local path in free-form metadata");
  }
  Record licenseLink = original;
  for (const QString &attribution :
       {QString("CC0-1.0; https://creativecommons.org/publicdomain/zero/1.0/"),
        QString("https://creativecommons.org/publicdomain/zero/1.0/"),
        QString("License=https://creativecommons.org/publicdomain/zero/1.0/"),
        QString("CC BY 4.0: http://creativecommons.org/licenses/by/4.0/")}) {
    licenseLink.resourceRights[inventory[0].hash] =
        QJsonObject{{"redistribution", "permitted"}, {"license", attribution}};
    const auto linked = decode(
        encodePortable(licenseLink, ResourcePackaging::PrivateCopy, error),
        error);
    require(error.isEmpty() && linked.resourceRights[inventory[0].hash]
                                       .toObject()["license"]
                                       .toString() == attribution,
            "portable export removed ordinary license attribution or web link");
  }
  auto native = decode(encode(restored, error), error);
  require(error.isEmpty() && native.resourceRights == restored.resourceRights,
          "native save/reopen lost resource declarations");
  QTemporaryDir worker;
  auto wire        = encodeWorkerRecord(restored, worker.path(), error);
  auto transferred = decodeWorkerRecord(wire, worker.path(), error);
  require(
      error.isEmpty() && transferred.resourceRights == restored.resourceRights,
      "worker round trip lost resource declarations");
  Record wrong;
  auto wrongImage         = std::make_shared<Image>(*mask);
  wrongImage->pixels[0].a = .5;
  wrong.preset.tip.shape  = Shape::Bitmap;
  wrong.preset.tip.images = {wrongImage};
  QStringList matched;
  auto unchanged = relinkPortable(manifest, {wrong}, matched, error);
  require(error.isEmpty() && matched.isEmpty(),
          "wrong resource hash was accepted");
  decode(unchanged, error);
  require(!error.isEmpty(), "unsuccessful repair produced a usable brush");
  // An omitted PNG may have a different byte encoding on another platform.
  // Relink by canonical pixels and rewrite the native byte-hash references.
  auto root            = QJsonDocument::fromJson(manifest).object();
  auto package         = root["package"].toObject();
  auto settings        = package["settings"].toObject();
  auto tip             = settings["tip"].toObject();
  auto refs            = tip["images"].toArray();
  const QString oldId  = refs[0].toString(), alternateId(64, 'a');
  refs[0]              = alternateId;
  tip["images"]        = refs;
  settings["tip"]      = tip;
  package["settings"]  = settings;
  root["package"]      = package;
  auto entries         = root["resources"].toObject();
  entries[alternateId] = entries.take(oldId);
  root["resources"]    = entries;
  auto repaired =
      relinkPortable(QJsonDocument(root).toJson(), {original}, matched, error);
  require(error.isEmpty() && matched.size() == 2,
          "matching copies did not repair all hashes");
  restored = decode(repaired, error);
  require(error.isEmpty() && restored.preset.tip.images.size() == 1 &&
              std::abs(restored.preset.tip.images[0]->pixels[0].a -
                       128.0 / 255) < 1e-9 &&
              resources(restored, error).size() == 2,
          "relinked color or texture resource changed");
  // Declarations come from the manifest, not a relink donor's permissions.
  for (auto rights : restored.resourceRights)
    require(rights.toObject()["redistribution"] == "unknown",
            "relink donor silently granted redistribution rights");
  auto damaged             = QJsonDocument::fromJson(backup).object();
  auto damagedPackage      = damaged["package"].toObject();
  auto assets              = damagedPackage["assets"].toObject();
  const auto damagedId     = assets.keys().front();
  auto asset               = assets[damagedId].toObject();
  asset["png"]             = "broken embedded bytes";
  assets[damagedId]        = asset;
  damagedPackage["assets"] = assets;
  damaged["package"]       = damagedPackage;
  info = inspectPortable(QJsonDocument(damaged).toJson(), error);
  require(
      error.isEmpty() &&
          std::count_if(info.resources.begin(), info.resources.end(),
                        [](const ResourceInfo &r) { return !r.embedded; }) == 1,
      "corrupted embedded resource was not repairable");
  repaired = relinkPortable(QJsonDocument(damaged).toJson(), {original},
                            matched, error);
  restored = decode(repaired, error);
  require(error.isEmpty() && matched.size() == 1,
          "corrupted resource repair failed");
  damaged              = QJsonDocument::fromJson(backup).object();
  entries              = damaged["resources"].toObject();
  const auto id        = entries.keys().front();
  auto entry           = entries[id].toObject();
  entry["hash"]        = QString(64, '0');
  entries[id]          = entry;
  damaged["resources"] = entries;
  decode(QJsonDocument(damaged).toJson(), error);
  require(!error.isEmpty(), "forged pixel hash accepted");
  const auto retryManifest =
      preparePortableRepair(QJsonDocument(damaged).toJson(), error);
  info = inspectPortable(retryManifest, error);
  require(error.isEmpty() &&
              std::none_of(info.resources.begin(), info.resources.end(),
                           [](const ResourceInfo &r) { return r.embedded; }),
          "worker validation failure could not prepare hash repair");
  const auto badRepair =
      relinkPortable(retryManifest, {original}, matched, error);
  decode(badRepair, error);
  require(!error.isEmpty(),
          "repair replaced a forged hash by a different identity");
  // A valid replacement PNG has a correct byte ID but disagrees with the
  // original canonical pixel identity. Explicit repair restores originals.
  Record changedPixels            = original;
  auto replacement                = std::make_shared<Image>(*color);
  replacement->pixels[0]          = {0, 0, 1, 1};
  changedPixels.preset.tip.images = {replacement};
  const auto donorPackage =
      QJsonDocument::fromJson(encode(changedPixels, error)).object();
  const auto donorId = donorPackage["settings"]
                           .toObject()["tip"]
                           .toObject()["images"]
                           .toArray()[0]
                           .toString();
  auto substituted       = QJsonDocument::fromJson(backup).object();
  auto subPackage        = substituted["package"].toObject();
  auto subSettings       = subPackage["settings"].toObject();
  auto subTip            = subSettings["tip"].toObject();
  auto subRefs           = subTip["images"].toArray();
  const auto originalId  = subRefs[0].toString();
  subRefs[0]             = donorId;
  subTip["images"]       = subRefs;
  subSettings["tip"]     = subTip;
  subPackage["settings"] = subSettings;
  auto subAssets         = subPackage["assets"].toObject();
  subAssets.remove(originalId);
  subAssets[donorId]          = donorPackage["assets"].toObject()[donorId];
  subPackage["assets"]        = subAssets;
  substituted["package"]      = subPackage;
  auto subManifest            = substituted["resources"].toObject();
  subManifest[donorId]        = subManifest.take(originalId);
  substituted["resources"]    = subManifest;
  const auto substitutedBytes = QJsonDocument(substituted).toJson();
  info                        = inspectPortable(substitutedBytes, error);
  require(error.isEmpty() &&
              std::all_of(info.resources.begin(), info.resources.end(),
                          [](const ResourceInfo &r) { return r.embedded; }),
          "valid PNG byte identity was treated as missing");
  decode(substitutedBytes, error);
  require(!error.isEmpty(), "pixel hash mismatch escaped worker validation");
  const auto cleanRetry = preparePortableRepair(substitutedBytes, error);
  auto cleanRepair = relinkPortable(cleanRetry, {original}, matched, error);
  auto cleanRecord = decode(cleanRepair, error);
  require(error.isEmpty() && matched.size() == 2 &&
              cleanRecord.preset.tip.images[0]->pixels[0].r > 0,
          "explicit replacement of invalid embedded pixels failed hash repair");
  entry["width"]       = 100000;
  entries[id]          = entry;
  damaged["resources"] = entries;
  inspectPortable(QJsonDocument(damaged).toJson(), error);
  require(!error.isEmpty(), "oversized portable geometry accepted");
  Record transparent            = original;
  auto faint                    = std::make_shared<Image>(*color);
  faint->pixels[0]              = {.001, 0, 0, .001};
  transparent.preset.tip.images = {faint};
  const auto faintBytes =
      encodePortable(transparent, ResourcePackaging::PrivateCopy, error);
  const auto faintRestored = decode(faintBytes, error);
  require(
      error.isEmpty() && faintRestored.preset.tip.images[0]->pixels[0].a == 0,
      "RGBA8 transparent quantization changed portable identity");
  QTemporaryDir destination;
  require(savePortable(destination.filePath("manifest.gzbrush"), original,
                       ResourcePackaging::ManifestOnly, error),
          qPrintable(error));
  auto file = load(destination.filePath("manifest.gzbrush"), error);
  require(!error.isEmpty() && !file.semanticallySupported,
          "saved unresolved manifest became a drawing preset");
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
  r.preset.scatterBothAxes              = false;
  r.preset.secondary.count              = 3;
  r.preset.secondary.independentSpacing = true;
  r.preset.secondary.spacing            = .37;
  r.preset.secondary.flipX = r.preset.secondary.flipY = true;
  r.preset.saturation.value                           = .7;
  r.preset.brightness.value                           = 1.2;
  r.preset.seed              = UINT64_C(18446744073709551615);
  r.preset.varySeedPerStroke = true;
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
              restored.preset.secondary.independentSpacing &&
              restored.preset.secondary.spacing == .37 &&
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
  for (const auto &key :
       {"count", "flipX", "flipY", "independentSpacing", "spacing"})
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
              !oldRecord.preset.secondary.independentSpacing &&
              oldRecord.preset.secondary.spacing == .1 &&
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
  require(restored.preset.varySeedPerStroke, "per-stroke seed policy lost");
  auto legacySeedRoot     = QJsonDocument::fromJson(bytes).object();
  auto legacySeedSettings = legacySeedRoot["settings"].toObject();
  legacySeedSettings.remove("varySeedPerStroke");
  legacySeedRoot["settings"] = legacySeedSettings;
  auto legacySeed = decode(QJsonDocument(legacySeedRoot).toJson(), error);
  require(error.isEmpty() && !legacySeed.preset.varySeedPerStroke,
          "old package changed its fixed seed behavior");
  Preset sourceSeed;
  sourceSeed.profile           = "krita-5.3.4.1-hard-pixel-candidate-1";
  sourceSeed.varySeedPerStroke = true;
  require(validate(sourceSeed).find("seed variation") != std::string::npos,
          "source profile accepted unsupported host seed variation");

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
void testLongCollectionPaths();
void testCollection() {
  testLongCollectionPaths();
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
// Exercise long names without shortening the library or preset identity.
// Qt writes/renames already use extended paths on Windows; our raw no-follow
// reads must accept the same files while retaining their security contract.
void testLongCollectionPaths() {
  QTemporaryDir temporary;
  require(temporary.isValid(), "cannot create long collection fixture");
  QString directory = temporary.path();
  while (directory.size() < 280)
    directory = QDir(directory).filePath(QString(48, 'l'));
  Record record;
  record.preset.name = std::string(40, 'p');
  QString error;
  const auto paths = installCollection(directory, {record}, error);
  require(error.isEmpty() && paths.size() == 1, qPrintable(error));
  require(paths.front().size() > 400, "long collection fixture is too short");
  const auto restored = load(paths.front(), error);
  require(error.isEmpty() && restored.preset.name == record.preset.name,
          qPrintable("long collection load: " + error));
  const auto duplicate = installCollection(directory, {record}, error);
  require(error.isEmpty() && duplicate.size() == 1 &&
              duplicate.front() == paths.front(),
          qPrintable("long collection reimport: " + error));
  {
    Detail::RegularResponseDirectory anchored;
    require(anchored.open(QFileInfo(paths.front()).absolutePath(), error),
            qPrintable(error));
    Detail::readResponseFile(anchored, "../escape", 1024, error);
    require(!error.isEmpty(), "long-path reader accepted traversal");
    Detail::readResponseFile(anchored, QFileInfo(paths.front()).fileName(), 1,
                             error);
    require(!error.isEmpty(), "long-path reader ignored byte budget");
#ifdef _WIN32
    const QString native =
        QStringLiteral("\\\\?\\") +
        QDir::toNativeSeparators(QFileInfo(paths.front()).absolutePath());
    HANDLE deletion = CreateFileW(
        reinterpret_cast<LPCWSTR>(native.utf16()), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const DWORD code = GetLastError();
    if (deletion != INVALID_HANDLE_VALUE) CloseHandle(deletion);
    require(deletion == INVALID_HANDLE_VALUE && code == ERROR_SHARING_VIOLATION,
            "long directory anchor did not prevent replacement");
#endif
  }
#ifdef _WIN32
  for (const QString &unsupported :
       {QString("//server/share"), QString("//?/") + directory,
        QString("//./") + directory,
        QDir::toNativeSeparators(QString("//?/") + directory)}) {
    Detail::RegularResponseDirectory rejected;
    const bool accepted = rejected.open(unsupported, error);
    require(!accepted && error.contains("local disk path"),
            qPrintable(QString("Unsupported raw directory input [%1]: "
                               "accepted=%2 error=%3")
                           .arg(unsupported)
                           .arg(accepted)
                           .arg(error)));
    readBoundedRegularFile(unsupported + "/probe", 1024, error);
    require(error.contains("local disk path"),
            qPrintable(QString("Unsupported raw file input [%1]: error=%2")
                           .arg(unsupported)
                           .arg(error)));
  }
  const QString alias = QDir(directory).filePath("hardlink.gzbrush");
  const QString nativeAlias =
      QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(alias);
  const QString nativeTarget =
      QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(paths.front());
  require(
      CreateHardLinkW(reinterpret_cast<LPCWSTR>(nativeAlias.utf16()),
                      reinterpret_cast<LPCWSTR>(nativeTarget.utf16()), nullptr),
      "cannot create long-path hardlink fixture");
  readBoundedRegularFile(paths.front(), 1024 * 1024, error);
  require(!error.isEmpty(), "long-path reader accepted a hardlink");
  require(QFile::remove(alias), "cannot remove long-path hardlink fixture");
#endif
  QFile damaged(paths.front());
  require(damaged.open(QIODevice::WriteOnly) && damaged.write("damaged") == 7,
          "cannot damage long collection fixture");
  damaged.close();
  require(
      installCollection(directory, {record}, error).empty() && !error.isEmpty(),
      "long collection reimport overwrote conflicting content");
}
void testImportedStatusClaims() {
  Record claimed;
  claimed.referenceVerified = true;
  claimed.provenance        = {{"sourceReferenceVerified", true},
                               {"sourceSemanticallySupported", true},
                               {"conversionKind", "partial-native"}};
  QString error;
  const auto bytes = encode(claimed, error);
  require(error.isEmpty(), qPrintable(error));
  auto check = [&](const Record &record) {
    require(
        error.isEmpty() && record.decoded && record.semanticallySupported &&
            !record.referenceVerified &&
            record.provenance["sourceReferenceVerified"] == QJsonValue(false) &&
            record.provenance["sourceSemanticallySupported"] ==
                QJsonValue(false),
        "untrusted source claims became verified state or erased native "
        "support");
  };
  check(decode(bytes, error));
  QTemporaryDir directory;
  require(directory.isValid(), "cannot create status wire fixture");
  const auto wire = encodeWorkerRecord(claimed, directory.path(), error);
  require(error.isEmpty(), qPrintable(error));
  check(decodeWorkerRecord(wire, directory.path(), error));
  const auto portable =
      encodePortable(claimed, ResourcePackaging::ManifestOnly, error);
  require(error.isEmpty(), qPrintable(error));
  const auto reopened = decode(portable, error);
  check(reopened);
  require(reopened.provenance["conversionKind"] == "partial-native",
          "portable roundtrip discarded partial native classification");
  for (const QString &forged : {"native-reinterpretation", "tip-only",
                                "static-native", "partial-native"}) {
    Record source;
    source.preset.profile = "krita-5.3.4.1-hard-pixel-candidate-1";
    source.provenance["conversionKind"] = forged;
    source.provenance["sourceProfileBeforeNativeConversion"] =
        "krita-5.3.4.1-legacy-dulling-candidate-1";
    const auto loaded = decode(encode(source, error), error);
    require(
        error.isEmpty() && loaded.preset.profile == source.preset.profile,
        "forged native metadata changed the decoded source runtime profile");
    const auto exported =
        encodePortable(loaded, ResourcePackaging::ManifestOnly, error);
    const auto imported = importBrush(exported, "gzbrush", error);
    require(
        error.isEmpty() && imported.complete && imported.records.size() == 1 &&
            imported.records.front().preset.profile == source.preset.profile &&
            imported.records.front().provenance["conversionKind"] ==
                "source-candidate" &&
            !imported.records.front().provenance.contains(
                "sourceProfileBeforeNativeConversion"),
        "source runtime profile retained forged native-conversion export "
        "classification");
  }
  claimed.provenance["conversionKind"] = "source-candidate";
  const auto native                    = decode(
      encodePortable(claimed, ResourcePackaging::ManifestOnly, error), error);
  require(
      error.isEmpty() && native.preset.profile == "groundzero-native-1" &&
          !native.provenance.contains("conversionKind"),
      "native runtime retained an incompatible source-profile classification");
}
void runLibraryFileCase(const QString &kind, const QString &operation) {
  QTemporaryDir directory;
  require(directory.isValid(), "cannot create library reader fixture");
  Record record;
  QString error, path;
  if (operation == "collection") {
    const auto installed = installCollection(directory.path(), {record}, error);
    require(error.isEmpty() && installed.size() == 1, qPrintable(error));
    path = installed.front();
  } else {
    path = install(directory.path(), record, error);
    require(error.isEmpty() && !path.isEmpty(), qPrintable(error));
  }
  const auto original  = encode(record, error);
  const QString target = directory.filePath("regular-target");
  require(QFile::rename(path, target), "cannot move library fixture");
  bool made = false;
#ifndef _WIN32
  if (kind == "fifo")
    made = mkfifo(QFile::encodeName(path).constData(), 0600) == 0;
  if (kind == "symlink") made = QFile::link(target, path);
  if (kind == "hardlink")
    made = link(QFile::encodeName(target).constData(),
                QFile::encodeName(path).constData()) == 0;
#else
  if (kind == "hardlink") {
    const auto nativePath   = QDir::toNativeSeparators(path);
    const auto nativeTarget = QDir::toNativeSeparators(target);
    made = CreateHardLinkW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                           reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
                           nullptr);
  }
#endif
  require(made, "cannot create hostile library fixture");
  if (operation == "load") {
    const auto loaded = load(path, error);
    require(!error.isEmpty() && !loaded.decoded, "special library file loaded");
  } else if (operation == "install") {
    require(
        install(directory.path(), record, error).isEmpty() && !error.isEmpty(),
        "special library file reused on installation");
  } else {
    require(installCollection(directory.path(), {record}, error).empty() &&
                !error.isEmpty(),
            "special collection member reused");
  }
  // Test-owned regular target stays unchanged; use QFile here because the
  // deliberate hardlink fixture is correctly rejected by the public reader.
  QFile unchanged(target);
  require(
      unchanged.open(QIODevice::ReadOnly) && unchanged.readAll() == original,
      "rejected library member modified its target");
}
void testLibraryFileReaders() {
  QStringList kinds{"hardlink"};
#ifndef _WIN32
  kinds << "fifo" << "symlink";
#endif
  for (const auto &kind : kinds)
    for (const QString &operation : {"load", "install", "collection"}) {
      QProcess child;
      child.start(QCoreApplication::applicationFilePath(),
                  {"--test-library-file", kind, operation});
      const bool finished = child.waitForFinished(10000);
      if (!finished) {
        child.kill();
        child.waitForFinished(3000);
      }
      const auto message =
          QString("Library %1 %2: finished=%3 exit=%4 stderr=%5")
              .arg(operation)
              .arg(kind)
              .arg(finished)
              .arg(child.exitCode())
              .arg(QString::fromUtf8(child.readAllStandardError()));
      require(finished && child.exitStatus() == QProcess::NormalExit &&
                  child.exitCode() == 0,
              qPrintable(message));
    }
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
  const QString path =
      QDir(QCoreApplication::applicationDirPath())
          .filePath(
#ifdef _WIN32
              "brush-worker-test-runtime/rasterbrushworker_test.exe"
#else
              "rasterbrushworker_test"
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
  QFile placeholder(directory.filePath("oversized-worker.fixture"));
  require(placeholder.open(QIODevice::WriteOnly),
          "cannot stage budget fixture");
  placeholder.close();
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
int badResponseFixture(const QString &input, const QString &directory) {
  const QStringList parts = QFileInfo(input).baseName().split('-');
  if (parts.size() != 4) return 72;
  const QString kind = parts[2], member = parts[3];
  QString error;
  Record record;
  auto image   = std::make_shared<Image>();
  image->width = image->height = 1;
  image->pixels                = {{0, 0, 0, 1}};
  record.preset.tip.shape      = Shape::Bitmap;
  record.preset.tip.images.push_back(image);
  const auto metadata = encodeWorkerRecord(record, directory, error);
  if (!error.isEmpty()) return 72;
  const auto root         = QJsonDocument::fromJson(metadata).object();
  const auto assets       = root["assets"].toObject();
  const QString imageName = assets.keys().front() + ".rgba8";
  auto write              = [&](const QString &name, const QByteArray &bytes) {
    QFile file(QDir(directory).filePath(name));
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
  };
  const auto manifest =
      QJsonDocument(QJsonObject{{"protocol", 2},
                                {"count", 1},
                                {"diagnostics", QJsonArray{}}})
          .toJson();
  if (!write("result.json", manifest) || !write("0.gzbrush", metadata) ||
      !write("error.txt", "original test diagnostic"))
    return 72;
  const QString name   = member == "error"     ? "error.txt"
                         : member == "result"  ? "result.json"
                         : member == "package" ? "0.gzbrush"
                                               : imageName;
  const QString path   = QDir(directory).filePath(name);
  const QString target = QDir(directory).filePath("regular-target");
  if (!QFile::copy(path, target) || !QFile::remove(path)) return 72;
  bool made = false;
#ifndef _WIN32
  if (kind == "fifo")
    made = mkfifo(QFile::encodeName(path).constData(), 0600) == 0;
  if (kind == "symlink") made = QFile::link(target, path);
  if (kind == "hardlink")
    made = link(QFile::encodeName(target).constData(),
                QFile::encodeName(path).constData()) == 0;
#else
  if (kind == "hardlink") {
    const QString nativePath   = QDir::toNativeSeparators(path);
    const QString nativeTarget = QDir::toNativeSeparators(target);
    made = CreateHardLinkW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                           reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
                           nullptr);
  }
#endif
  if (!made) return 72;
  return member == "error" ? 71 : 0;
}
void runResponseCase(const QString &kind, const QString &member) {
  QTemporaryDir directory;
  require(directory.isValid(), "cannot create hostile response input");
  const QString input =
      directory.filePath("bad-response-" + kind + "-" + member + ".fixture");
  QFile placeholder(input);
  require(placeholder.open(QIODevice::WriteOnly) &&
              placeholder.write("fixture") == 7,
          "cannot write hostile response placeholder");
  placeholder.close();
  QTemporaryDir output;
  require(output.isValid(), "cannot create hostile response output");
  QString error;
  Detail::RegularResponseDirectory anchored;
  require(anchored.open(output.path(), error), qPrintable(error));
  QProcess writer;
  // The macOS production test helper is sandboxed at exec. Use the ordinary
  // test executable so this fixture can actually create hostile output in the
  // parent directory; sandbox rejection would not exercise the response reader.
  writer.start(QCoreApplication::applicationFilePath(),
               {"--test-write-response", input, output.path()});
  const bool wrote = writer.waitForFinished(5000);
  if (!wrote) {
    writer.kill();
    writer.waitForFinished(3000);
  }
  require(wrote && writer.exitStatus() == QProcess::NormalExit &&
              writer.exitCode() == (member == "error" ? 71 : 0),
          "hostile writer did not create the response fixture");
  const auto result = readWorkerResponse(anchored, true, writer.exitCode(),
                                         error, 256u * 1024u * 1024u);
  require(!error.isEmpty() && result.records.empty() && !result.complete,
          "hostile worker response accepted");
  if (member == "error")
    require(error.contains("exit 71"),
            "hostile diagnostic fixture did not execute");
  else
    require(!error.contains("worker failed"),
            "hostile result fixture did not execute");
}
void testResponseFiles() {
  // Isolate each attack in a timed outer process: a regression must fail this
  // check, not hang the suite while QFile waits for a nonexistent FIFO writer.
  QStringList kinds{"hardlink"};
#ifndef _WIN32
  kinds << "fifo" << "symlink";
#endif
  for (const auto &kind : kinds)
    for (const QString &member : {"error", "result", "package", "image"}) {
      QProcess parent;
      parent.start(QCoreApplication::applicationFilePath(),
                   {"--test-response-case", kind, member});
      const bool finished = parent.waitForFinished(10000);
      if (!finished) {
        parent.kill();
        parent.waitForFinished(3000);
      }
      const QString message =
          QString("Hostile %1 %2 response: finished=%3 exit=%4 stderr=%5")
              .arg(kind)
              .arg(member)
              .arg(finished)
              .arg(parent.exitCode())
              .arg(QString::fromUtf8(parent.readAllStandardError()));
      require(finished && parent.exitStatus() == QProcess::NormalExit &&
                  parent.exitCode() == 0,
              qPrintable(message));
    }
  QTemporaryDir root;
  require(root.isValid(), "cannot create directory-anchor fixture");
  const QString path = root.filePath("responses");
  require(QDir().mkdir(path), "cannot create anchored directory");
  QFile file(QDir(path).filePath("probe"));
  require(file.open(QIODevice::WriteOnly) && file.write("held") == 4,
          "cannot create anchored file");
  file.close();
#ifdef _WIN32
  auto deletionHandle = [](const QString &directory) {
    const auto native = QDir::toNativeSeparators(directory);
    return CreateFileW(
        reinterpret_cast<LPCWSTR>(native.utf16()), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  };
  // Establish that the fixture's ACL allows rename/delete before anchoring.
  // Afterwards the refusal must specifically come from sharing enforcement.
  for (const auto &directory : {path, root.path()}) {
    HANDLE handle = deletionHandle(directory);
    require(handle != INVALID_HANDLE_VALUE,
            "directory fixture lacks delete access");
    CloseHandle(handle);
  }
#endif
  QString error;
  Detail::RegularResponseDirectory anchored;
  require(anchored.open(path, error), qPrintable(error));
#ifndef _WIN32
  require(QDir().rename(path, root.filePath("moved")) && QDir().mkdir(path),
          "cannot swap response directory fixture");
  QFile replacement(QDir(path).filePath("probe"));
  require(
      replacement.open(QIODevice::WriteOnly) && replacement.write("evil") == 4,
      "cannot write swapped response fixture");
  replacement.close();
#else
  for (const auto &directory : {path, root.path()}) {
    HANDLE handle    = deletionHandle(directory);
    const DWORD code = GetLastError();
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    const auto message =
        QString("Windows directory anchor allowed delete access: error=%1")
            .arg(code);
    require(handle == INVALID_HANDLE_VALUE && code == ERROR_SHARING_VIOLATION,
            qPrintable(message));
  }
  require(!QDir().rename(path, root.filePath("moved")),
          "anchored Windows response directory could be replaced");
#endif
  require(Detail::readResponseFile(anchored, "probe", 4, error) == "held" &&
              error.isEmpty(),
          "response directory swap redirected file read");
  Detail::readResponseFile(anchored, "probe", 3, error);
  require(!error.isEmpty(), "regular-file size limit ignored");
  QTemporaryDir malformed;
  require(malformed.isValid(), "cannot create outer manifest fixture");
  Detail::RegularResponseDirectory manifests;
  require(manifests.open(malformed.path(), error), qPrintable(error));
  for (int mutation = 0; mutation < 5; ++mutation) {
    QJsonObject object{
        {"protocol", 2}, {"count", 1}, {"diagnostics", QJsonArray{}}};
    if (mutation == 0) object["protocol"] = 2.5;
    if (mutation == 1) object["count"] = 1.5;
    if (mutation == 2) object["unknown"] = true;
    if (mutation == 3) object["diagnostics"] = "not an array";
    if (mutation == 4) {
      QJsonArray items;
      for (int i = 0; i < 1025; ++i) items.append("entry");
      object["diagnostics"] = items;
    }
    QFile file(malformed.filePath("result.json"));
    const auto bytes = QJsonDocument(object).toJson();
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                file.write(bytes) == bytes.size(),
            "cannot write outer manifest fixture");
    file.close();
    const auto rejected = readWorkerResponse(manifests, true, 0, error, 1024);
    require(!rejected.complete && rejected.records.empty() &&
                error.contains("protocol"),
            "malformed outer manifest reached resource decoding");
  }
  std::atomic_bool cancelled(true);
  const auto stopped =
      readWorkerResponse(manifests, true, 0, error, 1024, &cancelled);
  require(!stopped.complete && stopped.records.empty() && error.isEmpty(),
          "cancelled response began parsing files");
}
int blockingCancellationFixture(const QString &directory) {
  QFile marker;
#ifdef __APPLE__
  // The App Sandbox helper cannot write the parent's temporary directory.
  // Its inherited stdout is the parent's already-open wire file instead.
  if (!marker.open(STDOUT_FILENO, QIODevice::WriteOnly)) return 73;
#else
  marker.setFileName(QDir(directory).filePath("cancel-entered"));
  if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 73;
#endif
  const auto entered =
      QByteArray::number(QCoreApplication::applicationPid()) + "\n";
  if (marker.write(entered) != entered.size() || !marker.flush()) return 73;
  // This is a deliberately blocked decoder, not the test's success signal.
  // Only observing its PID marker and subsequent OS exit can pass the test.
  QThread::sleep(30);
  return 74;
}
#ifdef RASTERBRUSH_SANDBOX_TESTING
void testImportCancellationPhases(const QString &input) {
  struct ResetObserver {
    ~ResetObserver() { setImportCancellationObserver({}); }
  } reset;
  QString error;
  for (bool activeChild : {false, true}) {
    std::atomic_bool cancelled(false);
    bool stagingObserved = false, childEntered = false, launched = false;
    qint64 enteredPid = 0;
#ifdef _WIN32
    HANDLE childHandle = nullptr;
#endif
    QElapsedTimer timer;
    timer.start();
    qint64 cancellationTime = -1;
    setImportCancellationObserver(
        [&](const char *phase, const QString &markerPath, qint64 processId) {
          const QByteArray event(phase);
          if (!activeChild && event ==
#ifdef _WIN32
                                  "staging"
#else
                                  "prepared"
#endif
          ) {
            stagingObserved  = true;
            cancellationTime = timer.elapsed();
            cancelled.store(true);
          }
          if (event != "running") return;
          launched = true;
          // Give preparation its own budget. Do not infer launch from elapsed
          // time.
          QFile marker(markerPath);
          if (!childEntered && marker.open(QIODevice::ReadOnly)) {
            const auto bytes = marker.read(64);
            bool ok          = false;
            const auto pid   = bytes.trimmed().toLongLong(&ok);
            if (ok && bytes.endsWith('\n') && pid > 0 && pid == processId) {
#ifdef _WIN32
              childHandle  = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
              childEntered = childHandle != nullptr;
#else
              childEntered = kill(pid_t(pid), 0) == 0;
#endif
              enteredPid = pid;
            }
          }
          if (childEntered || timer.elapsed() > 15000) {
            cancellationTime = timer.elapsed();
            cancelled.store(true);
          }
        });
    const auto result =
        importBrushProcess(workerExecutable(), input, error, &cancelled);
    setImportCancellationObserver({});
    bool childExited = false;
#ifdef _WIN32
    if (childHandle) {
      childExited = WaitForSingleObject(childHandle, 0) == WAIT_OBJECT_0;
      CloseHandle(childHandle);
    }
#else
    if (enteredPid > 0)
      childExited = kill(pid_t(enteredPid), 0) < 0 && errno == ESRCH;
#endif
    require(error.isEmpty() && result.records.empty() && !result.complete &&
                cancellationTime >= 0 &&
                timer.elapsed() - cancellationTime < 5000,
            "cancelled import published data or failed to return promptly");
    if (activeChild)
      require(launched && childEntered && childExited,
              "active cancellation did not observe and reap its entered child");
    else
      require(stagingObserved && !launched,
              "prelaunch cancellation started a child or missed staging");
  }
}
#endif
void testWorkerProcess() {
#if defined(_WIN32) && defined(RASTERBRUSH_SANDBOX_TESTING)
  {
    QTemporaryDir privateFiles, firstOutput, secondOutput;
    QFile input(privateFiles.filePath("overlap"));
    QFile forbidden(privateFiles.filePath("private"));
    require(input.open(QIODevice::WriteOnly) && input.write("allowed") == 7 &&
                forbidden.open(QIODevice::WriteOnly) &&
                forbidden.write("private") == 7,
            "overlapping import fixtures failed");
    input.close();
    forbidden.close();
    QProcess first, second;
    first.start(workerExecutable(),
                {"--test-windows-sandbox-parent", input.fileName(),
                 firstOutput.path(), forbidden.fileName()});
    require(first.waitForStarted(5000),
            QString("Cannot start overlapping probe parent: %1")
                .arg(first.errorString())
                .toUtf8()
                .constData());
    QElapsedTimer waiting;
    waiting.start();
    while (!QFileInfo::exists(firstOutput.filePath("entered")) &&
           first.state() != QProcess::NotRunning && waiting.elapsed() < 15000)
      first.waitForFinished(10);
    const QString firstDetail =
        QString(
            "First overlapping sandbox did not enter: state=%1 exit=%2 "
            "status=%3 stderr=%4")
            .arg(int(first.state()))
            .arg(first.exitCode())
            .arg(int(first.exitStatus()))
            .arg(QString::fromUtf8(first.readAllStandardError()).left(4096));
    require(QFileInfo::exists(firstOutput.filePath("entered")),
            firstDetail.toUtf8().constData());
    second.start(workerExecutable(),
                 {"--test-windows-sandbox-parent", input.fileName(),
                  secondOutput.path(), forbidden.fileName()});
    // waitForFinished returns false for an already exited QProcess. Either
    // child can finish while the other is starting; reap both independently.
    const bool firstFinished =
        first.state() == QProcess::NotRunning || first.waitForFinished(15000);
    const bool secondFinished =
        second.state() == QProcess::NotRunning || second.waitForFinished(15000);
    const QString completionDetail =
        QString(
            "Overlapping imports failed: first finished=%1 exit=%2 "
            "status=%3 stderr=%4; second finished=%5 exit=%6 "
            "status=%7 stderr=%8")
            .arg(firstFinished)
            .arg(first.exitCode())
            .arg(int(first.exitStatus()))
            .arg(QString::fromUtf8(first.readAllStandardError()).left(4096))
            .arg(secondFinished)
            .arg(second.exitCode())
            .arg(int(second.exitStatus()))
            .arg(QString::fromUtf8(second.readAllStandardError()).left(4096));
    require(firstFinished && secondFinished &&
                first.exitStatus() == QProcess::NormalExit &&
                first.exitCode() == 0 &&
                second.exitStatus() == QProcess::NormalExit &&
                second.exitCode() == 0,
            completionDetail.toUtf8().constData());
    QFile firstTime(firstOutput.filePath("entered"));
    QFile secondTime(secondOutput.filePath("entered"));
    require(firstTime.open(QIODevice::ReadOnly) &&
                secondTime.open(QIODevice::ReadOnly),
            "Concurrent private-runtime imports did not both enter their "
            "sandboxes");
  }
#endif
#if defined(__APPLE__) && defined(RASTERBRUSH_SANDBOX_TESTING)
  require(probeMacWorkerTransport() == 0,
          "macOS bounded wire validation failed");
#endif
#if defined(_WIN32) && defined(RASTERBRUSH_SANDBOX_TESTING)
  {
    QTemporaryDir privateFiles, output;
    require(privateFiles.isValid() && output.isValid(),
            "sandbox staging failed");
    QFile input(privateFiles.filePath("input"));
    QFile forbidden(privateFiles.filePath("private"));
    require(input.open(QIODevice::WriteOnly) && input.write("allowed") == 7 &&
                forbidden.open(QIODevice::WriteOnly) &&
                forbidden.write("private") == 7,
            "sandbox fixtures failed");
    input.close();
    forbidden.close();
    const int code =
        probeWindowsImportSandbox(workerExecutable(), input.fileName(),
                                  output.path(), forbidden.fileName());
    const auto detail =
        QString("Windows AppContainer denial probe exit=%1").arg(code);
    require(code == 0 && QFileInfo::exists(output.filePath("sandbox-passed")),
            detail.toUtf8().constData());
    require(
        forbidden.open(QIODevice::ReadOnly) && forbidden.readAll() == "private",
        "Windows sandbox modified unrelated data");
  }
#endif
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
            QString("worker PNG tip import failed: %1")
                .arg(error)
                .toUtf8()
                .constData());
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
          QString("worker-process GBR import failed: %1")
              .arg(error)
              .toUtf8()
              .constData());
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
#ifdef RASTERBRUSH_SANDBOX_TESTING
  QFile placeholder(directory.filePath("blocking-worker.fixture"));
  require(placeholder.open(QIODevice::WriteOnly),
          "cannot stage cancellation fixture");
  placeholder.close();
  testImportCancellationPhases(placeholder.fileName());
#endif
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
    save("portable-manifest", 0,
         encodePortable(record, ResourcePackaging::ManifestOnly, error));
    require(error.isEmpty(), "cannot encode manifest corpus");
    save("portable-embedded", 0,
         encodePortable(record, ResourcePackaging::PrivateCopy, error));
    require(error.isEmpty(), "cannot encode embedded portable corpus");
    save("worker-metadata", 6, encodeWorkerRecord(record, wire.path(), error));
    require(error.isEmpty(), "cannot encode worker wire corpus");
    save("worker-manifest", 8,
         QByteArray("{\"protocol\":2,\"count\":1,\"diagnostics\":[]}"));
    save("worker-fractional-count", 8,
         QByteArray("{\"protocol\":2,\"count\":1.5,\"diagnostics\":[]}"));
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
// Each invariant group runs in a fresh copy of this executable. A failed
// require still aborts that group immediately, while other groups remain
// observable in the same platform build. Worker/probe CLI modes stay separate.
struct PresetCase {
  const char *name;
  void (*run)();
  int timeoutMs;
};
const PresetCase presetCases[] = {
    {"portability", testPortability, 90000},
    {"package", testPackage, 90000},
    {"imports", testImports, 90000},
    {"gih-selection-lifecycle", testGihSelectionLifecycle, 90000},
    {"bundles", testBundles, 90000},
    {"collection", testCollection, 90000},
    {"worker-wire", testWorkerWire, 90000},
    {"imported-status-claims", testImportedStatusClaims, 30000},
    {"library-file-readers", testLibraryFileReaders, 120000},
    {"worker-process", testWorkerProcess, 180000},
    {"response-files", testResponseFiles, 120000}};

int runPresetCases(const QString &corpusDirectory) {
  int failures = 0, cases = 0, totalBoundMs = 0;
  for (const auto &test : presetCases) totalBoundMs += test.timeoutMs + 10000;
  if (!corpusDirectory.isEmpty()) totalBoundMs += 100000;
  std::cout << "Preset case driver: aggregate deadline bound "
            << totalBoundMs / 1000 << " seconds\n";
  auto run = [&](const QString &name, int timeoutMs, const QStringList &extra) {
    ++cases;
    std::cout << "[ RUN      ] " << name.toStdString() << std::endl;
    QProcess child;
    child.setProcessChannelMode(QProcess::MergedChannels);
    QStringList arguments{"--preset-case", name};
    arguments.append(extra);
    child.start(QCoreApplication::applicationFilePath(), arguments);
    const bool started = child.waitForStarted(5000);
    bool finished      = false;
    if (started) {
      QElapsedTimer clock;
      clock.start();
      while (child.state() != QProcess::NotRunning &&
             clock.elapsed() < timeoutMs) {
        child.waitForFinished(int(std::max<qint64>(
            1, std::min<qint64>(100, timeoutMs - clock.elapsed()))));
        const auto output = child.readAll();
        std::cout.write(output.constData(), output.size());
      }
      finished = child.state() == QProcess::NotRunning;
    }
    if (child.state() != QProcess::NotRunning) {
      child.kill();
      child.waitForFinished(5000);
    }
    const auto output = child.readAll();
    std::cout.write(output.constData(), output.size());
    const bool passed = started && finished &&
                        child.exitStatus() == QProcess::NormalExit &&
                        child.exitCode() == 0;
    if (!passed) ++failures;
    std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ")
              << name.toStdString();
    if (!started)
      std::cout << " (launch: " << child.errorString().toStdString() << ')';
    else if (!finished)
      std::cout << " (timeout after " << timeoutMs / 1000 << " seconds)";
    else if (child.exitStatus() != QProcess::NormalExit)
      std::cout << " (crash)";
    else if (!passed)
      std::cout << " (exit " << child.exitCode() << ')';
    std::cout << std::endl;
  };
  for (const auto &test : presetCases)
    run(QString::fromLatin1(test.name), test.timeoutMs, {});
  if (!corpusDirectory.isEmpty()) run("corpus", 90000, {corpusDirectory});
  std::cout << "Preset cases: " << cases - failures << " passed, " << failures
            << " failed, " << cases << " executed\n";
  return failures ? 1 : 0;
}
}  // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
#ifdef __APPLE__
  if (argc == 4 && app.arguments().at(1) == "--raster-brush-import-stream")
    return runMacImportStream(app.arguments().at(2), app.arguments().at(3),
                              [](const QString &input, const QString &output) {
                                if (input.endsWith("oversized-worker.fixture"))
                                  return oversizedWorkerFixture(output);
                                if (input.endsWith("blocking-worker.fixture"))
                                  return blockingCancellationFixture(output);
                                return runImportWorker(input, output);
                              });
#endif
#if defined(_WIN32) && defined(RASTERBRUSH_SANDBOX_TESTING)
  if (argc == 5 && app.arguments().at(1) == "--test-windows-sandbox-parent")
    return probeWindowsImportSandbox(
        QCoreApplication::applicationFilePath(), app.arguments().at(2),
        app.arguments().at(3), app.arguments().at(4));
  if (argc == 6 && app.arguments().at(1) == "--test-windows-sandbox-worker")
    return probeWindowsImportSandboxWorker(
        app.arguments().at(2), app.arguments().at(3), app.arguments().at(4),
        app.arguments().at(5).toUShort());
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
      return blockingCancellationFixture(app.arguments().at(3));
    return runImportWorker(app.arguments().at(2), app.arguments().at(3));
  }
  try {
    if (argc == 4 && app.arguments().at(1) == "--test-write-response")
      return badResponseFixture(app.arguments().at(2), app.arguments().at(3));
    if (argc == 4 && app.arguments().at(1) == "--test-library-file") {
      runLibraryFileCase(app.arguments().at(2), app.arguments().at(3));
      return 0;
    }
    if (argc == 4 && app.arguments().at(1) == "--test-response-case") {
      runResponseCase(app.arguments().at(2), app.arguments().at(3));
      return 0;
    }
    if (argc == 2 && app.arguments().at(1) == "--test-worker-budget") {
      testWorkerPeakBudget();
      return 0;
    }
    if (argc == 2 && app.arguments().at(1) == "--list-preset-cases") {
      for (const auto &test : presetCases) std::cout << test.name << '\n';
      return 0;
    }
    if (argc >= 3 && app.arguments().at(1) == "--preset-case") {
      const auto name = app.arguments().at(2);
      if (argc == 4 && name == "corpus") {
        writeCorpus(app.arguments().at(3));
        return 0;
      }
      if (argc == 3)
        for (const auto &test : presetCases)
          if (name == QString::fromLatin1(test.name)) {
            test.run();
            return 0;
          }
      std::cerr << "Unknown preset case or invalid case arguments\n";
      return 2;
    }
    if (argc == 1) return runPresetCases({});
    if (argc == 3 && app.arguments().at(1) == "--write-corpus" &&
        !app.arguments().at(2).isEmpty())
      return runPresetCases(app.arguments().at(2));
    std::cerr << "Invalid preset test arguments\n";
    return 2;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
