#include "toonz/rasterbrushpreset.h"
#include "rasterbrushpng.h"
#include "rasterbrushresponse.h"
#include "rasterbrushbundlemetadata.h"
#include <QColorSpace>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QSet>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace RasterBrush {
namespace {
constexpr int maxBytes = 64 * 1024 * 1024;
struct Invalid : std::runtime_error {
  explicit Invalid(const QString &s) : std::runtime_error(s.toStdString()) {}
};
void keys(const QJsonObject &o, const QStringList &allowed) {
  for (auto i = o.begin(); i != o.end(); ++i)
    if (!allowed.contains(i.key()))
      throw Invalid("Unknown active field: " + i.key());
}
double number(const QJsonValue &v) {
  if (!v.isDouble() || !std::isfinite(v.toDouble()))
    throw Invalid("Expected a finite number");
  return v.toDouble();
}
bool boolean(const QJsonValue &v) {
  if (!v.isBool()) throw Invalid("Expected a boolean");
  return v.toBool();
}
QString text(const QJsonValue &v) {
  if (!v.isString() || v.toString().size() > 4096)
    throw Invalid("Expected a bounded string");
  return v.toString();
}
QJsonObject object(const QJsonValue &v) {
  if (!v.isObject()) throw Invalid("Expected an object");
  return v.toObject();
}
QJsonArray array(const QJsonValue &v, int maximum) {
  if (!v.isArray() || v.toArray().size() > maximum)
    throw Invalid("Invalid array or array budget exceeded");
  return v.toArray();
}
int choice(const QJsonValue &v, const QStringList &names) {
  int i = names.indexOf(text(v));
  if (i < 0) throw Invalid("Unknown mode: " + v.toString());
  return i;
}
const QStringList sensors = {"pressure",
                             "tilt_x",
                             "tilt_y",
                             "tilt_elevation",
                             "tilt_direction",
                             "rotation",
                             "direction",
                             "distance",
                             "time",
                             "speed",
                             "random_dab",
                             "random_stroke",
                             "initial_direction",
                             "dab_index"};
const QStringList shapes  = {"round", "square", "gaussian", "bitmap"};
const QStringList blends = {"normal", "multiply", "screen", "overlay", "erase"};
const QStringList masks  = {"multiply", "subtract"};
const QStringList sourceHsvChannels = {"off", "hue", "saturation", "value"};
const QStringList wets              = {"off", "dulling", "smear"};
const QStringList interpolations    = {"linear", "natural-cubic"};
QJsonObject dynamicJson(const Dynamic &d) {
  QJsonArray maps;
  for (const auto &m : d.mappings) {
    QJsonArray curve;
    for (auto k : m.curve) curve.append(QJsonArray{k.x, k.y});
    maps.append(
        QJsonObject{{"sensor", sensors[int(m.sensor)]},
                    {"scale", m.scale},
                    {"periodic", m.periodic},
                    {"interpolation", interpolations[int(m.interpolation)]},
                    {"curve", curve}});
  }
  return {{"value", d.value}, {"minimum", d.minimum}, {"mappings", maps}};
}
Dynamic dynamic(const QJsonValue &v) {
  auto o = object(v);
  keys(o, {"value", "minimum", "mappings"});
  Dynamic d;
  d.value   = number(o["value"]);
  d.minimum = number(o["minimum"]);
  for (auto item : array(o["mappings"], 16)) {
    auto a = object(item);
    keys(a, {"sensor", "scale", "curve", "interpolation", "periodic"});
    Mapping m;
    m.sensor = Sensor(choice(a["sensor"], sensors));
    m.scale  = number(a["scale"]);
    if (a.contains("periodic")) m.periodic = boolean(a["periodic"]);
    if (a.contains("interpolation"))
      m.interpolation =
          Interpolation(choice(a["interpolation"], interpolations));
    m.curve.clear();
    for (auto k : array(a["curve"], 1024)) {
      auto pair = array(k, 2);
      if (pair.size() != 2) throw Invalid("Invalid curve point");
      m.curve.push_back({number(pair[0]), number(pair[1])});
    }
    d.mappings.push_back(m);
  }
  return d;
}
int byte(double v) {
  return std::max(0, std::min(255, int(std::floor(v * 255 + .5))));
}
QImage toImage(const Image &im) {
  QImage out(im.width, im.height, QImage::Format_ARGB32);
  if (out.isNull()) throw Invalid("Image allocation failed");
  for (int y = 0; y < im.height; ++y)
    for (int x = 0; x < im.width; ++x) {
      auto p = im.pixels[size_t(y) * im.width + x];
      out.setPixel(
          x, y,
          qRgba(p.a > 0 ? byte(p.r / p.a) : 0, p.a > 0 ? byte(p.g / p.a) : 0,
                p.a > 0 ? byte(p.b / p.a) : 0, byte(p.a)));
    }
  return out;
}
QString assetId(const QByteArray &png, bool color) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(png);
  hash.addData(color ? "color" : "mask", color ? 5 : 4);
  return QString::fromLatin1(hash.result().toHex());
}
QByteArray canonicalPixels(const Image &im) {
  QByteArray raw;
  raw.reserve(8 + int(im.pixels.size() * 4));
  for (unsigned value : {unsigned(im.width), unsigned(im.height)})
    for (int shift = 24; shift >= 0; shift -= 8)
      raw.append(char(value >> shift));
  for (const Pixel &pixel : im.pixels) {
    const int alpha = byte(pixel.a);
    raw.append(char(alpha > 0 && im.color ? byte(pixel.r / pixel.a) : 0));
    raw.append(char(alpha > 0 && im.color ? byte(pixel.g / pixel.a) : 0));
    raw.append(char(alpha > 0 && im.color ? byte(pixel.b / pixel.a) : 0));
    raw.append(char(alpha));
  }
  return raw;
}
void validateRights(const QJsonObject &rights) {
  static const QRegularExpression identity("^[0-9a-f]{64}$");
  if (rights.size() > 512 || QJsonDocument(rights).toJson().size() > 256 * 1024)
    throw Invalid("Resource declaration budget exceeded");
  for (auto i = rights.begin(); i != rights.end(); ++i) {
    if (!identity.match(i.key()).hasMatch())
      throw Invalid("Invalid resource declaration hash");
    auto entry = object(i.value());
    keys(entry, {"redistribution", "license"});
    const auto permission = text(entry["redistribution"]);
    if (permission != "unknown" && permission != "permitted" &&
        permission != "restricted")
      throw Invalid("Invalid resource redistribution declaration");
    if (text(entry["license"]).size() > 256)
      throw Invalid("Resource license declaration is too long");
  }
}
struct Writer {
  QJsonObject assets;
  QString rawDirectory;
  QMap<QString, std::shared_ptr<const Image>> *images = nullptr;
  QString image(const std::shared_ptr<const Image> &im) {
    if (!im) return {};
    if (!rawDirectory.isEmpty()) {
      QByteArray raw;
      raw.reserve(8 + int(im->pixels.size() * 4));
      for (unsigned value : {unsigned(im->width), unsigned(im->height)})
        for (int shift = 24; shift >= 0; shift -= 8)
          raw.append(char(value >> shift));
      for (const Pixel &pixel : im->pixels) {
        raw.append(char(pixel.a > 0 ? byte(pixel.r / pixel.a) : 0));
        raw.append(char(pixel.a > 0 ? byte(pixel.g / pixel.a) : 0));
        raw.append(char(pixel.a > 0 ? byte(pixel.b / pixel.a) : 0));
        raw.append(char(byte(pixel.a)));
      }
      const QString id   = assetId(raw, im->color);
      const QString path = QDir(rawDirectory).filePath(id + ".rgba8");
      QFile file(path);
      if (QFileInfo::exists(path)) {
        if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly) ||
            file.size() != raw.size() || file.read(raw.size() + 1) != raw)
          throw Invalid("Conflicting worker image resource");
      } else if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                 file.write(raw) != raw.size() || !file.flush())
        throw Invalid("Cannot write private worker image");
      assets[id] = QJsonObject{{"rgba8", id}, {"color", im->color}};
      return id;
    }
    QByteArray png;
    QBuffer b(&png);
    b.open(QIODevice::WriteOnly);
    if (!toImage(*im).save(&b, "PNG"))
      throw Invalid("Cannot encode brush image");
    QString id = assetId(png, im->color);
    assets[id] = QJsonObject{{"png", QString::fromLatin1(png.toBase64())},
                             {"color", im->color}};
    if (images) (*images)[id] = im;
    return id;
  }
  QJsonObject tip(const Tip &t) {
    QJsonArray refs;
    for (auto im : t.images) refs.append(image(im));
    return {{"shape", shapes[int(t.shape)]},
            {"hardness", t.hardness},
            {"antialiasEdges", t.antialiasEdges},
            {"images", refs},
            {"randomSequence", t.randomSequence}};
  }
};
std::shared_ptr<const Image> readWorkerImage(
    const QString &directory, const QString &id, bool color, size_t remaining,
    const Detail::RegularResponseDirectory *anchored) {
  static const QRegularExpression identity("^[0-9a-f]{64}$");
  if (!identity.match(id).hasMatch()) throw Invalid("Invalid worker image ID");
  Detail::RegularResponseDirectory localDirectory;
  Detail::RegularResponseFile file;
  QString why;
  if (!anchored) {
    if (!localDirectory.open(directory, why)) throw Invalid(why);
    anchored = &localDirectory;
  }
  if (!file.open(*anchored, id + ".rgba8", 8 + 4 * 2 * 1024 * 1024, why))
    throw Invalid(why);
  const QByteArray header = file.read(8, why);
  if (header.size() != 8) throw Invalid("Truncated worker image header");
  auto word = [&](int offset) {
    unsigned value = 0;
    for (int i = 0; i < 4; ++i)
      value = (value << 8) | static_cast<unsigned char>(header[offset + i]);
    return value;
  };
  const unsigned width = word(0), height = word(4);
  if (!width || !height || width > 8192 || height > 8192 ||
      size_t(width) * height > 2 * 1024 * 1024)
    throw Invalid("Worker image geometry exceeds limits");
  const size_t pixels = size_t(width) * height;
  if (pixels > remaining / (sizeof(Pixel) + 8))
    throw Invalid("Remaining worker image budget exceeded before allocation");
  const qint64 length = qint64(pixels * 4);
  if (file.size() != length + 8)
    throw Invalid("Invalid worker image byte count");
  const QByteArray rgba = file.read(length, why);
  if (!why.isEmpty() || rgba.size() != length || !file.finish(why) ||
      assetId(header + rgba, color) != id)
    throw Invalid("Worker image checksum mismatch");
  auto image    = std::make_shared<Image>();
  image->width  = int(width);
  image->height = int(height);
  image->color  = color;
  image->pixels.resize(pixels);
  for (size_t i = 0; i < pixels; ++i) {
    const auto *v =
        reinterpret_cast<const unsigned char *>(rgba.constData()) + i * 4;
    const double alpha = v[3] / 255.0;
    image->pixels[i]   = {color ? v[0] / 255.0 * alpha : 0,
                        color ? v[1] / 255.0 * alpha : 0,
                        color ? v[2] / 255.0 * alpha : 0, alpha};
  }
  return image;
}
struct Reader {
  QMap<QString, std::shared_ptr<const Image>> assets;
  std::shared_ptr<const Image> image(const QJsonValue &v) {
    QString id = text(v);
    if (id.isEmpty()) return {};
    if (!assets.contains(id)) throw Invalid("Missing brush resource: " + id);
    return assets[id];
  }
  Tip tip(const QJsonValue &v) {
    auto o = object(v);
    keys(o,
         {"shape", "hardness", "images", "randomSequence", "antialiasEdges"});
    Tip t;
    t.shape    = Shape(choice(o["shape"], shapes));
    t.hardness = number(o["hardness"]);
    t.antialiasEdges =
        o.contains("antialiasEdges") ? boolean(o["antialiasEdges"]) : false;
    t.randomSequence = boolean(o["randomSequence"]);
    for (auto r : array(o["images"], 256)) {
      auto im = image(r);
      if (!im) throw Invalid("Empty tip resource");
      t.images.push_back(im);
    }
    return t;
  }
};
}  // namespace

