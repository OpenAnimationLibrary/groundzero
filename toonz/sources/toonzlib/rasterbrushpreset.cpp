#include "toonz/rasterbrushpreset.h"
#include "rasterbrushpng.h"
#include <QColorSpace>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
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
const QStringList sensors = {
    "pressure",       "tilt_x",   "tilt_y",     "tilt_elevation",
    "tilt_direction", "rotation", "direction",  "distance",
    "time",           "speed",    "random_dab", "random_stroke"};
const QStringList shapes = {"round", "square", "gaussian", "bitmap"};
const QStringList blends = {"normal", "multiply", "screen", "overlay", "erase"};
const QStringList masks  = {"multiply", "subtract"};
const QStringList wets   = {"off", "dulling", "smear"};
const QStringList interpolations = {"linear", "natural-cubic"};
QJsonObject dynamicJson(const Dynamic &d) {
  QJsonArray maps;
  for (const auto &m : d.mappings) {
    QJsonArray curve;
    for (auto k : m.curve) curve.append(QJsonArray{k.x, k.y});
    maps.append(
        QJsonObject{{"sensor", sensors[int(m.sensor)]},
                    {"scale", m.scale},
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
    keys(a, {"sensor", "scale", "curve", "interpolation"});
    Mapping m;
    m.sensor = Sensor(choice(a["sensor"], sensors));
    m.scale  = number(a["scale"]);
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
  return QString::fromLatin1(
      QCryptographicHash::hash(png + (color ? "color" : "mask"),
                               QCryptographicHash::Sha256)
          .toHex());
}
struct Writer {
  QJsonObject assets;
  QString image(const std::shared_ptr<const Image> &im) {
    if (!im) return {};
    QByteArray png;
    QBuffer b(&png);
    b.open(QIODevice::WriteOnly);
    if (!toImage(*im).save(&b, "PNG"))
      throw Invalid("Cannot encode brush image");
    QString id = assetId(png, im->color);
    assets[id] = QJsonObject{{"png", QString::fromLatin1(png.toBase64())},
                             {"color", im->color}};
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

QByteArray encode(const Record &r, QString &error) {
  error.clear();
  try {
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
    const auto &p = r.preset;
    const auto &t = p.texture;
    const auto &s = p.secondary;
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
        {"spacing", p.spacing},
        {"airbrushRate", p.airbrushRate},
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
        {"pickupControl", dynamicJson(p.pickupControl)},
        {"colorRateControl", dynamicJson(p.colorRateControl)},
        {"radiusControl", dynamicJson(p.radiusControl)},
        {"colorRate", p.colorRate},
        {"reload", p.reload},
        {"smudgeRadius", p.smudgeRadius},
        {"seed", QString::number(p.seed)}};
    settings["texture"] =
        QJsonObject{{"image", w.image(t.image)},  {"mode", masks[int(t.mode)]},
                    {"scale", t.scale},           {"angle", t.angle},
                    {"offsetX", t.offsetX},       {"offsetY", t.offsetY},
                    {"depth", t.depth},           {"contrast", t.contrast},
                    {"brightness", t.brightness}, {"invert", t.invert},
                    {"perDab", t.perDab}};
    settings["secondary"] = QJsonObject{{"enabled", s.enabled},
                                        {"tip", w.tip(s.tip)},
                                        {"mode", masks[int(s.mode)]},
                                        {"size", dynamicJson(s.size)},
                                        {"angle", dynamicJson(s.angle)},
                                        {"opacity", dynamicJson(s.opacity)},
                                        {"aspect", s.aspect},
                                        {"scatter", s.scatter}};
    if (w.assets.size() > 512) throw Invalid("Too many resources");
    QByteArray result =
        QJsonDocument(QJsonObject{{"format", "groundzero-raster-brush"},
                                  {"version", 1},
                                  {"settings", settings},
                                  {"assets", w.assets},
                                  {"provenance", r.provenance},
                                  {"diagnostics",
                                   QJsonArray::fromStringList(r.diagnostics)}})
            .toJson();
    if (result.size() > maxBytes) throw Invalid("Brush package exceeds 64 MiB");
    return result;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return {};
  }
}

Record decode(const QByteArray &bytes, QString &error,
              size_t maximumDecodedBytes) {
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
                "diagnostics"});
    if (text(root["format"]) != "groundzero-raster-brush" ||
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
      keys(a, {"png", "color"});
      if (!a["png"].isString()) throw Invalid("Invalid image encoding");
      auto png   = QByteArray::fromBase64(a["png"].toString().toLatin1());
      bool color = boolean(a["color"]);
      if (assetId(png, color) != i.key())
        throw Invalid("Brush resource hash mismatch");
      QString why;
      auto im = decodeImage(png, color, why, maximumDecodedBytes - total);
      if (!im) throw Invalid(why);
      total += im->pixels.size() * sizeof(Pixel);
      if (total > maximumDecodedBytes)
        throw Invalid("Aggregate resource budget exceeded");
      rd.assets[i.key()] = im;
    }
    auto o = object(root["settings"]);
    keys(o, {"name",          "profile",    "tip",           "size",
             "aspect",        "angle",      "opacity",       "flow",
             "scatter",       "spacing",    "airbrushRate",  "count",
             "wash",          "lockAlpha",  "flipX",         "flipY",
             "blend",         "color",      "wet",           "pickup",
             "colorRate",     "reload",     "smudgeRadius",  "seed",
             "texture",       "secondary",  "pickupControl", "colorRateControl",
             "radiusControl", "background", "backgroundMix", "hue",
             "saturation",    "brightness"});
    auto &p        = r.preset;
    p.name         = text(o["name"]).toStdString();
    p.profile      = text(o["profile"]).toStdString();
    p.tip          = rd.tip(o["tip"]);
    p.size         = dynamic(o["size"]);
    p.aspect       = dynamic(o["aspect"]);
    p.angle        = dynamic(o["angle"]);
    p.opacity      = dynamic(o["opacity"]);
    p.flow         = dynamic(o["flow"]);
    p.scatter      = dynamic(o["scatter"]);
    p.spacing      = number(o["spacing"]);
    p.airbrushRate = number(o["airbrushRate"]);
    double count   = number(o["count"]);
    if (count < 1 || count > 64 || count != std::floor(count))
      throw Invalid("Invalid dab count");
    p.count     = int(count);
    p.wash      = boolean(o["wash"]);
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
    bool ok = false;
    p.seed  = text(o["seed"]).toULongLong(&ok);
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
             "scatter"});
    p.secondary.enabled = boolean(s["enabled"]);
    p.secondary.tip     = rd.tip(s["tip"]);
    p.secondary.mode    = MaskMode(choice(s["mode"], masks));
    p.secondary.size    = dynamic(s["size"]);
    p.secondary.angle   = dynamic(s["angle"]);
    p.secondary.opacity = dynamic(s["opacity"]);
    p.secondary.aspect  = number(s["aspect"]);
    p.secondary.scatter = number(s["scatter"]);
    r.provenance        = object(root["provenance"]);
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

Record load(const QString &path, QString &error, size_t maximumDecodedBytes) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    error = f.errorString();
    return {};
  }
  if (f.size() > maxBytes) {
    error = "Brush package exceeds 64 MiB";
    return {};
  }
  QByteArray bytes = f.read(maxBytes + 1);
  if (!f.atEnd() || f.error() != QFile::NoError) {
    error = "Cannot read brush package";
    return {};
  }
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
  if (QFile::exists(path)) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.read(maxBytes + 1) != bytes) {
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
      QFile staged(QDir(stage.path()).filePath(name)),
          saved(existing.filePath(name));
      if (QFileInfo(saved).isSymLink() || !staged.open(QIODevice::ReadOnly) ||
          !saved.open(QIODevice::ReadOnly) || saved.size() != staged.size() ||
          saved.read(maxBytes + 1) != staged.read(maxBytes + 1)) {
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
