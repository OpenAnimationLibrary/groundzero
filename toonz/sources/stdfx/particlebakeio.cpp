#include "toonz/particlebake.h"
#include "toonz/particlecache.h"
#include "texception.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QSaveFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace ParticleBake {
namespace {
constexpr quint64 maxSpriteBytes = 512ULL * 1024 * 1024;
constexpr quint64 spriteCacheBytes = 128ULL * 1024 * 1024;
constexpr int maxDimension = 16384;
constexpr int maxDraws = 1000000;

bool cancelled(const Cancel &cancel) { return cancel && cancel(); }
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

struct RasterLock {
  TRasterP raster;
  explicit RasterLock(const TRasterP &value) : raster(value) { raster->lock(); }
  ~RasterLock() { raster->unlock(); }
};

QJsonArray affineJson(const TAffine &a) {
  return {a.a11, a.a12, a.a13, a.a21, a.a22, a.a23};
}
double number(const QJsonObject &o, const char *key) {
  require(o.value(key).isDouble(), "Invalid native particle numeric field");
  double v = o.value(key).toDouble();
  require(std::isfinite(v), "Non-finite native particle value");
  return v;
}
int integer(const QJsonObject &o, const char *key) {
  double v = number(o, key);
  require(v == std::floor(v) && v >= std::numeric_limits<int>::min() &&
              v <= std::numeric_limits<int>::max(), "Invalid particle integer");
  return int(v);
}
TAffine readAffine(const QJsonValue &value) {
  QJsonArray a = value.toArray();
  require(a.size() == 6, "Invalid native particle affine");
  for (const auto &v : a)
    require(v.isDouble() && std::isfinite(v.toDouble()),
            "Invalid native particle affine value");
  return TAffine(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(),
                 a[3].toDouble(), a[4].toDouble(), a[5].toDouble());
}
QJsonArray pointJson(const TPointD &p) { return {p.x, p.y}; }
TPointD readPoint(const QJsonValue &value) {
  auto a = value.toArray();
  require(a.size() == 2 && a[0].isDouble() && a[1].isDouble() &&
              std::isfinite(a[0].toDouble()) && std::isfinite(a[1].toDouble()),
          "Invalid native particle position");
  return TPointD(a[0].toDouble(), a[1].toDouble());
}

// A versioned, lossless RGBA sprite representation. Explicit channel order and
// byte order make it portable; never serialize native TPixel struct padding.
QByteArray encodeSprite(const TRasterP &raster) {
  require(bool(raster), "Missing captured particle artwork");
  const int w = raster->getLx(), h = raster->getLy();
  TRaster32P r32 = raster;
  TRaster64P r64 = raster;
  TRasterFP rf = raster;
  const int depth = r32 ? 32 : r64 ? 64 : rf ? 128 : 0;
  const quint64 bytes = quint64(w) * quint64(h) * quint64(depth / 8);
  require(depth && w > 0 && h > 0 && w <= maxDimension && h <= maxDimension &&
              bytes <= maxSpriteBytes, "Unsupported or oversized particle artwork");
  QByteArray data;
  data.reserve(int(bytes) + 32);
  QDataStream out(&data, QIODevice::WriteOnly);
  out.setVersion(QDataStream::Qt_5_12);
  out.setByteOrder(QDataStream::LittleEndian);
  out.setFloatingPointPrecision(QDataStream::SinglePrecision);
  out << quint32(0x5350544f) << quint32(1) << qint32(w) << qint32(h)
      << qint32(depth) << quint8(raster->isLinear() ? 1 : 0);
  RasterLock lock(raster);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (r32) {
        const auto &p = r32->pixels(y)[x];
        out << quint8(p.r) << quint8(p.g) << quint8(p.b) << quint8(p.m);
      } else if (r64) {
        const auto &p = r64->pixels(y)[x];
        out << quint16(p.r) << quint16(p.g) << quint16(p.b) << quint16(p.m);
      } else {
        const auto &p = rf->pixels(y)[x];
        require(std::isfinite(p.r) && std::isfinite(p.g) &&
                    std::isfinite(p.b) && std::isfinite(p.m),
                "Non-finite particle artwork pixel");
        out << p.r << p.g << p.b << p.m;
      }
    }
  require(out.status() == QDataStream::Ok, "Unable to encode particle artwork");
  return data;
}