std::shared_ptr<const Image> decodeImage(const QByteArray &bytes, bool color,
                                         QString &error,
                                         size_t maximumDecodedBytes) {
  error.clear();
  if (bytes.isEmpty() || bytes.size() > maxBytes) {
    error = "Brush image file budget exceeded";
    return {};
  }
  auto png = boundedPng(bytes, error);
  if (!error.isEmpty()) return {};
  QBuffer buffer;
  buffer.setData(png.image);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer);
  reader.setDecideFormatFromContent(true);
  if (reader.format() != "png") {
    error = "Only PNG brush image resources are accepted";
    return {};
  }
  QSize size = reader.size();
  if (size.width() < 1 || size.height() < 1 || size.width() > 8192 ||
      size.height() > 8192 ||
      qint64(size.width()) * size.height() > 2 * 1024 * 1024) {
    error = "Brush image allocation budget exceeded";
    return {};
  }
  // Reserve room for retained float pixels and bounded codec/conversion
  // scratch before Qt allocates a decoded raster, not after returning it.
  const size_t pixels = size_t(size.width()) * size.height();
  if (pixels > std::min(maximumDecodedBytes, size_t(256u * 1024u * 1024u)) /
                   (sizeof(Pixel) + 16)) {
    error = "Remaining decoded image budget exceeded before allocation";
    return {};
  }
  QImage q = reader.read().convertToFormat(QImage::Format_ARGB32);
  if (q.isNull() || q.size() != size) {
    error = "Cannot decode brush image";
    return {};
  }
  if (color && q.colorSpace().isValid())
    q = q.convertedToColorSpace(QColorSpace::SRgb);
  std::shared_ptr<Image> im(new Image);
  im->width  = q.width();
  im->height = q.height();
  im->color  = color;
  im->pixels.resize(size_t(im->width) * im->height);
  for (int y = 0; y < im->height; ++y)
    for (int x = 0; x < im->width; ++x) {
      QRgb p                                = q.pixel(x, y);
      double a                              = qAlpha(p) / 255.0;
      im->pixels[size_t(y) * im->width + x] = {
          color ? qRed(p) / 255.0 * a : 0, color ? qGreen(p) / 255.0 * a : 0,
          color ? qBlue(p) / 255.0 * a : 0, a};
    }
  return im;
}

