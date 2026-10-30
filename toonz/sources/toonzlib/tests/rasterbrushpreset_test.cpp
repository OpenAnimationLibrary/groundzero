#include "toonz/rasterbrushpreset.h"
#include "toonz/rasterbrushimport.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QImage>
#include <QBuffer>
#include <iostream>
#include <stdexcept>
using namespace RasterBrush;
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
          "ratio=\"1\" hfade=\"1\" vfade=\"1\" spikes=\"2\"/>"
          "</Brush>]]></param><param name=\"SmudgeRateMode\">1</param>"
          "<param name=\"SmudgeRateUseNewEngine\">true</param></Preset>");
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  require(image.save(&buffer, "PNG"), "cannot create original KPP fixture");
  return bytes;
}
QByteArray externalKpp() {
  QImage image(2, 2, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  image.setText("version", "2.2");
  image.setText(
      "preset",
      "<Preset name=\"External fixture\" paintopid=\"paintbrush\"><param "
      "name=\"brush_definition\"><![CDATA[<Brush type=\"gbr_brush\" "
      "filename=\"original.gbr\" scale=\"1\" spacing=\"0.25\" "
      "angle=\"0\"/>]]></param></Preset>");
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
  auto dry = importBrush(kpp(), "kpp", error);
  require(dry.complete, qPrintable(error));
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
  r.provenance  = {{"license", "CC0-1.0"}, {"origin", "original test fixture"}};
  auto image    = std::make_shared<Image>();
  image->width  = 2;
  image->height = 2;
  image->color  = true;
  image->pixels = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {0, 0, 0, 0}};
  r.preset.tip.shape = Shape::Bitmap;
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
  r.preset.saturation.value = .7;
  r.preset.brightness.value = 1.2;
  r.preset.seed             = UINT64_C(18446744073709551615);
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
  require(restored.preset.pickupControl.mappings.size() == 1,
          "wet dynamics lost in package");
  require(restored.preset.hue.mappings.size() == 1 &&
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
}  // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testPackage();
    testImports();
    testBundles();
    std::cout << "Native brush package tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