TRasterP decodeSprite(const QByteArray &data) {
  QDataStream in(data);
  in.setVersion(QDataStream::Qt_5_12);
  in.setByteOrder(QDataStream::LittleEndian);
  in.setFloatingPointPrecision(QDataStream::SinglePrecision);
  quint32 magic, version;
  qint32 w, h, depth;
  quint8 linear;
  in >> magic >> version >> w >> h >> depth >> linear;
  require(in.status() == QDataStream::Ok && magic == 0x5350544f && version == 1 &&
              w > 0 && h > 0 && w <= maxDimension && h <= maxDimension &&
              (depth == 32 || depth == 64 || depth == 128) && linear <= 1,
          "Invalid particle artwork header");
  const quint64 size = quint64(w) * quint64(h) * quint64(depth / 8);
  require(size <= maxSpriteBytes && quint64(data.size()) == size + 21,
          "Truncated or oversized particle artwork");
  TRasterP raster;
  if (depth == 32) raster = TRaster32P(w, h);
  else if (depth == 64) raster = TRaster64P(w, h);
  else raster = TRasterFP(w, h);
  RasterLock lock(raster);
  TRaster32P r32 = raster;
  TRaster64P r64 = raster;
  TRasterFP rf = raster;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (r32) {
        quint8 r, g, b, a;
        in >> r >> g >> b >> a;
        r32->pixels(y)[x] = TPixel32(r, g, b, a);
      } else if (r64) {
        quint16 r, g, b, a;
        in >> r >> g >> b >> a;
        r64->pixels(y)[x] = TPixel64(r, g, b, a);
      } else {
        float r, g, b, a;
        in >> r >> g >> b >> a;
        require(std::isfinite(r) && std::isfinite(g) && std::isfinite(b) &&
                    std::isfinite(a), "Non-finite particle artwork pixel");
        rf->pixels(y)[x] = TPixelF(r, g, b, a);
      }
    }
  require(in.status() == QDataStream::Ok && in.atEnd(), "Invalid particle artwork data");
  raster->setLinear(bool(linear));
  return raster;
}

void saveBytes(const QString &path, const QByteArray &data) {
  QSaveFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(data) == data.size() &&
              file.commit(), "Unable to write particle cache asset");
}

TRasterP loadSprite(const QString &directory, const QString &relative) {
  require(!relative.isEmpty() && !QDir::isAbsolutePath(relative),
          "Particle artwork must be package-relative");
  QString root = QFileInfo(directory).canonicalFilePath();
  QFileInfo info(QDir(directory).filePath(relative));
  QString path = info.canonicalFilePath();
  require(!root.isEmpty() && path.startsWith(root + '/') && info.isFile(),
          "Missing particle artwork or path outside cache package");
  require(info.size() >= 21 && quint64(info.size()) <= maxSpriteBytes + 21,
          "Oversized particle artwork file");
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Unable to open particle artwork");
  QByteArray data = file.read(maxSpriteBytes + 22);
  require(data.size() == info.size() && file.atEnd(), "Unable to read particle artwork");
  const QByteArray digest = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
  require(info.completeBaseName().toLatin1() == digest,
          "Particle artwork checksum mismatch");
  struct Entry { QString revision; TRasterP raster; quint64 bytes, serial; };
  static QMutex mutex;
  static std::map<QString, Entry> cache;
  static quint64 used = 0, serial = 0;
  QMutexLocker guard(&mutex);
  const QString revision = QString::fromLatin1(digest);
  auto found = cache.find(path);
  if (found != cache.end()) {
    if (found->second.revision == revision) {
      found->second.serial = ++serial;
      return found->second.raster;
    }
    used -= found->second.bytes;
    cache.erase(found);
  }
  TRasterP raster = decodeSprite(data);
  quint64 bytes = quint64(data.size());
  while (!cache.empty() && used + bytes > spriteCacheBytes) {
    auto oldest = std::min_element(cache.begin(), cache.end(),
        [](const auto &a, const auto &b) { return a.second.serial < b.second.serial; });
    used -= oldest->second.bytes;
    cache.erase(oldest);
  }
  if (bytes <= spriteCacheBytes) {
    cache.emplace(path, Entry{revision, raster, bytes, ++serial});
    used += bytes;
  }
  return raster;
}