static QByteArray encodeRecord(
    const Record &r, QString &error, const QString &rawDirectory,
    QMap<QString, std::shared_ptr<const Image>> *images = nullptr) {
  error.clear();
  try {
    validateRights(r.resourceRights);
    if (r.provenance.contains("bundleMetadata")) {
      const auto why =
          Detail::bundleMetadataError(r.provenance["bundleMetadata"]);
      if (!why.isEmpty()) throw Invalid(why);
    }
    std::string why = validate(r.preset);
    if (!why.empty()) throw Invalid(QString::fromStdString(why));
    if (QString::fromStdString(r.preset.name).size() > 4096 ||
        QJsonDocument(r.provenance).toJson().size() > 16384)
      throw Invalid("Preset metadata budget exceeded");
    if (r.diagnostics.size() > 1024 ||
        QJsonDocument(QJsonArray::fromStringList(r.diagnostics))
                .toJson()
                .size() > 256 * 1024)
      throw Invalid("Preset diagnostic budget exceeded");
    for (const auto &note : r.diagnostics)
      if (note.size() > 16384) throw Invalid("Preset diagnostic is too long");
    Writer w;
    w.rawDirectory = rawDirectory;
    w.images       = images;
    const auto &p  = r.preset;
    const auto &t  = p.texture;
    const auto &s  = p.secondary;
    QJsonObject settings{
        {"name", QString::fromStdString(p.name)},
        {"profile", QString::fromStdString(p.profile)},
        {"tip", w.tip(p.tip)},
        {"size", dynamicJson(p.size)},
        {"aspect", dynamicJson(p.aspect)},
        {"angle", dynamicJson(p.angle)},
        {"opacity", dynamicJson(p.opacity)},
        {"flow", dynamicJson(p.flow)},
        {"scatter", dynamicJson(p.scatter)},
        {"countControl", dynamicJson(p.countControl)},
        {"scatterBothAxes", p.scatterBothAxes},
        {"spacing", p.spacing},
        {"airbrushRate", p.airbrushRate},
        {"airbrushIgnoreSpacing", p.airbrushIgnoreSpacing},
        {"count", p.count},
        {"wash", p.wash},
        {"lockAlpha", p.lockAlpha},
        {"flipX", p.flipX},
        {"flipY", p.flipY},
        {"blend", blends[int(p.blend)]},
        {"color", QJsonArray{p.color.r, p.color.g, p.color.b, p.color.a}},
        {"background", QJsonArray{p.background.r, p.background.g,
                                  p.background.b, p.background.a}},
        {"backgroundMix", dynamicJson(p.backgroundMix)},
        {"hue", dynamicJson(p.hue)},
        {"saturation", dynamicJson(p.saturation)},
        {"brightness", dynamicJson(p.brightness)},
        {"wet", wets[int(p.wet)]},
        {"pickup", p.pickup},
        {"sourceSmudgeLengthEnabled", p.sourceSmudgeLengthEnabled},
        {"sourceSmearAlpha", p.sourceSmearAlpha},
        {"sourceColorRateEnabled", p.sourceColorRateEnabled},
        {"sourceTextureSoftTexturing", p.sourceTextureSoftTexturing},
        {"sourceMirrorV1",
         QJsonObject{{"enabled", p.sourceMirror.enabled},
                     {"horizontal", p.sourceMirror.horizontal},
                     {"vertical", p.sourceMirror.vertical},
                     {"control", dynamicJson(p.sourceMirror.control)}}},
        {"sourceMaskedV1",
         QJsonObject{{"enabled", p.sourceMasked.enabled},
                     {"useMasterSize", p.sourceMasked.useMasterSize},
                     {"diameter", p.sourceMasked.diameter},
                     {"masterSizeCoeff", p.sourceMasked.masterSizeCoeff},
                     {"aspect", p.sourceMasked.aspect},
                     {"spacing", p.sourceMasked.spacing},
                     {"opacity", p.sourceMasked.opacity},
                     {"flow", p.sourceMasked.flow},
                     {"mode", masks[int(p.sourceMasked.mode)]}}},
        {"sourceHsvChannelV1", sourceHsvChannels[int(p.sourceHsvChannel)]},
        {"sourceHsvValueV1", p.sourceHsvValue},
        {"sourceHsvControlsV2",
         QJsonArray{
             QJsonObject{{"enabled", p.sourceHsvV2[0].enabled},
                         {"control", dynamicJson(p.sourceHsvV2[0].control)}},
             QJsonObject{{"enabled", p.sourceHsvV2[1].enabled},
                         {"control", dynamicJson(p.sourceHsvV2[1].control)}},
             QJsonObject{{"enabled", p.sourceHsvV2[2].enabled},
                         {"control", dynamicJson(p.sourceHsvV2[2].control)}}}},
        {"pickupControl", dynamicJson(p.pickupControl)},
        {"colorRateControl", dynamicJson(p.colorRateControl)},
        {"radiusControl", dynamicJson(p.radiusControl)},
        {"colorRate", p.colorRate},
        {"reload", p.reload},
        {"smudgeRadius", p.smudgeRadius},
        {"varySeedPerStroke", p.varySeedPerStroke},
        {"seed", QString::number(p.seed)}};
    settings["texture"] =
        QJsonObject{{"image", w.image(t.image)},  {"mode", masks[int(t.mode)]},
                    {"scale", t.scale},           {"angle", t.angle},
                    {"offsetX", t.offsetX},       {"offsetY", t.offsetY},
                    {"depth", t.depth},           {"contrast", t.contrast},
                    {"brightness", t.brightness}, {"invert", t.invert},
                    {"perDab", t.perDab}};
    settings["secondary"] =
        QJsonObject{{"enabled", s.enabled},
                    {"tip", w.tip(s.tip)},
                    {"mode", masks[int(s.mode)]},
                    {"size", dynamicJson(s.size)},
                    {"angle", dynamicJson(s.angle)},
                    {"opacity", dynamicJson(s.opacity)},
                    {"aspect", s.aspect},
                    {"count", s.count},
                    {"flipX", s.flipX},
                    {"flipY", s.flipY},
                    {"scatter", s.scatter},
                    {"independentSpacing", s.independentSpacing},
                    {"spacing", s.spacing}};
    if (w.assets.size() > 512) throw Invalid("Too many resources");
    QByteArray result =
        QJsonDocument(
            QJsonObject{
                {"format", rawDirectory.isEmpty()
                               ? "groundzero-raster-brush"
                               : "groundzero-brush-worker-record"},
                {"version", 1},
                {"settings", settings},
                {"assets", w.assets},
                {"provenance", r.provenance},
                {"resourceRights", r.resourceRights},
                {"diagnostics", QJsonArray::fromStringList(r.diagnostics)}})
            .toJson();
    if (result.size() > maxBytes) throw Invalid("Brush package exceeds 64 MiB");
    return result;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return {};
  }
}

static Record decodeRecord(
    const QByteArray &bytes, QString &error, size_t maximumDecodedBytes,
    const QString &rawDirectory,
    const Detail::RegularResponseDirectory *anchored = nullptr) {
  maximumDecodedBytes =
      std::min(maximumDecodedBytes, size_t(256u * 1024u * 1024u));
  Record r;
  error.clear();
  try {
    if (bytes.isEmpty() || bytes.size() > maxBytes)
      throw Invalid("Brush package exceeds 64 MiB");
    QJsonParseError parse;
    auto doc = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject())
      throw Invalid("Invalid brush JSON: " + parse.errorString());
    auto root = doc.object();
    keys(root, {"format", "version", "settings", "assets", "provenance",
                "resourceRights", "diagnostics"});
    if (text(root["format"]) != (rawDirectory.isEmpty()
                                     ? "groundzero-raster-brush"
                                     : "groundzero-brush-worker-record") ||
        number(root["version"]) != 1)
      throw Invalid("Unsupported native brush version");
    if (root.contains("diagnostics")) {
      auto notes = array(root["diagnostics"], 1024);
      if (QJsonDocument(notes).toJson().size() > 256 * 1024)
        throw Invalid("Preset diagnostic budget exceeded");
      for (const auto &note : notes) {
        if (!note.isString() || note.toString().size() > 16384)
          throw Invalid("Invalid preset diagnostic");
        r.diagnostics << note.toString();
      }
    }
    Reader rd;
    auto assets = object(root["assets"]);
    if (assets.size() > 512) throw Invalid("Too many resources");
    size_t total = 0;
    for (auto i = assets.begin(); i != assets.end(); ++i) {
      auto a = object(i.value());
      std::shared_ptr<const Image> im;
      if (!rawDirectory.isEmpty()) {
        keys(a, {"rgba8", "color"});
        if (text(a["rgba8"]) != i.key())
          throw Invalid("Conflicting worker image ID");
        im = readWorkerImage(rawDirectory, i.key(), boolean(a["color"]),
                             maximumDecodedBytes - total, anchored);
      } else {
        keys(a, {"png", "color"});
        if (!a["png"].isString()) throw Invalid("Invalid image encoding");
        auto png   = QByteArray::fromBase64(a["png"].toString().toLatin1());
        bool color = boolean(a["color"]);
        if (assetId(png, color) != i.key())
          throw Invalid("Brush resource hash mismatch");
        QString why;
        im = decodeImage(png, color, why, maximumDecodedBytes - total);
        if (!im) throw Invalid(why);
      }
      total += im->pixels.size() * sizeof(Pixel);
      if (total > maximumDecodedBytes)
        throw Invalid("Aggregate resource budget exceeded");
      rd.assets[i.key()] = im;
    }
    auto o = object(root["settings"]);
    keys(o, {"name",
             "profile",
             "tip",
             "size",
             "aspect",
             "angle",
             "opacity",
             "flow",
             "scatter",
             "spacing",
             "airbrushRate",
             "airbrushIgnoreSpacing",
             "count",
             "wash",
             "lockAlpha",
             "flipX",
             "flipY",
             "blend",
             "color",
             "wet",
             "pickup",
             "colorRate",
             "reload",
             "smudgeRadius",
             "seed",
             "varySeedPerStroke",
             "texture",
             "secondary",
             "pickupControl",
             "colorRateControl",
             "radiusControl",
             "background",
             "backgroundMix",
             "hue",
             "saturation",
             "brightness",
             "countControl",
             "scatterBothAxes",
             "sourceSmudgeLengthEnabled",
             "sourceSmearAlpha",
             "sourceColorRateEnabled",
             "sourceTextureSoftTexturing",
             "sourceMirrorV1",
             "sourceMaskedV1",
             "sourceHsvChannelV1",
             "sourceHsvValueV1",
             "sourceHsvControlsV2"});
    auto &p   = r.preset;
    p.name    = text(o["name"]).toStdString();
    p.profile = text(o["profile"]).toStdString();
    p.tip     = rd.tip(o["tip"]);
    p.size    = dynamic(o["size"]);
    p.aspect  = dynamic(o["aspect"]);
    p.angle   = dynamic(o["angle"]);
    p.opacity = dynamic(o["opacity"]);
    p.flow    = dynamic(o["flow"]);
    p.scatter = dynamic(o["scatter"]);
    if (o.contains("countControl")) p.countControl = dynamic(o["countControl"]);
    if (o.contains("scatterBothAxes"))
      p.scatterBothAxes = boolean(o["scatterBothAxes"]);
    p.spacing      = number(o["spacing"]);
    p.airbrushRate = number(o["airbrushRate"]);
    if (o.contains("airbrushIgnoreSpacing"))
      p.airbrushIgnoreSpacing = boolean(o["airbrushIgnoreSpacing"]);
    double count = number(o["count"]);
    if (count < 1 || count > 64 || count != std::floor(count))
      throw Invalid("Invalid dab count");
    p.count = int(count);
    p.wash  = boolean(o["wash"]);
    if (o.contains("sourceSmudgeLengthEnabled"))
      p.sourceSmudgeLengthEnabled = boolean(o["sourceSmudgeLengthEnabled"]);
    if (o.contains("sourceHsvChannelV1") != o.contains("sourceHsvValueV1"))
      throw Invalid("Incomplete versioned source HSV settings");
    if (o.contains("sourceHsvChannelV1")) {
      p.sourceHsvChannel =
          SourceHsvChannel(choice(o["sourceHsvChannelV1"], sourceHsvChannels));
      p.sourceHsvValue = number(o["sourceHsvValueV1"]);
    }
    if (o.contains("sourceHsvControlsV2")) {
      const auto controls = array(o["sourceHsvControlsV2"], 3);
      if (controls.size() != 3)
        throw Invalid("Incomplete source HSV V2 channel set");
      for (int channel = 0; channel < 3; ++channel) {
        const auto setting = object(controls[channel]);
        keys(setting, {"enabled", "control"});
        p.sourceHsvV2[channel].enabled = boolean(setting["enabled"]);
        p.sourceHsvV2[channel].control = dynamic(setting["control"]);
      }
    }
    if (o.contains("sourceMirrorV1")) {
      const auto mirror = object(o["sourceMirrorV1"]);
      keys(mirror, {"enabled", "horizontal", "vertical", "control"});
      p.sourceMirror.enabled    = boolean(mirror["enabled"]);
      p.sourceMirror.horizontal = boolean(mirror["horizontal"]);
      p.sourceMirror.vertical   = boolean(mirror["vertical"]);
      p.sourceMirror.control    = dynamic(mirror["control"]);
    }
    if (o.contains("sourceMaskedV1")) {
      const auto mask = object(o["sourceMaskedV1"]);
      keys(mask, {"enabled", "useMasterSize", "diameter", "masterSizeCoeff",
                  "aspect", "spacing", "opacity", "flow", "mode"});
      p.sourceMasked.enabled         = boolean(mask["enabled"]);
      p.sourceMasked.useMasterSize   = boolean(mask["useMasterSize"]);
      p.sourceMasked.diameter        = number(mask["diameter"]);
      p.sourceMasked.masterSizeCoeff = number(mask["masterSizeCoeff"]);
      p.sourceMasked.aspect          = number(mask["aspect"]);
      p.sourceMasked.spacing         = number(mask["spacing"]);
      p.sourceMasked.opacity         = number(mask["opacity"]);
      p.sourceMasked.flow            = number(mask["flow"]);
      p.sourceMasked.mode            = MaskMode(choice(mask["mode"], masks));
    }
    if (o.contains("sourceTextureSoftTexturing"))
      p.sourceTextureSoftTexturing = boolean(o["sourceTextureSoftTexturing"]);
    if (o.contains("sourceColorRateEnabled"))
      p.sourceColorRateEnabled = boolean(o["sourceColorRateEnabled"]);
    if (o.contains("sourceSmearAlpha"))
      p.sourceSmearAlpha = boolean(o["sourceSmearAlpha"]);
    p.lockAlpha = boolean(o["lockAlpha"]);
    p.flipX     = boolean(o["flipX"]);
    p.flipY     = boolean(o["flipY"]);
    p.blend     = Blend(choice(o["blend"], blends));
    auto color  = array(o["color"], 4);
    if (color.size() != 4) throw Invalid("Invalid color");
    p.color = {number(color[0]), number(color[1]), number(color[2]),
               number(color[3])};
    if (o.contains("background")) {
      auto background = array(o["background"], 4);
      if (background.size() != 4) throw Invalid("Invalid background color");
      p.background = {number(background[0]), number(background[1]),
                      number(background[2]), number(background[3])};
    }
    if (o.contains("backgroundMix"))
      p.backgroundMix = dynamic(o["backgroundMix"]);
    if (o.contains("hue")) p.hue = dynamic(o["hue"]);
    if (o.contains("saturation")) p.saturation = dynamic(o["saturation"]);
    if (o.contains("brightness")) p.brightness = dynamic(o["brightness"]);
    p.wet          = Wet(choice(o["wet"], wets));
    p.pickup       = number(o["pickup"]);
    p.colorRate    = number(o["colorRate"]);
    p.reload       = number(o["reload"]);
    p.smudgeRadius = number(o["smudgeRadius"]);
    if (o.contains("pickupControl"))
      p.pickupControl = dynamic(o["pickupControl"]);
    if (o.contains("colorRateControl"))
      p.colorRateControl = dynamic(o["colorRateControl"]);
    if (o.contains("radiusControl"))
      p.radiusControl = dynamic(o["radiusControl"]);
    p.varySeedPerStroke = o.contains("varySeedPerStroke")
                              ? boolean(o["varySeedPerStroke"])
                              : false;
    bool ok             = false;
    p.seed              = text(o["seed"]).toULongLong(&ok);
    if (!ok) throw Invalid("Invalid random seed");
    auto t = object(o["texture"]);
    keys(t, {"image", "mode", "scale", "angle", "offsetX", "offsetY", "depth",
             "contrast", "brightness", "invert", "perDab"});
    p.texture.image      = rd.image(t["image"]);
    p.texture.mode       = MaskMode(choice(t["mode"], masks));
    p.texture.scale      = number(t["scale"]);
    p.texture.angle      = number(t["angle"]);
    p.texture.offsetX    = number(t["offsetX"]);
    p.texture.offsetY    = number(t["offsetY"]);
    p.texture.depth      = number(t["depth"]);
    p.texture.contrast   = number(t["contrast"]);
    p.texture.brightness = number(t["brightness"]);
    p.texture.invert     = boolean(t["invert"]);
    p.texture.perDab     = boolean(t["perDab"]);
    auto s               = object(o["secondary"]);
    keys(s, {"enabled", "tip", "mode", "size", "angle", "opacity", "aspect",
             "scatter", "count", "flipX", "flipY", "independentSpacing",
             "spacing"});
    p.secondary.enabled = boolean(s["enabled"]);
    p.secondary.tip     = rd.tip(s["tip"]);
    p.secondary.mode    = MaskMode(choice(s["mode"], masks));
    p.secondary.size    = dynamic(s["size"]);
    p.secondary.angle   = dynamic(s["angle"]);
    p.secondary.opacity = dynamic(s["opacity"]);
    p.secondary.aspect  = number(s["aspect"]);
    p.secondary.scatter = number(s["scatter"]);
    if (s.contains("count")) {
      const double count = number(s["count"]);
      if (count != std::floor(count) || count < 1 || count > 64)
        throw Invalid("Invalid secondary stamp count");
      p.secondary.count = int(count);
    }
    if (s.contains("independentSpacing"))
      p.secondary.independentSpacing = boolean(s["independentSpacing"]);
    if (s.contains("spacing")) {
      p.secondary.spacing = number(s["spacing"]);
      if (p.secondary.spacing < .01 || p.secondary.spacing > 5)
        throw Invalid("Invalid independent secondary spacing");
    }
    if (s.contains("flipX")) p.secondary.flipX = boolean(s["flipX"]);
    if (s.contains("flipY")) p.secondary.flipY = boolean(s["flipY"]);
    if (root.contains("resourceRights")) {
      r.resourceRights = object(root["resourceRights"]);
      validateRights(r.resourceRights);
    }
    r.provenance = object(root["provenance"]);
    // Source support and reference verification are evidence produced by this
    // application, never credentials supplied by an imported JSON document.
    // Keep native decoded/semanticallySupported separate from source parity.
    for (const QString &key :
         {"sourceReferenceVerified", "sourceSemanticallySupported"})
      if (r.provenance.contains(key)) r.provenance[key] = false;
    if (r.provenance.contains("bundleMetadata")) {
      const auto why =
          Detail::bundleMetadataError(r.provenance["bundleMetadata"]);
      if (!why.isEmpty()) throw Invalid(why);
    }
    if (QJsonDocument(r.provenance).toJson().size() > 16384)
      throw Invalid("Provenance budget exceeded");
    r.decoded       = true;
    std::string why = validate(p);
    if (!why.empty()) throw Invalid(QString::fromStdString(why));
    r.semanticallySupported = true;
    return r;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    r.diagnostics << error;
    return r;
  }
}