QJsonObject nativeMetadata(const ParticleCache::Snapshot &snapshot) {
  if (!snapshot.extra.contains("drawFile")) return snapshot.extra;
  const QString relative = snapshot.extra.value("drawFile").toString();
  require(!relative.isEmpty() && !QDir::isAbsolutePath(relative),
          "Native draw metadata must be package-relative");
  const QString root = QFileInfo(snapshot.sourceDirectory).canonicalFilePath();
  QFileInfo info(QDir(snapshot.sourceDirectory).filePath(relative));
  require(!root.isEmpty() && info.canonicalFilePath().startsWith(root + '/') &&
              info.isFile() && info.size() <= 64 * 1024 * 1024,
          "Invalid or oversized native draw metadata file");
  QFile file(info.canonicalFilePath());
  require(file.open(QIODevice::ReadOnly), "Cannot read native draw metadata");
  const QByteArray data = file.read(64 * 1024 * 1024 + 1);
  require(data.size() <= 64 * 1024 * 1024 && file.atEnd(),
          "Oversized native draw metadata");
  QJsonParseError parse;
  QJsonDocument document = QJsonDocument::fromJson(data, &parse);
  require(parse.error == QJsonParseError::NoError && document.isObject(),
          "Malformed native draw metadata");
  return document.object();
}

QJsonObject drawJson(const DrawSample &d, const QString &file) {
  QJsonArray history;
  for (const auto &p : d.history) history.append(pointJson(p));
  return {{"id", QString::number(d.id)}, {"artwork", file},
          {"affine", affineJson(d.rasterToWorld)},
          {"position", pointJson(d.position)}, {"velocity", pointJson(d.velocity)},
          {"age", d.age}, {"lifetime", d.lifetime}, {"source", d.source},
          {"artworkFrame", d.artworkFrame}, {"trailDistance", d.trailDistance},
          {"scale", d.scale}, {"angle", d.angle}, {"opacity", d.opacity},
          {"tintMix", d.tintMix},
          {"tint", QJsonArray{int(d.tint.r), int(d.tint.g), int(d.tint.b), int(d.tint.m)}},
          {"history", history}, {"background", d.background},
          {"motionBlur", d.motionBlur}, {"blurGamma", d.blurGamma},
          {"trailOpacityStart", d.trailOpacityStart},
          {"trailOpacityEnd", d.trailOpacityEnd}};
}
}  // namespace