QByteArray readBoundedRegularFile(const QString &path, qint64 maximumBytes,
                                  QString &error) {
  error.clear();
  if (!Detail::validateResponsePathInput(path, error)) return {};
  const QFileInfo info(path);
  Detail::RegularResponseDirectory directory;
  if (!directory.open(info.absolutePath(), error)) return {};
  return Detail::readResponseFile(directory, info.fileName(), maximumBytes,
                                  error);
}

namespace {
QString pixelHash(const Image &image) {
  return assetId(canonicalPixels(image), image.color);
}
std::vector<std::shared_ptr<const Image>> recordImages(const Record &record) {
  auto images = record.preset.tip.images;
  images.insert(images.end(), record.preset.secondary.tip.images.begin(),
                record.preset.secondary.tip.images.end());
  if (record.preset.texture.image)
    images.push_back(record.preset.texture.image);
  return images;
}
struct PortableData {
  QJsonObject root, package, manifest;
  PortableInfo info;
};
PortableData portableData(const QByteArray &bytes) {
  if (bytes.isEmpty() || bytes.size() > maxBytes)
    throw Invalid("Portable brush package exceeds 64 MiB");
  QJsonParseError parse;
  const auto doc = QJsonDocument::fromJson(bytes, &parse);
  if (parse.error != QJsonParseError::NoError || !doc.isObject())
    throw Invalid("Invalid portable brush JSON");
  PortableData data;
  data.root = doc.object();
  if (data.root.value("format") != "groundzero-raster-brush-portable")
    return PortableData{};
  keys(data.root, {"format", "version", "package", "resources"});
  if (number(data.root["version"]) != 1)
    throw Invalid("Unsupported portable brush version");
  data.package = object(data.root["package"]);
  if (text(data.package["format"]) != "groundzero-raster-brush" ||
      number(data.package["version"]) != 1)
    throw Invalid("Portable manifest requires a native version-1 package");
  data.manifest = object(data.root["resources"]);
  auto assets   = object(data.package["assets"]);
  if (data.manifest.size() > 512 || assets.size() > data.manifest.size())
    throw Invalid("Portable resource count exceeds limits");
  auto settings  = object(data.package["settings"]);
  data.info.name = text(settings["name"]);
  QSet<QString> references;
  auto tipReferences = [&](const QJsonValue &value) {
    const auto tip = object(value);
    for (const auto &reference : array(tip["images"], 256)) {
      const auto id = text(reference);
      if (id.isEmpty()) throw Invalid("Empty portable tip reference");
      references.insert(id);
    }
  };
  tipReferences(settings["tip"]);
  tipReferences(object(settings["secondary"])["tip"]);
  const auto texture = text(object(settings["texture"])["image"]);
  if (!texture.isEmpty()) references.insert(texture);
  if (references.size() != data.manifest.size())
    throw Invalid("Portable resource manifest does not match its settings");
  static const QRegularExpression identity("^[0-9a-f]{64}$");
  size_t pixels = 0;
  for (auto i = data.manifest.begin(); i != data.manifest.end(); ++i) {
    if (!identity.match(i.key()).hasMatch() || !references.contains(i.key()))
      throw Invalid("Invalid portable asset reference");
    const auto metadata = object(i.value());
    keys(metadata,
         {"hash", "width", "height", "color", "redistribution", "license"});
    ResourceInfo resource;
    resource.hash = text(metadata["hash"]);
    if (!identity.match(resource.hash).hasMatch())
      throw Invalid("Invalid portable pixel hash");
    const double width  = number(metadata["width"]),
                 height = number(metadata["height"]);
    if (width < 1 || width > 8192 || height < 1 || height > 8192 ||
        width != std::floor(width) || height != std::floor(height) ||
        width * height > 2 * 1024 * 1024)
      throw Invalid("Portable resource geometry exceeds limits");
    resource.width          = int(width);
    resource.height         = int(height);
    resource.color          = boolean(metadata["color"]);
    resource.redistribution = text(metadata["redistribution"]);
    resource.license        = text(metadata["license"]);
    validateRights(QJsonObject{
        {resource.hash, QJsonObject{{"redistribution", resource.redistribution},
                                    {"license", resource.license}}}});
    pixels += size_t(resource.width) * resource.height;
    if (pixels > size_t(256u * 1024u * 1024u) / sizeof(Pixel))
      throw Invalid("Portable decoded resource budget exceeded");
    resource.embedded = assets.contains(i.key());
    if (resource.embedded) {
      const auto asset = object(assets[i.key()]);
      keys(asset, {"png", "color"});
      if (!asset["png"].isString() || boolean(asset["color"]) != resource.color)
        throw Invalid("Conflicting portable resource metadata");
      if (assetId(QByteArray::fromBase64(asset["png"].toString().toLatin1()),
                  resource.color) != i.key()) {
        // A corrupted embedded copy is repairable from the declared pixel hash.
        resource.embedded = false;
        assets.remove(i.key());
      }
    }
    data.info.resources.push_back(resource);
  }
  for (auto i = assets.begin(); i != assets.end(); ++i)
    if (!data.manifest.contains(i.key()))
      throw Invalid("Unlisted portable resource");
  data.package["assets"] = assets;
  data.info.portable     = true;
  return data;
}
QByteArray portableBytes(const PortableData &data) {
  auto root         = data.root;
  root["package"]   = data.package;
  root["resources"] = data.manifest;
  const auto bytes  = QJsonDocument(root).toJson();
  if (bytes.size() > maxBytes)
    throw Invalid("Portable brush package exceeds 64 MiB");
  return bytes;
}
void replaceResourceReference(QJsonObject &package, const QString &oldId,
                              const QString &newId) {
  auto settings = package["settings"].toObject();
  auto tip      = [&](QJsonObject value) {
    auto refs = value["images"].toArray();
    for (int i = 0; i < refs.size(); ++i)
      if (refs[i].toString() == oldId) refs[i] = newId;
    value["images"] = refs;
    return value;
  };
  settings["tip"]       = tip(settings["tip"].toObject());
  auto secondary        = settings["secondary"].toObject();
  secondary["tip"]      = tip(secondary["tip"].toObject());
  settings["secondary"] = secondary;
  auto texture          = settings["texture"].toObject();
  if (texture["image"].toString() == oldId) texture["image"] = newId;
  settings["texture"] = texture;
  package["settings"] = settings;
}
void verifyPortablePixels(const PortableData &data, Record &record) {
  QMap<QString, std::shared_ptr<const Image>> images;
  const auto settings = data.package["settings"].toObject();
  auto bind           = [&](const QJsonObject &tip,
                  const std::vector<std::shared_ptr<const Image>> &values) {
    const auto refs = tip["images"].toArray();
    if (size_t(refs.size()) != values.size())
      throw Invalid("Portable tip references changed");
    for (int i = 0; i < refs.size(); ++i)
      images[refs[i].toString()] = values[size_t(i)];
  };
  bind(settings["tip"].toObject(), record.preset.tip.images);
  bind(settings["secondary"].toObject()["tip"].toObject(),
       record.preset.secondary.tip.images);
  if (record.preset.texture.image)
    images[settings["texture"].toObject()["image"].toString()] =
        record.preset.texture.image;
  record.resourceRights = {};
  for (auto i = data.manifest.begin(); i != data.manifest.end(); ++i) {
    const auto metadata = i.value().toObject();
    const auto image    = images.value(i.key());
    if (!image || pixelHash(*image) != metadata["hash"].toString() ||
        image->width != metadata["width"].toInt() ||
        image->height != metadata["height"].toInt() ||
        image->color != metadata["color"].toBool())
      throw Invalid("Portable resource does not match its declared pixel hash");
    record.resourceRights[metadata["hash"].toString()] =
        QJsonObject{{"redistribution", metadata["redistribution"]},
                    {"license", metadata["license"]}};
  }
}
}  // namespace

std::vector<ResourceInfo> resources(const Record &record, QString &error) {
  error.clear();
  try {
    const auto why = validate(record.preset);
    if (!why.empty()) throw Invalid(QString::fromStdString(why));
    validateRights(record.resourceRights);
    QMap<QString, ResourceInfo> unique;
    for (const auto &image : recordImages(record)) {
      ResourceInfo info;
      info.hash     = pixelHash(*image);
      info.width    = image->width;
      info.height   = image->height;
      info.color    = image->color;
      info.embedded = true;
      if (record.resourceRights.contains(info.hash)) {
        const auto rights   = record.resourceRights[info.hash].toObject();
        info.redistribution = rights["redistribution"].toString();
        info.license        = rights["license"].toString();
      }
      unique[info.hash] = info;
    }
    std::vector<ResourceInfo> result;
    for (auto i = unique.begin(); i != unique.end(); ++i)
      result.push_back(i.value());
    return result;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return {};
  }
}
PortableInfo inspectPortable(const QByteArray &bytes, QString &error) {
  error.clear();
  try {
    return portableData(bytes).info;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return {};
  }
}
QByteArray preparePortableRepair(const QByteArray &bytes, QString &error) {
  error.clear();
  try {
    auto data = portableData(bytes);
    if (!data.info.portable) throw Invalid("Not a portable brush manifest");
    data.package["assets"] = QJsonObject{};
    return portableBytes(data);
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return {};
  }
}
QByteArray encodePortable(const Record &record, ResourcePackaging mode,
                          QString &error) {
  error.clear();
  try {
    if (mode != ResourcePackaging::ManifestOnly &&
        mode != ResourcePackaging::PermittedOnly &&
        mode != ResourcePackaging::PrivateCopy)
      throw Invalid("Unknown resource packaging choice");
    Record exported = record;
    // Keep portable technical provenance, not source paths or arbitrary private
    // metadata.
    exported.provenance = {{"conversion",
                            "Native portable preset; source reference "
                            "verification is not established"},
                           {"sourceReferenceVerified", false},
                           {"sourceSemanticallySupported", false}};
    // A bounded classification is safe to carry; arbitrary source descriptors
    // (including aliases and private paths) remain excluded above.
    const QString conversionKind =
        record.provenance.value("conversionKind").toString();
    const bool nativeProfile = record.preset.profile == "groundzero-native-1";
    if (!nativeProfile)
      exported.provenance["conversionKind"] = "source-candidate";
    else if (conversionKind == "tip-only" ||
             conversionKind == "static-native" ||
             conversionKind == "partial-native" ||
             conversionKind == "native-reinterpretation")
      exported.provenance["conversionKind"] = conversionKind;
    const QString originalProfile =
        record.provenance.value("sourceProfileBeforeNativeConversion")
            .toString();
    const QStringList sourceProfiles{
        "krita-5.3.4.1-fixed-bitmap-mask-candidate-1",
        "krita-5.3.4.1-fixed-bitmap-candidate-1",
        "krita-5.3.4.1-hard-pixel-candidate-1",
        "krita-5.3.4.1-default-circle-candidate-1",
        "krita-5.3.4.1-default-rect-candidate-1",
        "krita-5.3.4.1-legacy-smear-candidate-1",
        "krita-5.3.4.1-legacy-dulling-candidate-1",
        "krita-5.3.4.1-new-smear-candidate-1",
        "krita-5.3.4.1-new-dulling-candidate-1"};
    if (nativeProfile && conversionKind == "native-reinterpretation" &&
        sourceProfiles.contains(originalProfile))
      exported.provenance["sourceProfileBeforeNativeConversion"] =
          originalProfile;
    static const QRegularExpression hash("^[0-9a-f]{64}$");
    for (const QString &key : {"sourceSha256", "bundleSha256"})
      if (hash.match(record.provenance.value(key).toString()).hasMatch())
        exported.provenance[key] = record.provenance[key];
    for (const QString &key : {"containerVersion", "containerSubversion"})
      if (record.provenance.value(key).isDouble())
        exported.provenance[key] = record.provenance[key];
    if (record.provenance.contains("bundleMetadata")) {
      const auto metadata = record.provenance["bundleMetadata"];
      const auto why      = Detail::bundleMetadataError(metadata);
      if (!why.isEmpty()) throw Invalid(why);
      exported.provenance["bundleMetadata"] = metadata;
    }
    exported.diagnostics.clear();
    // Diagnostics can contain arbitrary importer filenames. Do not attempt to
    // infer which free-form diagnostic text is safe to redistribute.
    static const QRegularExpression privatePath(
        R"path((?:^|[\s="'(,;:])[A-Za-z]:[\\/]|\\\\|(?i:file:)|(?:^|[\s="'(,;])(?:[/\\]|~[/\\]))path");
    QMap<QString, std::shared_ptr<const Image>> images;
    const auto native = encodeRecord(exported, error, {}, &images);
    if (!error.isEmpty()) return {};
    PortableData data;
    data.root    = {{"format", "groundzero-raster-brush-portable"},
                    {"version", 1}};
    data.package = QJsonDocument::fromJson(native).object();
    auto assets  = data.package["assets"].toObject();
    QJsonObject rightsInUse;
    for (auto i = images.begin(); i != images.end(); ++i) {
      const QString identity = pixelHash(*i.value());
      auto rights            = record.resourceRights.value(identity).toObject();
      const QString permission =
          rights.value("redistribution").toString("unknown");
      QString license = rights.value("license").toString();
      if (privatePath.match(license).hasMatch()) license.clear();
      data.manifest[i.key()] = QJsonObject{{"hash", identity},
                                           {"width", i.value()->width},
                                           {"height", i.value()->height},
                                           {"color", i.value()->color},
                                           {"redistribution", permission},
                                           {"license", license}};
      rightsInUse[identity] =
          QJsonObject{{"redistribution", permission}, {"license", license}};
      if (mode == ResourcePackaging::ManifestOnly ||
          (mode == ResourcePackaging::PermittedOnly &&
           permission != "permitted"))
        assets.remove(i.key());
    }
    data.package["assets"]         = assets;
    data.package["resourceRights"] = rightsInUse;
    return portableBytes(data);
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return {};
  }
}
QByteArray relinkPortable(const QByteArray &bytes,
                          const std::vector<Record> &copies,
                          QStringList &matchedHashes, QString &error) {
  error.clear();
  matchedHashes.clear();
  try {
    auto data = portableData(bytes);
    if (!data.info.portable) throw Invalid("Not a portable brush manifest");
    auto assets = data.package["assets"].toObject();
    for (const auto &copy : copies) {
      QMap<QString, std::shared_ptr<const Image>> images;
      const auto encoded = encodeRecord(copy, error, {}, &images);
      if (!error.isEmpty()) return {};
      const auto candidates =
          QJsonDocument::fromJson(encoded).object()["assets"].toObject();
      for (auto candidate = images.begin(); candidate != images.end();
           ++candidate) {
        const QString hash = pixelHash(*candidate.value());
        for (const QString &oldId : data.manifest.keys()) {
          const auto metadata = data.manifest[oldId].toObject();
          if (assets.contains(oldId) || metadata["hash"].toString() != hash)
            continue;
          if (metadata["width"].toInt() != candidate.value()->width ||
              metadata["height"].toInt() != candidate.value()->height ||
              metadata["color"].toBool() != candidate.value()->color)
            throw Invalid("Relink resource geometry disagrees with its hash");
          replaceResourceReference(data.package, oldId, candidate.key());
          data.manifest.remove(oldId);
          data.manifest[candidate.key()] = metadata;
          assets[candidate.key()]        = candidates[candidate.key()];
          if (!matchedHashes.contains(hash)) matchedHashes << hash;
        }
      }
    }
    data.package["assets"] = assets;
    return portableBytes(data);
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return {};
  }
}
bool savePortable(const QString &path, const Record &record,
                  ResourcePackaging mode, QString &error) {
  const auto bytes = encodePortable(record, mode, error);
  if (!error.isEmpty()) return false;
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    error = file.errorString();
    return false;
  }
  return true;
}