bool loadFrame(const ParticleCache::Snapshot &snapshot, Frame &result,
               QString &error, const Cancel &cancel) {
  result = Frame();
  error.clear();
  try {
    require(!cancelled(cancel), "Particle cache loading cancelled");
    const auto o = nativeMetadata(snapshot);
    require(o.value("native").toBool() && o.value("draws").isArray(),
            "Not a native particle capture");
    Frame frame;
    frame.frame = number(o, "sceneFrame");
    frame.bitsPerPixel = integer(o, "bitsPerPixel");
    require(frame.bitsPerPixel == 32 || frame.bitsPerPixel == 64 ||
                frame.bitsPerPixel == 128, "Invalid particle capture precision");
    frame.linearColorSpace = o.value("linear").toBool();
    frame.colorSpaceGamma = number(o, "gamma");
    require(frame.colorSpaceGamma > 0, "Invalid particle capture gamma");
    frame.renderAffine = readAffine(o.value("renderAffine"));
    auto domain = o.value("domain").toArray();
    require(domain.size() == 4, "Invalid particle capture domain");
    for (const auto &v : domain)
      require(v.isDouble() && std::isfinite(v.toDouble()), "Invalid capture domain");
    frame.domain = TRectD(domain[0].toDouble(), domain[1].toDouble(),
                         domain[2].toDouble(), domain[3].toDouble());
    require(!frame.domain.isEmpty(), "Empty particle capture domain");
    const double fps = number(o, "fps");
    require(fps > 0 && snapshot.particles.size() <= maxDraws,
            "Invalid native particle timing or count");
    auto attribute = [&](const char *name, std::size_t row, int component,
                         double fallback) {
      for (const auto &a : snapshot.attributes) {
        if (a.name != QLatin1String(name) || a.fixed) continue;
        require((a.type == ParticleCache::AttributeType::Float ||
                 a.type == ParticleCache::AttributeType::Vector) &&
                    component < a.components,
                "Invalid native particle attribute");
        const std::size_t offset = row * std::size_t(a.components) + component;
        require(offset < a.floats.size() && std::isfinite(a.floats[offset]),
                "Truncated native particle attribute");
        return double(a.floats[offset]);
      }
      return fallback;
    };
    for (std::size_t i = 0; i < snapshot.particles.size(); ++i) {
      const auto &p = snapshot.particles[i];
      require(p.id >= 0, "Invalid native particle identity");
      DrawSample state;
      state.id = std::uint64_t(p.id);
      state.position = TPointD(p.position[0], p.position[1]);
      state.velocity = TPointD(p.velocity[0], p.velocity[1]);
      state.age = p.age * fps; state.lifetime = p.lifetime * fps;
      state.source = p.artworkIndex;
      require(std::isfinite(p.artworkFrame) && p.artworkFrame >= std::numeric_limits<int>::min() &&
                  p.artworkFrame <= std::numeric_limits<int>::max(), "Invalid native drawing frame");
      state.artworkFrame = int(p.artworkFrame);
      state.scale = attribute("ot_scale", i, 0, p.size);
      state.opacity = attribute("ot_opacity", i, 0, p.tint[3]);
      state.angle = p.rotation;
      state.tintMix = attribute("ot_tintMix", i, 0, 0);
      int rgba[4];
      for (int c = 0; c < 4; ++c)
        rgba[c] = int(std::clamp(attribute("ot_tintRGBA", i, c, p.tint[c]), 0.0, 1.0) * 255 + .5);
      state.tint = TPixel32(rgba[0], rgba[1], rgba[2], rgba[3]);
      for (int h = 0; h < 3; ++h)
        state.history[h] = TPointD(attribute("ot_history", i, h * 2, p.position[0]),
                                   attribute("ot_history", i, h * 2 + 1, p.position[1]));
      const TAffine fallback = TTranslation(state.position) * TRotation(state.angle) * TScale(state.scale);
      state.rasterToWorld = TAffine(attribute("ot_affine", i, 0, fallback.a11),
          attribute("ot_affine", i, 1, fallback.a12), attribute("ot_affine", i, 2, fallback.a13),
          attribute("ot_affine", i, 3, fallback.a21), attribute("ot_affine", i, 4, fallback.a22),
          attribute("ot_affine", i, 5, fallback.a23));
      frame.particles.push_back(state);
    }
    auto draws = o.value("draws").toArray();
    require(draws.size() <= maxDraws, "Too many native particle draws");
    std::map<QString, TRasterP> assets;
    quint64 bytes = 0;
    for (const auto &value : draws) {
      require(!cancelled(cancel), "Particle cache loading cancelled");
      require(value.isObject(), "Invalid native particle draw");
      const auto d = value.toObject();
      DrawSample sample;
      bool validId = false;
      sample.id = d.value("id").toString().toULongLong(&validId);
      require(validId, "Invalid native particle identity");
      const QString asset = d.value("artwork").toString();
      auto held = assets.find(asset);
      if (held == assets.end()) {
        TRasterP raster = loadSprite(snapshot.sourceDirectory, asset);
        bytes += quint64(raster->getLx()) * raster->getLy() * raster->getPixelSize();
        require(bytes <= maxSpriteBytes, "Native particle frame exceeds memory limit");
        held = assets.emplace(asset, raster).first;
      }
      sample.artwork = held->second;
      sample.rasterToWorld = readAffine(d.value("affine"));
      sample.position = readPoint(d.value("position"));
      sample.velocity = readPoint(d.value("velocity"));
      sample.age = number(d, "age"); sample.lifetime = number(d, "lifetime");
      sample.source = integer(d, "source");
      sample.artworkFrame = integer(d, "artworkFrame");
      sample.trailDistance = integer(d, "trailDistance");
      sample.scale = number(d, "scale"); sample.angle = number(d, "angle");
      sample.opacity = number(d, "opacity"); sample.tintMix = number(d, "tintMix");
      sample.background = d.value("background").toBool();
      sample.motionBlur = d.value("motionBlur").toBool();
      sample.blurGamma = number(d, "blurGamma");
      sample.trailOpacityStart = number(d, "trailOpacityStart");
      sample.trailOpacityEnd = number(d, "trailOpacityEnd");
      auto tint = d.value("tint").toArray();
      require(tint.size() == 4, "Invalid particle tint");
      for (const auto &v : tint)
        require(v.isDouble() && v.toDouble() >= 0 && v.toDouble() <= 255 &&
                    v.toDouble() == v.toInt(), "Invalid particle tint component");
      sample.tint = TPixel32(tint[0].toInt(), tint[1].toInt(),
                            tint[2].toInt(), tint[3].toInt());
      auto history = d.value("history").toArray();
      require(history.size() == 3, "Invalid particle history");
      for (int i = 0; i < 3; ++i) sample.history[i] = readPoint(history[i]);
      frame.draws.push_back(std::move(sample));
    }
    require(!cancelled(cancel), "Particle cache loading cancelled");
    result = std::move(frame);
    return true;
  } catch (const TException &e) {
    error = QString::fromStdWString(e.getMessage());
    return false;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return false;
  } catch (...) {
    error = "Unable to load native particle frame";
    return false;
  }
}