QByteArray encode(const Record &record, QString &error) {
  return encodeRecord(record, error, {});
}
Record decode(const QByteArray &bytes, QString &error,
              size_t maximumDecodedBytes) {
  try {
    const auto portable = portableData(bytes);
    if (!portable.info.portable)
      return decodeRecord(bytes, error, maximumDecodedBytes, {});
    for (const auto &resource : portable.info.resources)
      if (!resource.embedded)
        throw Invalid("Missing brush resource: " + resource.hash);
    auto record = decodeRecord(QJsonDocument(portable.package).toJson(), error,
                               maximumDecodedBytes, {});
    if (!error.isEmpty()) return {};
    verifyPortablePixels(portable, record);
    return record;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return {};
  }
}
QByteArray encodeWorkerRecord(const Record &record, const QString &directory,
                              QString &error) {
  if (directory.isEmpty()) {
    error = "Missing worker resource directory";
    return {};
  }
  return encodeRecord(record, error, directory);
}
Record decodeWorkerRecord(const QByteArray &bytes, const QString &directory,
                          QString &error, size_t maximumDecodedBytes) {
  if (directory.isEmpty()) {
    error = "Missing worker resource directory";
    return {};
  }
  return decodeRecord(bytes, error, maximumDecodedBytes, directory);
}

Record decodeWorkerRecordInDirectory(
    const QByteArray &bytes, const Detail::RegularResponseDirectory &directory,
    QString &error, size_t maximumDecodedBytes) {
  // A nonempty tag selects the private wire format; all resources use the
  // already-anchored directory rather than looking up this tag as a path.
  return decodeRecord(bytes, error, maximumDecodedBytes, "worker", &directory);
}

Record load(const QString &path, QString &error, size_t maximumDecodedBytes) {
  const auto bytes = readBoundedRegularFile(path, maxBytes, error);
  if (!error.isEmpty()) return {};
  return decode(bytes, error, maximumDecodedBytes);
}
bool save(const QString &path, const Record &r, QString &error) {
  auto bytes = encode(r, error);
  if (!error.isEmpty()) return false;
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() ||
      !f.commit()) {
    error = f.errorString();
    return false;
  }
  return true;
}
QString install(const QString &directory, const Record &r, QString &error) {
  auto bytes = encode(r, error);
  if (!error.isEmpty()) return {};
  QString id = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  QString name = QString::fromStdString(r.preset.name);
  for (int i = 0; i < name.size(); ++i)
    if (!name[i].isLetterOrNumber() && name[i] != '-' && name[i] != '_')
      name[i] = '_';
  name = name.left(40);
  if (name.isEmpty()) name = "brush";
  if (!QDir().mkpath(directory)) {
    error = "Cannot create brush library";
    return {};
  }
  QString path = QDir(directory).filePath(name + "-" + id + ".gzbrush");
  const QFileInfo existing(path);
  if (existing.exists() || existing.isSymLink()) {
    const auto saved = readBoundedRegularFile(path, maxBytes, error);
    if (!error.isEmpty() || saved != bytes) {
      error = "Existing brush resource conflicts with its identity";
      return {};
    }
    return path;
  }
  if (!save(path, r, error)) return {};
  return path;
}
QStringList installCollection(const QString &directory,
                              const std::vector<Record> &records,
                              QString &error,
                              const std::atomic_bool *cancelled) {
  error.clear();
  auto stopped = [&]() {
    return cancelled && cancelled->load(std::memory_order_relaxed);
  };
  if (records.empty() || records.size() > 256) {
    error = "A brush collection must contain 1 to 256 presets";
    return {};
  }
  if (!QDir().mkpath(directory)) {
    error = "Cannot create brush library";
    return {};
  }
  QTemporaryDir stage(QDir(directory).filePath(".brush-import-XXXXXX"));
  if (!stage.isValid()) {
    error = "Cannot stage brush collection";
    return {};
  }
  QStringList names;
  QCryptographicHash identity(QCryptographicHash::Sha256);
  qint64 total = 0;
  for (const auto &record : records) {
    if (stopped()) {
      error = "Brush import cancelled";
      return {};
    }
    auto bytes = encode(record, error);
    if (!error.isEmpty()) return {};
    total += bytes.size();
    if (total > 256 * 1024 * 1024) {
      error = "Brush collection package budget exceeded";
      return {};
    }
    auto hash =
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    identity.addData(hash);
    QString label = QString::fromStdString(record.preset.name).left(40);
    for (int i = 0; i < label.size(); ++i)
      if (!label[i].isLetterOrNumber() && label[i] != '-' && label[i] != '_')
        label[i] = '_';
    if (label.isEmpty()) label = "brush";
    QString name = label + "-" + QString::fromLatin1(hash) + ".gzbrush";
    if (names.contains(name)) continue;
    QFile file(QDir(stage.path()).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(bytes) != bytes.size() || !file.flush()) {
      error = "Cannot write staged brush collection: " + file.errorString();
      return {};
    }
    file.close();
    names << name;
  }
  if (stopped()) {
    error = "Brush import cancelled";
    return {};
  }
  QString destination = QDir(directory).filePath(
      "pack-" + QString::fromLatin1(identity.result().toHex()));
  if (QFileInfo(destination).isSymLink()) {
    error = "Brush collection destination cannot be a symbolic link";
    return {};
  }
  if (!QFileInfo::exists(destination)) {
    if (!QDir().rename(stage.path(), destination)) {
      error = "Cannot atomically publish brush collection";
      return {};
    }
    stage.setAutoRemove(false);
  } else {
    Detail::RegularResponseDirectory savedDirectory, stagedDirectory;
    if (!savedDirectory.open(destination, error) ||
        !stagedDirectory.open(stage.path(), error))
      return {};
    QDir existing(destination);
    auto actual = existing.entryList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
        QDir::Name);
    QStringList expected = names;
    expected.sort();
    if (actual != expected) {
      error = "Existing brush collection conflicts with its identity";
      return {};
    }
    for (const auto &name : names) {
      if (stopped()) {
        error = "Brush import cancelled";
        return {};
      }
      const auto staged =
          Detail::readResponseFile(stagedDirectory, name, maxBytes, error);
      if (!error.isEmpty()) return {};
      const auto saved =
          Detail::readResponseFile(savedDirectory, name, maxBytes, error);
      if (!error.isEmpty() || saved != staged) {
        error = "Existing brush collection content conflicts with its identity";
        return {};
      }
    }
  }
  QStringList installed;
  for (const auto &name : names) installed << QDir(destination).filePath(name);
  return installed;
}
}  // namespace RasterBrush