QString frameRevision(const ParticleCache::Snapshot &snapshot, QString &error) {
  error.clear();
  try {
    const QJsonObject metadata = nativeMetadata(snapshot);
    require(metadata.value("native").toBool() && metadata.value("draws").isArray(),
            "Invalid native particle metadata");
    const QJsonArray draws = metadata.value("draws").toArray();
    require(draws.size() <= maxDraws, "Too many native particle draws");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    std::map<QString, bool> visited;
    const QString root = QFileInfo(snapshot.sourceDirectory).canonicalFilePath();
    require(!root.isEmpty(), "Missing native particle package");
    for (const auto &value : draws) {
      require(value.isObject(), "Invalid native particle draw");
      const QString relative = value.toObject().value("artwork").toString();
      if (!visited.emplace(relative, true).second) continue;
      require(!relative.isEmpty() && !QDir::isAbsolutePath(relative),
              "Particle artwork must be package-relative");
      QFileInfo info(QDir(root).filePath(relative));
      require(info.isFile() && info.canonicalFilePath().startsWith(root + '/') &&
                  info.size() >= 21 && quint64(info.size()) <= maxSpriteBytes + 21,
              "Invalid native particle artwork asset");
      QFile file(info.canonicalFilePath());
      require(file.open(QIODevice::ReadOnly), "Cannot read native particle artwork");
      QCryptographicHash content(QCryptographicHash::Sha256);
      qint64 total = 0;
      while (!file.atEnd()) {
        const QByteArray bytes = file.read(256 * 1024);
        require(!bytes.isEmpty(), "Unable to read native particle artwork");
        total += bytes.size();
        require(quint64(total) <= maxSpriteBytes + 21, "Oversized particle artwork");
        content.addData(bytes);
      }
      const QByteArray digest = content.result().toHex();
      require(total == info.size() && digest == info.completeBaseName().toLatin1(),
              "Particle artwork checksum mismatch");
      hash.addData(digest);
    }
    return QString::fromLatin1(hash.result().toHex());
  } catch (const TException &e) {
    error = QString::fromStdWString(e.getMessage());
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
  } catch (...) {
    error = "Unable to identify native particle frame";
  }
  return {};
}

bool writePackage(const QString &directory, const ParticleCache::Metadata &metadata,
                  const FrameProvider &provider, QString &error,
                  const Cancel &cancel, const ParticleCache::Progress &progress) {
  error.clear();
  try {
    require(bool(provider) && metadata.firstFrame <= metadata.lastFrame &&
                qint64(metadata.lastFrame) - metadata.firstFrame < 100000 &&
                std::isfinite(metadata.framesPerSecond) && metadata.framesPerSecond > 0,
            "Invalid particle bake range or frame rate");
    QFileInfo destination(QDir::cleanPath(QFileInfo(directory).absoluteFilePath()));
    require(!destination.exists() && !destination.isSymLink(),
            "Particle cache destination already exists");
    QDir parent = destination.dir();
    require(parent.exists() && !destination.fileName().isEmpty(),
            "Particle cache parent directory does not exist");
    QTemporaryDir temporary(parent.filePath(".particle-bake-XXXXXX"));
    require(temporary.isValid(), "Unable to create particle bake staging directory");
    QDir stage(temporary.path());
    require(stage.mkdir("frames") && stage.mkdir("artwork"),
            "Unable to create particle bake directories");
    QJsonArray frames, artwork;
    std::map<QString, bool> writtenAssets;
    const int count = metadata.lastFrame - metadata.firstFrame + 1;
    for (int index = 0; index < count; ++index) {
      require(!cancelled(cancel), "Particle bake cancelled");
      const int number = metadata.firstFrame + index;
      Frame captured;
      if (!provider(number, captured, error)) {
        if (error.isEmpty()) error = "Particle frame capture failed";
        return false;
      }
      require(captured.draws.size() <= maxDraws, "Too many particle draws");
      QJsonArray draws;
      ParticleCache::Snapshot snapshot;
      snapshot.hasStableIds = true;
      snapshot.hasVelocity = true;
      snapshot.hasArtworkFrame = true;
      std::map<std::uint64_t, bool> births;
      quint64 frameBytes = 0;
      std::map<QString, bool> frameAssets;
      for (const auto &draw : captured.draws) {
        require(!cancelled(cancel), "Particle bake cancelled");
        QByteArray bytes = encodeSprite(draw.artwork);
        QString asset = "artwork/" + QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) + ".otsprite";
        if (frameAssets.emplace(asset, true).second) {
          frameBytes += quint64(bytes.size());
          require(frameBytes <= maxSpriteBytes, "Native particle frame exceeds memory limit");
        }
        if (writtenAssets.emplace(asset, true).second) {
          saveBytes(stage.filePath(asset), bytes);
          artwork.append(asset);
        }
        draws.append(drawJson(draw, asset));
      }
      require(captured.particles.size() <= maxDraws, "Too many captured particles");
      ParticleCache::Attribute tintMix, affine, nativeOpacity, nativeScale, tintRGBA, history;
      tintMix.name = "ot_tintMix";
      tintMix.components = 1;
      affine.name = "ot_affine";
      affine.components = 6;
      nativeOpacity.name = "ot_opacity";
      nativeScale.name = "ot_scale";
      tintRGBA.name = "ot_tintRGBA";
      tintRGBA.components = 4;
      history.name = "ot_history";
      history.components = 6;
      for (const auto &state : captured.particles) {
        require(!cancelled(cancel), "Particle bake cancelled");
        require(!state.background && state.id <= quint64(std::numeric_limits<std::int32_t>::max()) &&
                    births.emplace(state.id, true).second,
                "Invalid or duplicate particle identity for interchange");
        ParticleCache::Particle p;
        p.id = std::int64_t(state.id);
        p.position = {{state.position.x, state.position.y, 0}};
        p.velocity = {{state.velocity.x, state.velocity.y, 0}};
        p.size = std::abs(state.scale); p.rotation = state.angle;
        // Native life is measured in scene frames; interchange uses seconds.
        p.age = state.age / metadata.framesPerSecond;
        p.lifetime = state.lifetime / metadata.framesPerSecond;
        p.artworkIndex = state.source; p.artworkFrame = state.artworkFrame;
        p.tint = {{state.tint.r / 255.0, state.tint.g / 255.0,
                   state.tint.b / 255.0, std::clamp(state.opacity, 0.0, 1.0)}};
        snapshot.particles.push_back(p);
        tintMix.floats.push_back(float(state.tintMix));
        nativeOpacity.floats.push_back(float(state.opacity));
        nativeScale.floats.push_back(float(state.scale));
        for (int channel : {int(state.tint.r), int(state.tint.g), int(state.tint.b), int(state.tint.m)})
          tintRGBA.floats.push_back(channel / 255.0f);
        for (const auto &point : state.history) {
          history.floats.push_back(float(point.x));
          history.floats.push_back(float(point.y));
        }
        const auto &a = state.rasterToWorld;
        for (double v : {a.a11, a.a12, a.a13, a.a21, a.a22, a.a23})
          affine.floats.push_back(float(v));
      }
      snapshot.attributes.push_back(std::move(tintMix));
      snapshot.attributes.push_back(std::move(affine));
      snapshot.attributes.push_back(std::move(nativeOpacity));
      snapshot.attributes.push_back(std::move(nativeScale));
      snapshot.attributes.push_back(std::move(tintRGBA));
      snapshot.attributes.push_back(std::move(history));
      QJsonObject extra{{"native", true}, {"sceneFrame", captured.frame},
          {"fps", metadata.framesPerSecond},
          {"domain", QJsonArray{captured.domain.x0, captured.domain.y0,
                                captured.domain.x1, captured.domain.y1}},
          {"renderAffine", affineJson(captured.renderAffine)},
          {"bitsPerPixel", captured.bitsPerPixel}, {"linear", captured.linearColorSpace},
          {"gamma", captured.colorSpaceGamma}, {"draws", draws}};
      // Round-trip the native sprite data as well as Partio data before commit.
      snapshot.extra = extra;
      snapshot.sourceDirectory = stage.absolutePath();
      Frame verified;
      if (!loadFrame(snapshot, verified, error, cancel)) return false;
      const QString file = QString("frames/%1.bgeo").arg(number, 8, 10, QLatin1Char('0'));
      ParticleCache::WriteOptions options;
      options.preserveOriginalAttributes = false;
      if (!ParticleCache::writeFrame(stage.filePath(file), snapshot, options, error, cancel))
        return false;
      const QString drawFile = QString("frames/%1.draws.json").arg(number, 8, 10, QLatin1Char('0'));
      const QByteArray drawData = QJsonDocument(extra).toJson(QJsonDocument::Compact);
      require(drawData.size() <= 64 * 1024 * 1024, "Native draw metadata exceeds per-frame limit");
      saveBytes(stage.filePath(drawFile), drawData);
      artwork.append(drawFile);
      frames.append(QJsonObject{{"frame", number}, {"file", file},
          {"extra", QJsonObject{{"native", true}, {"drawFile", drawFile}}}});
      if (progress) progress(index + 1, count);
    }
    require(!cancelled(cancel), "Particle bake cancelled");
    QJsonObject packageMetadata = metadata.extra;
    packageMetadata["sizeSemantics"] = "artwork-scale";
    packageMetadata["appearance"] = "prepared-native-sprites";
    QJsonObject manifest{{"format", "opentoonz-particle-cache"}, {"version", 1},
        {"fps", metadata.framesPerSecond}, {"firstFrame", metadata.firstFrame},
        {"lastFrame", metadata.lastFrame}, {"units", metadata.units},
        {"mapping", ParticleCache::mappingToJson(ParticleCache::Mapping())},
        {"frames", frames}, {"artwork", artwork}, {"extra", packageMetadata}};
    saveBytes(stage.filePath("cache.otpc"), QJsonDocument(manifest).toJson());
    ParticleCache::Sequence sequence;
    if (!ParticleCache::openSequence(stage.filePath("cache.otpc"), sequence, error)) return false;
    require(!cancelled(cancel), "Particle bake cancelled");
    require(!QFileInfo::exists(destination.absoluteFilePath()) &&
                parent.rename(QFileInfo(temporary.path()).fileName(), destination.fileName()),
            "Unable to publish particle cache; destination may already exist");
    temporary.setAutoRemove(false);
    return true;
  } catch (const TException &e) {
    error = QString::fromStdWString(e.getMessage());
    return false;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return false;
  } catch (...) {
    error = "Unable to write native particle package";
    return false;
  }
}
}  // namespace ParticleBake
