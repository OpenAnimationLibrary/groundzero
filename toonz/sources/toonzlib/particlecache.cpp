#include "toonz/particlecache.h"

#include <Partio.h>
#include <zlib.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace ParticleCache {
namespace {
constexpr std::size_t maxParticles = 10000000;
constexpr std::size_t maxAttributes = 256;
constexpr std::size_t maxComponents = 4096;
constexpr std::size_t maxDataBytes = 1024ULL * 1024 * 1024;
constexpr int maxFrames = 100000;

struct Release {
  void operator()(Partio::ParticlesInfo *p) const { if (p) p->release(); }
};
using Particles = std::unique_ptr<Partio::ParticlesDataMutable, Release>;
using Headers = std::unique_ptr<Partio::ParticlesInfo, Release>;
std::mutex ioMutex;
std::mutex cacheMutex;
struct CacheEntry { SnapshotPtr snapshot; std::uint64_t used; };
std::map<QString, CacheEntry> memoryCache;
std::size_t cacheBytes = 0;
std::size_t cacheLimit = 256ULL * 1024 * 1024;
std::uint64_t cacheClock = 0;

bool cancelled(const Cancel &cancel, QString &error) {
  if (cancel && cancel()) { error = "Particle cache operation cancelled."; return true; }
  return false;
}

QString extension(QString path) {
  if (path.endsWith(".gz", Qt::CaseInsensitive)) path.chop(3);
  return QFileInfo(path).suffix().toLower();
}

bool supported(const QString &path, bool writing) {
  const auto ext = extension(path);
  for (const auto &f : capabilities())
    if (f.extension == ext) return writing ? f.writable : f.readable;
  return false;
}

bool needsGeometryPosition(const QString &format) {
  return format == "bgeo" || format == "bhclassic" || format == "itbl" ||
      format == "atbl" || format == "geo" || format == "hclassic" || format == "ptc" ||
      format == "ptf" || format == "pdc" || format == "bin" || format == "rib";
}

enum class Transfer { Copy, Inflate, Deflate };

// Keep gzip outside Partio: several upstream codecs ignore its compressed flag
// or do not recognize gzip input. inflate also checks each member's CRC/trailer.
bool transferFile(const QString &source, const QString &destination, Transfer mode,
                  QString &error, const Cancel &cancel) {
  QFile input(source), output(destination);
  if (!input.open(QIODevice::ReadOnly)) { error = input.errorString(); return false; }
  if (input.size() > qint64(maxDataBytes)) {
    error = "Particle file exceeds the 1 GiB input limit."; return false;
  }
  if (!output.open(QIODevice::WriteOnly)) { error = output.errorString(); return false; }
  std::array<unsigned char, 64 * 1024> inBuffer, outBuffer;
  std::size_t outputBytes = 0;
  auto read = [&]() -> qint64 {
    const qint64 size = input.read(reinterpret_cast<char *>(inBuffer.data()), inBuffer.size());
    if (size < 0) error = input.errorString();
    else if (input.pos() > qint64(maxDataBytes)) {
      error = "Particle file exceeds the 1 GiB input limit."; return -1;
    }
    return size;
  };
  auto write = [&](const unsigned char *data, std::size_t size) {
    if (size > maxDataBytes - outputBytes) {
      error = "Particle file exceeds the 1 GiB decompressed/output limit."; return false;
    }
    if (output.write(reinterpret_cast<const char *>(data), qint64(size)) != qint64(size)) {
      error = output.errorString(); return false;
    }
    outputBytes += size; return true;
  };
  if (mode == Transfer::Copy) {
    while (!input.atEnd()) {
      if (cancelled(cancel, error)) return false;
      const qint64 size = read();
      if (size < 0) return false;
      if (!write(inBuffer.data(), std::size_t(size))) return false;
    }
  } else {
    z_stream stream{};
    const bool compress = mode == Transfer::Deflate;
    const int initialized = compress
        ? deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY)
        : inflateInit2(&stream, MAX_WBITS + 16);
    if (initialized != Z_OK) { error = "Cannot initialize particle gzip codec."; return false; }
    struct EndStream {
      z_stream &stream; bool compress;
      ~EndStream() { if (compress) deflateEnd(&stream); else inflateEnd(&stream); }
    } end{stream, compress};
    bool eof = false;
    for (;;) {
      if (cancelled(cancel, error)) return false;
      if (!stream.avail_in && !eof) {
        const qint64 size = read();
        if (size < 0) return false;
        eof = size == 0;
        stream.next_in = inBuffer.data(); stream.avail_in = uInt(size);
      }
      stream.next_out = outBuffer.data(); stream.avail_out = uInt(outBuffer.size());
      const int status = compress ? deflate(&stream, eof ? Z_FINISH : Z_NO_FLUSH)
                                  : inflate(&stream, Z_NO_FLUSH);
      if (!write(outBuffer.data(), outBuffer.size() - stream.avail_out)) return false;
      if (status == Z_STREAM_END) {
        if (compress) break;
        // Gzip permits concatenated members. Preserve unread bytes when
        // resetting, and reject trailing garbage as an invalid next member.
        if (!stream.avail_in) {
          const qint64 size = read();
          if (size < 0) return false;
          if (!size) break;
          stream.next_in = inBuffer.data(); stream.avail_in = uInt(size);
        }
        Bytef *remaining = stream.next_in; const uInt size = stream.avail_in;
        if (inflateReset2(&stream, MAX_WBITS + 16) != Z_OK) {
          error = "Cannot reset particle gzip codec."; return false;
        }
        stream.next_in = remaining; stream.avail_in = size; eof = false;
      } else if (status != Z_OK && status != Z_BUF_ERROR) {
        error = compress ? "Cannot compress particle data." : "Invalid particle gzip data or checksum.";
        return false;
      } else if (eof && !stream.avail_in && stream.avail_out) {
        error = compress ? "Cannot finish particle gzip output." : "Truncated particle gzip data.";
        return false;
      }
    }
  }
  if (cancelled(cancel, error)) return false;
  if (!output.flush()) { error = output.errorString(); return false; }
  return true;
}

bool prepareInput(const QString &path, const QTemporaryDir &staging,
                  QString &prepared, QString &error, const Cancel &cancel) {
  if (!staging.isValid()) { error = "Cannot create particle input staging directory."; return false; }
  QFile input(path);
  if (!input.open(QIODevice::ReadOnly)) { error = input.errorString(); return false; }
  const QByteArray magic = input.peek(2);
  if (input.error() != QFile::NoError) { error = input.errorString(); return false; }
  const bool gzip = path.endsWith(".gz", Qt::CaseInsensitive) || magic == QByteArray::fromHex("1f8b");
  input.close();
  prepared = QDir(staging.path()).filePath("frame." + extension(path));
  return transferFile(path, prepared, gzip ? Transfer::Inflate : Transfer::Copy, error, cancel);
}

bool finite(double v) { return std::isfinite(v); }

bool validMapping(const Mapping &m, QString &error) {
  std::set<int> axes(m.axes.begin(), m.axes.end());
  if (axes != std::set<int>({0, 1, 2}) || !finite(m.units) || m.units <= 0 ||
      !finite(m.defaultSize) || m.defaultSize < 0 || m.position.isEmpty()) {
    error = "Invalid particle position, units, size or coordinate mapping.";
    return false;
  }
  for (int sign : m.axisSigns) if (sign != -1 && sign != 1) {
    error = "Particle coordinate signs must be +1 or -1."; return false;
  }
  return true;
}

QString fileDigest(const QString &path, QString &error, const Cancel &cancel) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) { error = f.errorString(); return {}; }
  if (f.size() > qint64(maxDataBytes)) {
    error = "Particle file exceeds the 1 GiB input limit."; return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!f.atEnd()) {
    if (cancelled(cancel, error)) return {};
    const QByteArray chunk = f.read(256 * 1024);
    if (chunk.isEmpty() && f.error() != QFile::NoError) {
      error = f.errorString(); return {};
    }
    hash.addData(chunk);
  }
  return QString::fromLatin1(hash.result().toHex());
}

void trimCache() {
  while (cacheBytes > cacheLimit && !memoryCache.empty()) {
    auto oldest = std::min_element(memoryCache.begin(), memoryCache.end(),
        [](const auto &a, const auto &b) { return a.second.used < b.second.used; });
    cacheBytes -= oldest->second.snapshot->memoryBytes;
    memoryCache.erase(oldest);
  }
}

bool checkHeader(const Partio::ParticlesInfo &p, QString &error) {
  const int count = p.numParticles();
  const int attrs = p.numAttributes(), fixed = p.numFixedAttributes();
  if (count < 0 || std::size_t(count) > maxParticles || attrs < 0 || fixed < 0 ||
      std::size_t(attrs) + std::size_t(fixed) > maxAttributes) {
    error = "Particle count or attribute count exceeds the supported limit.";
    return false;
  }
  std::size_t bytes = std::size_t(count) * sizeof(Particle);
  for (int i = 0; i < attrs + fixed; ++i) {
    int components;
    Partio::ParticleAttributeType type;
    if (i < attrs) {
      Partio::ParticleAttribute a;
      if (!p.attributeInfo(i, a)) return false;
      components = a.count; type = a.type;
    } else {
      Partio::FixedAttribute a;
      if (!p.fixedAttributeInfo(i - attrs, a)) return false;
      components = a.count; type = a.type;
    }
    if (components <= 0 || std::size_t(components) > maxComponents ||
        type < Partio::VECTOR || type > Partio::INDEXEDSTR ||
        (type == Partio::VECTOR && components != 3)) {
      error = "Invalid particle attribute type or component count."; return false;
    }
    const std::size_t entries = i < attrs ? std::size_t(count) : 1;
    if (entries && std::size_t(components) > maxDataBytes / sizeof(float) / entries) {
      error = "Decoded particle data exceeds the 1 GiB limit."; return false;
    }
    const std::size_t size = entries * std::size_t(components) * sizeof(float);
    if (size > maxDataBytes || bytes > maxDataBytes - size) {
      error = "Decoded particle data exceeds the 1 GiB limit."; return false;
    }
    bytes += size;
  }
  return true;
}

AttributeType attributeType(Partio::ParticleAttributeType type) {
  switch (type) {
  case Partio::VECTOR: return AttributeType::Vector;
  case Partio::INT: return AttributeType::Int;
  case Partio::INDEXEDSTR: return AttributeType::IndexedString;
  default: return AttributeType::Float;
  }
}

Partio::ParticleAttributeType partioType(AttributeType type) {
  switch (type) {
  case AttributeType::Vector: return Partio::VECTOR;
  case AttributeType::Int: return Partio::INT;
  case AttributeType::IndexedString: return Partio::INDEXEDSTR;
  default: return Partio::FLOAT;
  }
}

bool readAttributes(const Partio::ParticlesData &p, Snapshot &snapshot, QString &error) {
  std::set<QString> names;
  for (int i = 0; i < p.numAttributes() + p.numFixedAttributes(); ++i) {
    Attribute a;
    a.fixed = i >= p.numAttributes();
    Partio::ParticleAttribute pa;
    Partio::FixedAttribute fa;
    if (a.fixed) {
      p.fixedAttributeInfo(i - p.numAttributes(), fa);
      a.name = QString::fromUtf8(fa.name.c_str()); a.type = attributeType(fa.type);
      a.components = fa.count;
    } else {
      p.attributeInfo(i, pa);
      a.name = QString::fromUtf8(pa.name.c_str()); a.type = attributeType(pa.type);
      a.components = pa.count;
    }
    const QString identity = (a.fixed ? "fixed:" : "particle:") + a.name;
    if (a.name.isEmpty() || names.count(identity)) {
      error = "Empty or duplicate particle attribute name."; return false;
    }
    names.insert(identity);
    const std::size_t rows = a.fixed ? 1 : std::size_t(p.numParticles());
    const std::size_t entries = rows * std::size_t(a.components);
    if (a.type == AttributeType::Float || a.type == AttributeType::Vector) {
      a.floats.resize(entries);
      for (std::size_t row = 0; row < rows; ++row) {
        const float *values = a.fixed ? p.fixedData<float>(fa) : p.data<float>(pa, row);
        for (int c = 0; c < a.components; ++c) {
          if (!finite(values[c])) { error = "Particle cache contains non-finite values."; return false; }
          a.floats[row * a.components + c] = values[c];
        }
      }
    } else {
      if (a.type == AttributeType::IndexedString) {
        const auto &strings = a.fixed ? p.fixedIndexedStrs(fa) : p.indexedStrs(pa);
        std::size_t textBytes = 0;
        for (const auto &s : strings) {
          if (s.size() > maxDataBytes / sizeof(QChar) ||
              textBytes > maxDataBytes - s.size() * sizeof(QChar)) {
            error = "Particle string dictionary exceeds the memory limit."; return false;
          }
          textBytes += s.size() * sizeof(QChar);
          a.strings.push_back(QString::fromUtf8(s.c_str()));
        }
      }
      a.integers.resize(entries);
      for (std::size_t row = 0; row < rows; ++row) {
        const int *values = a.fixed ? p.fixedData<int>(fa) : p.data<int>(pa, row);
        for (int c = 0; c < a.components; ++c) {
          const int value = values[c];
          if (a.type == AttributeType::IndexedString && (value < 0 || value >= a.strings.size())) {
            error = "Particle cache contains an invalid string-table index."; return false;
          }
          a.integers[row * a.components + c] = value;
        }
      }
    }
    snapshot.memoryBytes += sizeof(Attribute) + entries * sizeof(float);
    for (const auto &s : a.strings) snapshot.memoryBytes += std::size_t(s.size()) * sizeof(QChar);
    if (snapshot.memoryBytes > maxDataBytes) {
      error = "Decoded particle attributes exceed the memory limit."; return false;
    }
    snapshot.attributes.push_back(std::move(a));
  }
  return true;
}

const Attribute *findAttribute(const Snapshot &s, const QString &name) {
  if (name.isEmpty()) return nullptr;
  for (const auto &a : s.attributes) if (!a.fixed && a.name == name) return &a;
  return nullptr;
}

double number(const Attribute *a, std::size_t row, int component, double fallback) {
  if (!a || component >= a->components) return fallback;
  const std::size_t index = row * a->components + component;
  // A mixed int/float conditional promotes the integer to float first, which
  // merges distinct IDs above 2^24. Promote each stored value directly to double.
  if (a->type == AttributeType::Int) return double(a->integers[index]);
  return double(a->floats[index]);
}

bool validateNumeric(const Attribute *a, int minimum, QString &error) {
  if (a && (a->components < minimum || a->type == AttributeType::IndexedString)) {
    error = "Attribute '" + a->name + "' has an incompatible type or component count.";
    return false;
  }
  return true;
}

bool mapParticles(Snapshot &s, std::size_t count, const Mapping &m, QString &error) {
  const auto pos = findAttribute(s, m.position);
  const auto vel = findAttribute(s, m.velocity);
  const auto id = findAttribute(s, m.id);
  const auto size = findAttribute(s, m.size);
  const auto rotation = findAttribute(s, m.rotation);
  const auto color = findAttribute(s, m.color);
  const auto opacity = findAttribute(s, m.opacity);
  const auto age = findAttribute(s, m.age);
  const auto life = findAttribute(s, m.lifetime);
  const auto artwork = findAttribute(s, m.artworkIndex);
  const auto frame = findAttribute(s, m.artworkFrame);
  if (!pos && count) { error = "Missing position attribute '" + m.position + "'."; return false; }
  if (!validateNumeric(pos, 3, error) || !validateNumeric(vel, 3, error) ||
      !validateNumeric(color, 3, error)) return false;
  for (const auto a : {id, size, rotation, opacity, age, life, artwork, frame})
    if (!validateNumeric(a, 1, error)) return false;
  s.hasStableIds = id != nullptr;
  s.hasVelocity = vel != nullptr;
  s.hasArtworkFrame = frame != nullptr;
  if (!id && count) s.warnings << "No stable IDs: subframe playback holds the previous cache frame.";
  if (count > maxDataBytes / sizeof(Particle) ||
      s.memoryBytes > maxDataBytes - count * sizeof(Particle)) {
    error = "Decoded particle data exceeds the 1 GiB limit."; return false;
  }
  s.particleCount = count;
  s.particles.resize(count);
  s.memoryBytes += count * sizeof(Particle);
  for (std::size_t i = 0; i < count; ++i) {
    Particle &p = s.particles[i];
    const double identity = number(id, i, 0, double(i));
    if (std::floor(identity) != identity || std::abs(identity) > 9007199254740991.0) {
      error = "Particle IDs must be exactly representable integers."; return false;
    }
    p.id = std::int64_t(identity);
    for (int c = 0; c < 3; ++c) {
      p.position[c] = number(pos, i, m.axes[c], 0) * m.axisSigns[c] * m.units;
      p.velocity[c] = number(vel, i, m.axes[c], 0) * m.axisSigns[c] * m.units;
      p.tint[c] = number(color, i, c, 1);
    }
    p.tint[3] = number(opacity, i, 0, number(color, i, 3, 1));
    p.size = number(size, i, 0, m.defaultSize) * m.units * (m.sizeIsDiameter ? 0.5 : 1);
    p.rotation = number(rotation, i, 0, 0) * (m.rotationInRadians ? 180.0 / 3.14159265358979323846 : 1);
    p.age = number(age, i, 0, 0); p.lifetime = number(life, i, 0, 0);
    const double artworkId = number(artwork, i, 0, 0);
    if (std::floor(artworkId) != artworkId || artworkId < 0 || artworkId > std::numeric_limits<int>::max()) {
      error = "Artwork indices must be nonnegative integers."; return false;
    }
    p.artworkIndex = int(artworkId);
    p.artworkFrame = number(frame, i, 0, 0);
    if (p.size < 0 || p.tint[3] < 0 || p.tint[3] > 1 || !finite(p.size) || !finite(p.rotation)) {
      error = "Invalid particle size, opacity or rotation."; return false;
    }
    for (int c = 0; c < 3; ++c) if (!finite(p.position[c]) || !finite(p.velocity[c])) {
      error = "Particle coordinate transform overflow."; return false;
    }
  }
  return true;
}

bool validMetadata(const Metadata &m, QString &error) {
  if (!finite(m.framesPerSecond) || m.framesPerSecond <= 0 ||
      m.lastFrame < m.firstFrame ||
      std::int64_t(m.lastFrame) - m.firstFrame >= maxFrames) {
    error = "Invalid particle sequence frame range or frame rate."; return false;
  }
  return true;
}

QString resolve(const QDir &base, const QString &path) {
  return QDir::cleanPath(QFileInfo(path).isAbsolute() ? path : base.absoluteFilePath(path));
}

bool integerJson(const QJsonValue &v, int &out) {
  if (!v.isDouble()) return false;
  const double n = v.toDouble();
  if (!finite(n) || std::floor(n) != n || n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max()) return false;
  out = int(n); return true;
}

SnapshotPtr attachMetadata(SnapshotPtr snapshot, const Sequence &sequence, int frame) {
  if (!snapshot) return snapshot;
  const auto found = sequence.frameMetadata.find(frame);
  if (found == sequence.frameMetadata.end()) return snapshot;
  const QString directory = sequence.manifestPath.isEmpty()
      ? QFileInfo(sequence.frames.at(frame)).absolutePath()
      : QFileInfo(sequence.manifestPath).absolutePath();
  const QByteArray metadata = QJsonDocument(found->second).toJson(QJsonDocument::Compact);
  const QString revision = QString::fromLatin1(QCryptographicHash::hash(
      snapshot->revision.toUtf8() + directory.toUtf8() + metadata,
      QCryptographicHash::Sha256).toHex());
  const QString cacheKey = "metadata:" + revision;
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    const auto cached = memoryCache.find(cacheKey);
    if (cached != memoryCache.end()) {
      cached->second.used = ++cacheClock; return cached->second.snapshot;
    }
  }
  auto result = std::make_shared<Snapshot>(*snapshot);
  result->sourceDirectory = directory;
  result->extra = found->second;
  result->revision = revision;
  result->memoryBytes += std::size_t(metadata.size());
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (result->memoryBytes <= cacheLimit) {
      const auto cached = memoryCache.find(cacheKey);
      if (cached != memoryCache.end()) cacheBytes -= cached->second.snapshot->memoryBytes;
      memoryCache[cacheKey] = {result, ++cacheClock}; cacheBytes += result->memoryBytes;
      trimCache();
    }
  }
  return result;
}

SnapshotPtr frameAt(const Sequence &s, int frame, const Mapping &mapping,
                    MissingFrame missing, QString &error, const Cancel &cancel) {
  auto found = s.frames.find(frame);
  if (found == s.frames.end()) {
    if (missing == MissingFrame::Empty) return std::make_shared<Snapshot>();
    if (missing == MissingFrame::Hold) {
      found = s.frames.upper_bound(frame);
      if (found == s.frames.begin()) return std::make_shared<Snapshot>();
      --found;
    } else { error = QString("Missing particle cache frame %1.").arg(frame); return {}; }
  }
  return attachMetadata(loadFrame(found->second, mapping, error, cancel), s, found->first);
}

}  // namespace

std::vector<FormatCapability> capabilities() {
  return {{"bgeo", "Houdini classic BGEO v5", true, true},
          {"bhclassic", "Houdini classic binary geometry", true, true},
          {"geo", "Houdini classic ASCII geometry", true, true},
          {"hclassic", "Houdini classic ASCII geometry", true, true},
          {"pdb", "Particle database", true, true},
          {"pdb32", "32-bit particle database", true, true},
          {"pdb64", "64-bit particle database", true, true},
          {"pda", "ASCII particle database", true, true},
          {"mc", "Maya cache (read only)", true, false},
          {"ptc", "Point cloud", true, true},
          {"ptf", "Point cloud alias", true, true},
          {"pdc", "Maya particle cache", true, true},
          {"prt", "Krakatoa particles", true, true},
          {"bin", "RealFlow particles", true, true},
          {"pts", "ASCII points (read only)", true, false},
          {"itbl", "Classic BGEO particle table", true, true},
          {"atbl", "Classic BGEO particle table", true, true},
          {"rib", "RenderMan points (write only)", false, true}};
}

QJsonObject mappingToJson(const Mapping &m) {
  QJsonObject j;
#define STORE(field) j[#field] = m.field
  STORE(position); STORE(velocity); STORE(id); STORE(size); STORE(rotation);
  STORE(color); STORE(opacity); STORE(age); STORE(lifetime);
  STORE(artworkIndex); STORE(artworkFrame); STORE(units); STORE(defaultSize);
  STORE(sizeIsDiameter); STORE(rotationInRadians);
#undef STORE
  QJsonArray axes, signs;
  for (int i = 0; i < 3; ++i) { axes.append(m.axes[i]); signs.append(m.axisSigns[i]); }
  j["axes"] = axes; j["axisSigns"] = signs;
  return j;
}

bool mappingFromJson(const QJsonObject &j, Mapping &m, QString &error) {
  Mapping candidate = m;
#define READ_STRING(field) if (j.contains(#field)) { if (!j[#field].isString()) { error = "Invalid particle attribute mapping."; return false; } candidate.field = j[#field].toString(); }
  READ_STRING(position); READ_STRING(velocity); READ_STRING(id); READ_STRING(size);
  READ_STRING(rotation); READ_STRING(color); READ_STRING(opacity); READ_STRING(age);
  READ_STRING(lifetime); READ_STRING(artworkIndex); READ_STRING(artworkFrame);
#undef READ_STRING
  for (const auto key : {"units", "defaultSize"}) if (j.contains(key) && !j[key].isDouble()) {
    error = "Invalid particle mapping number."; return false;
  }
  if (j.contains("units")) candidate.units = j["units"].toDouble();
  if (j.contains("defaultSize")) candidate.defaultSize = j["defaultSize"].toDouble();
  for (const auto key : {"sizeIsDiameter", "rotationInRadians"}) if (j.contains(key) && !j[key].isBool()) {
    error = "Invalid particle mapping boolean."; return false;
  }
  if (j.contains("sizeIsDiameter")) candidate.sizeIsDiameter = j["sizeIsDiameter"].toBool();
  if (j.contains("rotationInRadians")) candidate.rotationInRadians = j["rotationInRadians"].toBool();
  for (const auto key : {"axes", "axisSigns"}) if (j.contains(key)) {
    const auto array = j[key].toArray();
    if (array.size() != 3) { error = "Invalid coordinate mapping."; return false; }
    for (int i = 0; i < 3; ++i)
      if (!integerJson(array[i], QString(key) == "axes" ? candidate.axes[i] : candidate.axisSigns[i])) {
        error = "Invalid coordinate mapping."; return false;
      }
  }
  if (!validMapping(candidate, error)) return false;
  m = candidate; return true;
}

bool openSequence(const QString &path, Sequence &sequence, QString &error) {
  error.clear();
  QFileInfo source(path);
  if (source.isDir()) source.setFile(QDir(path).filePath("cache.otpc"));
  if (!source.exists() || !source.isFile()) { error = "Particle cache file does not exist."; return false; }
  Sequence result;
  const QDir base = source.absoluteDir();
  if (source.suffix().compare("otpc", Qt::CaseInsensitive) == 0) {
    QFile file(source.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * 1024 * 1024) {
      error = "Cannot read particle manifest or manifest exceeds 32 MiB."; return false;
    }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
    const auto j = doc.object();
    int version = 0;
    if (parse.error != QJsonParseError::NoError || !doc.isObject() ||
        j["format"].toString() != "opentoonz-particle-cache" ||
        !integerJson(j["version"], version) || version != 1) {
      error = "Unsupported or invalid particle cache manifest."; return false;
    }
    if ((j.contains("mapping") && !j["mapping"].isObject()) ||
        (j.contains("extra") && !j["extra"].isObject()) ||
        (j.contains("artwork") && !j["artwork"].isArray()) ||
        (j.contains("units") && !j["units"].isString())) {
      error = "Invalid particle manifest metadata."; return false;
    }
    result.manifestPath = source.absoluteFilePath();
    if (!j["fps"].isDouble() || !integerJson(j["firstFrame"], result.metadata.firstFrame) ||
        !integerJson(j["lastFrame"], result.metadata.lastFrame)) {
      error = "Missing or invalid particle manifest timing."; return false;
    }
    result.metadata.framesPerSecond = j["fps"].toDouble();
    result.metadata.units = j["units"].toString("ot-world");
    result.metadata.extra = j["extra"].toObject();
    if (!validMetadata(result.metadata, error) ||
        !mappingFromJson(j["mapping"].toObject(), result.mapping, error)) return false;
    result.metadata.mapping = result.mapping;
    const auto frames = j["frames"].toArray();
    if (frames.isEmpty() || frames.size() > maxFrames) {
      error = "Particle manifest has no frames or exceeds the frame limit."; return false;
    }
    for (const auto value : frames) {
      const auto entry = value.toObject(); int frame;
      const QString name = entry["file"].toString();
      if (!integerJson(entry["frame"], frame) || name.isEmpty() || name.contains(QChar('\0')) ||
          frame < result.metadata.firstFrame || frame > result.metadata.lastFrame ||
          !supported(name, false) || result.frames.count(frame) ||
          (entry.contains("extra") && !entry["extra"].isObject())) {
        error = "Invalid or duplicate particle frame entry."; return false;
      }
      result.frames.emplace(frame, resolve(base, name));
      if (entry.contains("extra")) result.frameMetadata[frame] = entry["extra"].toObject();
    }
    for (const auto value : j["artwork"].toArray()) {
      if (!value.isString() || value.toString().isEmpty() || value.toString().contains(QChar('\0'))) {
        error = "Invalid particle artwork path."; return false;
      }
      result.metadata.artworkPaths << resolve(base, value.toString());
    }
  } else {
    if (!supported(source.fileName(), false)) {
      error = "Unsupported particle format. Modern BGEO and .bgeo.sc are not supported; use .bhclassic.";
      return false;
    }
    const QRegularExpression pattern("^(.*?)(-?[0-9]+)(\\.[^.]+(?:\\.gz)?)$");
    const auto match = pattern.match(source.fileName());
    if (match.hasMatch()) {
      const QRegularExpression siblings("^" + QRegularExpression::escape(match.captured(1)) +
          "(-?[0-9]+)" + QRegularExpression::escape(match.captured(3)) + "$");
      for (const auto &name : base.entryList(QDir::Files, QDir::Name)) {
        const auto candidate = siblings.match(name);
        if (!candidate.hasMatch()) continue;
        bool ok = false; const int frame = candidate.captured(1).toInt(&ok);
        if (!ok || result.frames.count(frame) || result.frames.size() >= maxFrames) {
          error = "Ambiguous frame numbering or too many particle files."; return false;
        }
        result.frames[frame] = base.absoluteFilePath(name);
      }
    } else result.frames[1] = source.absoluteFilePath();
    if (result.frames.empty()) { error = "No particle frames found."; return false; }
    result.metadata.firstFrame = result.frames.begin()->first;
    result.metadata.lastFrame = result.frames.rbegin()->first;
    if (!validMetadata(result.metadata, error)) return false;
  }
  sequence = std::move(result); return true;
}

QStringList sequenceFiles(const Sequence &sequence) {
  QStringList paths;
  if (!sequence.manifestPath.isEmpty()) paths << sequence.manifestPath;
  for (const auto &frame : sequence.frames) paths << frame.second;
  paths << sequence.metadata.artworkPaths;
  paths.removeDuplicates(); return paths;
}

SnapshotPtr inspectFrame(const QString &path, QString &error, const Cancel &cancel) try {
  error.clear();
  if (cancelled(cancel, error)) return {};
  if (!supported(path, false)) { error = "Unsupported particle cache reader."; return {}; }
  const QFileInfo info(path);
  if (!info.isFile()) { error = "Particle cache frame is missing."; return {}; }
  const QString digest = fileDigest(path, error, cancel);
  if (digest.isEmpty()) return {};
  QTemporaryDir staging;
  QString prepared;
  if (!prepareInput(path, staging, prepared, error, cancel)) return {};
  auto result = std::make_shared<Snapshot>();
  std::ostringstream diagnostics;
  {
    std::lock_guard<std::mutex> lock(ioMutex);
    if (cancelled(cancel, error)) return {};
    const QByteArray name = QFile::encodeName(prepared);
    Headers header(Partio::readHeaders(name.constData(), true, diagnostics));
    if (!header) { error = QString::fromStdString(diagnostics.str()); return {}; }
    if (!checkHeader(*header, error)) return {};
    Particles data(Partio::read(name.constData(), true, diagnostics));
    if (!data) { error = QString::fromStdString(diagnostics.str()); return {}; }
    if (!checkHeader(*data, error) || !readAttributes(*data, *result, error)) return {};
    result->particleCount = std::size_t(data->numParticles());
  }
  if (cancelled(cancel, error)) return {};
  if (fileDigest(path, error, cancel) != digest) {
    if (error.isEmpty()) error = "Particle cache changed while being inspected; retry.";
    return {};
  }
  result->sourceDirectory = info.absolutePath();
  result->revision = digest;
  if (!diagnostics.str().empty()) result->warnings << QString::fromStdString(diagnostics.str());
  return result;
} catch (const std::exception &exception) {
  error = "Cannot inspect particle cache: " + QString::fromLocal8Bit(exception.what()); return {};
} catch (...) {
  error = "Cannot inspect particle cache: unexpected library failure."; return {};
}

SnapshotPtr loadFrame(const QString &path, const Mapping &mapping, QString &error,
                      const Cancel &cancel) try {
  error.clear();
  if (!validMapping(mapping, error) || cancelled(cancel, error)) return {};
  if (!supported(path, false)) { error = "Unsupported particle cache reader."; return {}; }
  const QFileInfo info(path);
  if (!info.isFile()) { error = "Particle cache frame is missing."; return {}; }
  const QString digest = fileDigest(path, error, cancel);
  if (digest.isEmpty()) return {};
  const QString key = info.canonicalFilePath() + ':' + digest + ':' +
      QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(mappingToJson(mapping)).toJson(QJsonDocument::Compact),
                                                 QCryptographicHash::Sha256).toHex());
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    const auto found = memoryCache.find(key);
    if (found != memoryCache.end()) { found->second.used = ++cacheClock; return found->second.snapshot; }
  }
  QTemporaryDir staging;
  QString prepared;
  if (!prepareInput(path, staging, prepared, error, cancel)) return {};
  auto result = std::make_shared<Snapshot>();
  std::ostringstream diagnostics;
  {
    std::lock_guard<std::mutex> lock(ioMutex);
    if (cancelled(cancel, error)) return {};
    const QByteArray name = QFile::encodeName(prepared);
    Headers header(Partio::readHeaders(name.constData(), true, diagnostics));
    if (!header) { error = QString::fromStdString(diagnostics.str()); return {}; }
    if (!checkHeader(*header, error)) return {};
    Particles data(Partio::read(name.constData(), true, diagnostics));
    if (!data) { error = QString::fromStdString(diagnostics.str()); return {}; }
    if (!checkHeader(*data, error) || !readAttributes(*data, *result, error) ||
        !mapParticles(*result, std::size_t(data->numParticles()), mapping, error)) return {};
  }
  if (cancelled(cancel, error)) return {};
  if (fileDigest(path, error, cancel) != digest) {
    if (error.isEmpty()) error = "Particle cache changed while being read; retry the frame.";
    return {};
  }
  if (!diagnostics.str().empty()) result->warnings << QString::fromStdString(diagnostics.str());
  result->sourceDirectory = info.absolutePath();
  result->revision = QString::fromLatin1(QCryptographicHash::hash(
      key.toUtf8(), QCryptographicHash::Sha256).toHex());
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (result->memoryBytes <= cacheLimit) {
      const auto old = memoryCache.find(key);
      if (old != memoryCache.end()) cacheBytes -= old->second.snapshot->memoryBytes;
      memoryCache[key] = {result, ++cacheClock}; cacheBytes += result->memoryBytes;
      trimCache();
    }
  }
  return result;
} catch (const std::exception &exception) {
  error = "Cannot load particle cache: " + QString::fromLocal8Bit(exception.what()); return {};
} catch (...) {
  error = "Cannot load particle cache: unexpected library failure."; return {};
}

SnapshotPtr interpolate(const Snapshot &a, const Snapshot &b, double weight, QString &error) {
  error.clear();
  if (!finite(weight) || weight < 0 || weight > 1) { error = "Invalid particle interpolation weight."; return {}; }
  if (weight == 0) return std::make_shared<Snapshot>(a);
  if (weight == 1) return std::make_shared<Snapshot>(b);
  if (!a.hasStableIds || !b.hasStableIds || a.extra.value("native").toBool() || b.extra.value("native").toBool())
    return std::make_shared<Snapshot>(a);
  std::unordered_map<std::int64_t, const Particle *> next;
  std::set<std::int64_t> previous;
  for (const auto &p : b.particles) if (!next.emplace(p.id, &p).second) {
    error = "Cannot interpolate duplicate particle IDs."; return {};
  }
  auto result = std::make_shared<Snapshot>();
  result->hasStableIds = true; result->hasVelocity = a.hasVelocity && b.hasVelocity;
  result->hasArtworkFrame = a.hasArtworkFrame && b.hasArtworkFrame;
  auto blend = [weight](double x, double y) { return x + (y - x) * weight; };
  for (const auto &p : a.particles) {
    if (!previous.insert(p.id).second) { error = "Cannot interpolate duplicate particle IDs."; return {}; }
    Particle v = p;
    const auto found = next.find(p.id);
    // Birth/death uses left-sample membership; no invented identities or ghosts.
    if (found != next.end()) {
      const Particle &q = *found->second;
      for (int c = 0; c < 3; ++c) {
        v.position[c] = blend(p.position[c], q.position[c]);
        v.velocity[c] = blend(p.velocity[c], q.velocity[c]);
      }
      for (int c = 0; c < 4; ++c) v.tint[c] = blend(p.tint[c], q.tint[c]);
      v.size = blend(p.size, q.size);
      v.rotation = p.rotation + std::remainder(q.rotation - p.rotation, 360.0) * weight;
      v.age = blend(p.age, q.age); v.lifetime = blend(p.lifetime, q.lifetime);
      v.artworkFrame = blend(p.artworkFrame, q.artworkFrame);
    }
    result->particles.push_back(v);
  }
  result->memoryBytes = result->particles.size() * sizeof(Particle);
  result->particleCount = result->particles.size();
  result->revision = QString::fromLatin1(QCryptographicHash::hash(
      a.revision.toUtf8() + ':' + b.revision.toUtf8() + ':' +
      QByteArray::number(weight, 'g', 17), QCryptographicHash::Sha256).toHex());
  return result;
}

SnapshotPtr sampleSequence(const Sequence &sequence, double sceneFrame,
                           const Playback &playback, const Mapping &mapping,
                           QString &error, const Cancel &cancel) {
  error.clear();
  if (sequence.frames.empty()) { error = "Particle sequence contains no frames."; return {}; }
  if (!validMetadata(sequence.metadata, error)) return {};
  if (!finite(sceneFrame) || !finite(playback.sceneFramesPerSecond) || playback.sceneFramesPerSecond <= 0 ||
      !finite(playback.startFrame) || !finite(playback.offset) || !finite(playback.speed)) {
    error = "Invalid particle playback timing."; return {};
  }
  const double first = sequence.metadata.firstFrame, last = sequence.metadata.lastFrame;
  double frame = first + (sceneFrame - playback.startFrame) * playback.speed *
      sequence.metadata.framesPerSecond / playback.sceneFramesPerSecond + playback.offset;
  if (!finite(frame)) { error = "Particle playback time overflow."; return {}; }
  if (frame < first || frame > last) {
    if (playback.end == EndBehavior::Empty) return std::make_shared<Snapshot>();
    if (playback.end == EndBehavior::Loop) {
      const double length = last - first + 1;
      frame = first + std::fmod(std::fmod(frame - first, length) + length, length);
    } else frame = std::clamp(frame, first, last);
  }
  const int left = int(std::floor(frame));
  const double weight = frame - left;
  auto a = frameAt(sequence, left, mapping, playback.missing, error, cancel);
  if (!a || !playback.interpolate || weight < 1e-9 || !a->hasStableIds || a->extra.value("native").toBool()) return a;
  const int right = left >= last
      ? (playback.end == EndBehavior::Loop ? int(first) : int(last)) : left + 1;
  auto b = frameAt(sequence, right, mapping, playback.missing, error, cancel);
  if (!b) return {};
  return interpolate(*a, *b, weight, error);
}

namespace {

bool validateAttributes(const std::vector<Attribute> &attributes,
                        std::size_t count, QString &error) {
  if (count > maxParticles || attributes.size() > maxAttributes) {
    error = "Particle count or attribute count exceeds the export limit."; return false;
  }
  std::size_t bytes = 0;
  std::set<QString> names;
  for (const auto &a : attributes) {
    const QString key = (a.fixed ? "fixed:" : "particle:") + a.name;
    if (a.name.isEmpty() || a.name.contains(QChar('\0')) || !names.insert(key).second ||
        a.components < 1 || std::size_t(a.components) > maxComponents ||
        (a.type == AttributeType::Vector && a.components != 3)) {
      error = "Invalid or duplicate export attribute."; return false;
    }
    const std::size_t rows = a.fixed ? 1 : count;
    if (rows && std::size_t(a.components) > maxDataBytes / sizeof(float) / rows) {
      error = "Particle export exceeds the memory limit."; return false;
    }
    const std::size_t entries = rows * std::size_t(a.components);
    if (entries > maxDataBytes / sizeof(float) || bytes > maxDataBytes - entries * sizeof(float)) {
      error = "Particle export exceeds the memory limit."; return false;
    }
    bytes += entries * sizeof(float);
    for (const auto &s : a.strings) {
      const std::size_t textSize = std::size_t(s.size()) * sizeof(QChar);
      if (textSize > maxDataBytes || bytes > maxDataBytes - textSize) {
        error = "Particle export string dictionaries exceed the memory limit."; return false;
      }
      bytes += textSize;
    }
    if (a.type == AttributeType::Float || a.type == AttributeType::Vector) {
      if (a.floats.size() != entries) { error = "Export attribute has the wrong number of values."; return false; }
      for (float v : a.floats) if (!finite(v)) {
        error = "Cannot export non-finite particle attributes."; return false;
      }
    } else {
      if (a.integers.size() != entries) { error = "Export attribute has the wrong number of values."; return false; }
      if (a.type == AttributeType::IndexedString) {
        for (const auto &s : a.strings) if (s.contains(QChar('\0'))) {
          error = "Particle string attribute contains a null character."; return false;
        }
        for (int v : a.integers) if (v < 0 || v >= a.strings.size()) {
          error = "Invalid export string-table index."; return false;
        }
      }
    }
  }
  return true;
}

std::vector<Attribute> canonicalAttributes(const Snapshot &snapshot, QString &error) {
  std::vector<Attribute> attributes;
  auto addFloat = [&](const QString &name, int components, bool vector) -> Attribute & {
    Attribute a; a.name = name; a.components = components;
    a.type = vector ? AttributeType::Vector : AttributeType::Float;
    a.floats.reserve(snapshot.particles.size() * components);
    attributes.push_back(std::move(a)); return attributes.back();
  };
  addFloat("position", 3, true); addFloat("velocity", 3, true);
  addFloat("pscale", 1, false); addFloat("rotation", 1, false);
  addFloat("Cd", 3, true); addFloat("opacity", 1, false);
  addFloat("age", 1, false); addFloat("life", 1, false);
  addFloat("ot_frame", 1, false);
  Attribute identity; identity.name = "id"; identity.type = AttributeType::Int;
  attributes.push_back(identity);
  Attribute artwork; artwork.name = "ot_artwork"; artwork.type = AttributeType::Int;
  attributes.push_back(artwork);
  std::set<std::int64_t> ids;
  for (const auto &p : snapshot.particles) {
    if (!finite(p.size) || p.size < 0 || !finite(p.tint[3]) ||
        p.tint[3] < 0 || p.tint[3] > 1 || p.artworkIndex < 0) {
      error = "Cannot export invalid particle size, opacity or artwork index.";
      return {};
    }
    if (p.id < std::numeric_limits<std::int32_t>::min() || p.id > std::numeric_limits<std::int32_t>::max()) {
      error = "Partio integer IDs are 32-bit; the requested ID cannot be preserved.";
      return {};
    }
    if (snapshot.hasStableIds && !ids.insert(p.id).second) {
      error = "Cannot export duplicate stable particle IDs."; return {};
    }
    for (int c = 0; c < 3; ++c) {
      attributes[0].floats.push_back(float(p.position[c]));
      attributes[1].floats.push_back(float(p.velocity[c]));
      attributes[4].floats.push_back(float(p.tint[c]));
    }
    attributes[2].floats.push_back(float(p.size));
    attributes[3].floats.push_back(float(p.rotation));
    attributes[5].floats.push_back(float(p.tint[3]));
    attributes[6].floats.push_back(float(p.age));
    attributes[7].floats.push_back(float(p.lifetime));
    attributes[8].floats.push_back(float(p.artworkFrame));
    attributes[9].integers.push_back(std::int32_t(p.id));
    attributes[10].integers.push_back(p.artworkIndex);
  }
  attributes.erase(std::remove_if(attributes.begin(), attributes.end(),
      [&](const Attribute &a) {
        return (a.name == "id" && !snapshot.hasStableIds) ||
               (a.name == "velocity" && !snapshot.hasVelocity) ||
               (a.name == "ot_frame" && !snapshot.hasArtworkFrame);
      }), attributes.end());
  return attributes;
}

Particles encode(const std::vector<Attribute> &attributes, std::size_t count) {
  Particles data(Partio::create());
  data->addParticles(int(count));
  for (const auto &a : attributes) {
    const QByteArray name = a.name.toUtf8();
    const auto type = partioType(a.type);
    if (a.fixed) {
      const auto attr = data->addFixedAttribute(name.constData(), type, a.components);
      if (a.type == AttributeType::IndexedString)
        for (const auto &value : a.strings) data->registerFixedIndexedStr(attr, value.toUtf8().constData());
      if (a.type == AttributeType::Float || a.type == AttributeType::Vector)
        std::copy(a.floats.begin(), a.floats.end(), data->fixedDataWrite<float>(attr));
      else std::copy(a.integers.begin(), a.integers.end(), data->fixedDataWrite<int>(attr));
    } else {
      const auto attr = data->addAttribute(name.constData(), type, a.components);
      if (a.type == AttributeType::IndexedString)
        for (const auto &value : a.strings) data->registerIndexedStr(attr, value.toUtf8().constData());
      for (std::size_t row = 0; row < count; ++row) {
        const std::size_t start = row * a.components;
        if (a.type == AttributeType::Float || a.type == AttributeType::Vector)
          std::copy_n(a.floats.begin() + start, a.components, data->dataWrite<float>(attr, row));
        else std::copy_n(a.integers.begin() + start, a.components, data->dataWrite<int>(attr, row));
      }
    }
  }
  return data;
}

bool preserves(const std::vector<Attribute> &expected, const Snapshot &actual,
               QString &error) {
  for (const auto &a : expected) {
    const auto found = std::find_if(actual.attributes.begin(), actual.attributes.end(),
        [&](const Attribute &b) { return b.name == a.name && b.fixed == a.fixed; });
    if (found == actual.attributes.end() || found->type != a.type || found->components != a.components ||
        found->integers != a.integers || found->strings.size() != a.strings.size() ||
        !std::equal(found->strings.cbegin(), found->strings.cend(), a.strings.cbegin()) ||
        found->floats.size() != a.floats.size()) {
      error = "The selected format cannot preserve attribute '" + a.name + "'."; return false;
    }
    for (std::size_t i = 0; i < a.floats.size(); ++i) {
      const double tolerance = 1e-6 * std::max(1.0, std::abs(double(a.floats[i])));
      if (std::abs(double(a.floats[i]) - found->floats[i]) > tolerance) {
        error = "The selected format loses values in attribute '" + a.name + "'."; return false;
      }
    }
  }
  return true;
}

bool prepareWriter(const QString &format, std::vector<Attribute> &attributes,
                   std::size_t count, bool allowLossy, QString &error) {
  const bool classicBinary = format == "bgeo" || format == "bhclassic" ||
                             format == "itbl" || format == "atbl";
  const bool ptc = format == "ptc" || format == "ptf";
  const bool pdb = format == "pdb" || format == "pdb32" || format == "pdb64";
  for (auto it = attributes.begin(); it != attributes.end();) {
    const auto &a = *it;
    bool unsupported = a.fixed && !classicBinary;
    const bool numericFloat = a.type == AttributeType::Float || a.type == AttributeType::Vector;
    if (!a.fixed) {
      if (pdb || format == "prt" || format == "pda" || format == "rib")
        unsupported = unsupported || a.type == AttributeType::IndexedString;
      if (format == "prt" && a.name.toUtf8().size() >= 32)
        unsupported = true;  // Upstream copies into a fixed 32-byte channel name.
      if (ptc)
        unsupported = unsupported || !numericFloat ||
                      (a.components != 1 && a.components != 3 && a.components != 16);
      if (format == "pdc")
        unsupported = unsupported ||
            !((a.type == AttributeType::Float && a.components == 1) ||
              (a.type == AttributeType::Vector && a.components == 3));
      if (format == "pda")
        unsupported = unsupported ||
            (a.type == AttributeType::Vector ? a.components != 3 : a.components != 1);
      if (a.name == "position" && (!numericFloat || a.components != 3)) {
        error = "The particle writer requires a three-component floating-point position.";
        return false;
      }
      if (ptc && ((a.name == "normal" && (!numericFloat || a.components != 3)) ||
                  (a.name == "radius" && (!numericFloat || a.components != 1)))) {
        error = "PTC requires floating-point normal[3] and radius[1] attributes.";
        return false;
      }
      if (format == "bin") {
        const bool vector = a.name == "velocity" || a.name == "force" ||
            a.name == "vorticity" || a.name == "normal" || a.name == "uvw";
        if (vector && (!numericFloat || a.components != 3)) {
          error = "RealFlow vector attributes require three floating-point components.";
          return false;
        }
      }
    }
    if (unsupported) {
      if (!allowLossy) {
        error = "The selected format cannot preserve attribute '" + a.name + "'.";
        return false;
      }
      it = attributes.erase(it);
    } else ++it;
  }
  if (ptc) {
    auto has = [&](const char *name) {
      return std::any_of(attributes.begin(), attributes.end(), [&](const Attribute &a) {
        return !a.fixed && a.name == name;
      });
    };
    if (!has("normal")) {
      Attribute a; a.name = "normal"; a.type = AttributeType::Vector; a.components = 3;
      a.floats.resize(count * 3, 0); attributes.push_back(std::move(a));
    }
    if (!has("radius")) {
      Attribute a; a.name = "radius"; a.floats.resize(count, 1);
      for (const auto &scale : attributes) if (!scale.fixed && scale.name == "pscale" &&
          scale.components == 1 && !scale.floats.empty()) a.floats = scale.floats;
      attributes.push_back(std::move(a));
    }
  }
  return validateAttributes(attributes, count, error);
}

bool saveJson(const QString &path, const QJsonObject &object, QString &error) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) { error = file.errorString(); return false; }
  const QByteArray bytes = QJsonDocument(object).toJson();
  if (file.write(bytes) != bytes.size() || !file.commit()) { error = file.errorString(); return false; }
  return true;
}

}  // namespace

bool writeFrame(const QString &path, const Snapshot &snapshot,
                const WriteOptions &options, QString &error, const Cancel &cancel) try {
  error.clear();
  if (cancelled(cancel, error)) return false;
  const QFileInfo target(path);
  if (!supported(path, true) || extension(path) != options.format.toLower()) {
    error = "The selected extension has no matching particle writer."; return false;
  }
  if (target.exists()) { error = "Particle export destination already exists."; return false; }
  if (!target.absoluteDir().exists()) { error = "Particle export parent directory does not exist."; return false; }
  const bool originalAttributes = options.preserveOriginalAttributes &&
      (!snapshot.attributes.empty() || (snapshot.particles.empty() && snapshot.particleCount != 0));
  const std::size_t count = originalAttributes && snapshot.particles.empty()
      ? snapshot.particleCount : snapshot.particles.size();
  auto attributes = originalAttributes
      ? snapshot.attributes : canonicalAttributes(snapshot, error);
  if (!options.preserveOriginalAttributes) {
    // Refresh mapped render values while retaining every unrelated/custom
    // attribute, including native bake fields and fixed metadata.
    for (const auto &a : snapshot.attributes) {
      const bool present = std::any_of(attributes.begin(), attributes.end(),
          [&](const Attribute &b) { return a.name == b.name && a.fixed == b.fixed; });
      if (!present) attributes.push_back(a);
    }
  }
  if (!error.isEmpty() || !validateAttributes(attributes, count, error)) return false;
  // Most formats require position. A custom source mapping is preserved by
  // keeping its attribute while adding a canonical position when absent.
  const auto pos = std::find_if(attributes.begin(), attributes.end(),
      [](const Attribute &a) { return !a.fixed && a.name == "position"; });
  if (pos == attributes.end() && snapshot.particles.size() == count &&
      (!originalAttributes || count != 0 || needsGeometryPosition(extension(path)))) {
    Attribute a; a.name = "position"; a.type = AttributeType::Vector; a.components = 3;
    for (const auto &p : snapshot.particles) for (double v : p.position) a.floats.push_back(float(v));
    attributes.push_back(std::move(a));
    if (!validateAttributes(attributes, count, error)) return false;
  }
  const QString format = extension(path);
  if (needsGeometryPosition(format) && std::none_of(attributes.begin(), attributes.end(), [](const Attribute &a) {
        return !a.fixed && a.name == "position";
      })) {
    error = "The selected format requires a position attribute; choose a usable source position mapping.";
    return false;
  }
  if (extension(path) == "rib") {
    const bool hasWidth = std::any_of(attributes.begin(), attributes.end(), [](const Attribute &a) {
      return !a.fixed && (a.name == "width" || a.name == "radius" || a.name == "radiusPP");
    });
    if (!hasWidth) {
      Attribute width; width.name = "width";
      const auto scale = std::find_if(attributes.begin(), attributes.end(), [](const Attribute &a) {
        return !a.fixed && a.name == "pscale" && a.components == 1 &&
               (a.type == AttributeType::Float || a.type == AttributeType::Vector);
      });
      for (std::size_t i = 0; i < count; ++i)
        width.floats.push_back(float(2 * (scale == attributes.end()
            ? (snapshot.particles.empty() ? 1 : snapshot.particles[i].size) : scale->floats[i])));
      attributes.push_back(std::move(width));
      if (!validateAttributes(attributes, count, error)) return false;
    }
  }
  if (!prepareWriter(format, attributes, count, options.allowLossy, error)) return false;
  if (!supported(path, false) && !options.allowLossy) {
    error = "This write-only format cannot verify attribute preservation. Enable lossy export explicitly.";
    return false;
  }
  QTemporaryDir staging(target.absoluteDir().filePath(".particle-write-XXXXXX"));
  if (!staging.isValid()) { error = "Cannot create particle export staging directory."; return false; }
  const QString staged = QDir(staging.path()).filePath("frame." + format);
  {
    std::lock_guard<std::mutex> lock(ioMutex);
    if (cancelled(cancel, error)) return false;
    Particles data = encode(attributes, count);
    std::ostringstream diagnostics;
    const QByteArray filename = QFile::encodeName(staged);
    Partio::write(filename.constData(), *data, false, true, diagnostics);
    if (!diagnostics.str().empty()) {
      error = QString::fromStdString(diagnostics.str()); return false;
    }
    const QFileInfo written(staged);
    if (!written.exists() || written.size() <= 0) { error = "Particle writer produced no data."; return false; }
    if (written.size() > qint64(maxDataBytes)) { error = "Particle output exceeds the 1 GiB limit."; return false; }
    if (supported(staged, false)) {
      Headers header(Partio::readHeaders(filename.constData(), true, diagnostics));
      if (!header || !checkHeader(*header, error)) {
        if (error.isEmpty()) error = "Cannot verify exported particle header: " + QString::fromStdString(diagnostics.str());
        return false;
      }
      Particles verify(Partio::read(filename.constData(), true, diagnostics));
      Snapshot decoded;
      if (!verify || verify->numParticles() != int(count) ||
          !checkHeader(*verify, error) || !readAttributes(*verify, decoded, error)) {
        if (error.isEmpty()) error = "Particle export failed read-back validation.";
        return false;
      }
      if (!options.allowLossy && !preserves(attributes, decoded, error)) return false;
    }
  }
  if (cancelled(cancel, error)) return false;
  QString published = staged;
  if (options.compressed || path.endsWith(".gz", Qt::CaseInsensitive)) {
    published += ".gz";
    if (!transferFile(staged, published, Transfer::Deflate, error, cancel)) return false;
  }
  // QFile::rename fails if the destination exists; it never replaces user data.
  if (!QFile::rename(published, target.absoluteFilePath())) {
    error = "Cannot publish particle frame; the destination may already exist."; return false;
  }
  return true;
} catch (const std::exception &exception) {
  error = "Cannot write particle cache: " + QString::fromLocal8Bit(exception.what()); return false;
} catch (...) {
  error = "Cannot write particle cache: unexpected library failure."; return false;
}

bool writePackage(const QString &directory, const Metadata &metadata,
                  const FrameProvider &provider, const WriteOptions &options,
                  QString &error, const Cancel &cancel, const Progress &progress) try {
  error.clear();
  if (!validMetadata(metadata, error) || !provider || cancelled(cancel, error)) return false;
  const QFileInfo destination(QDir::cleanPath(directory));
  if (destination.exists() || destination.fileName().isEmpty()) {
    error = "Choose a new particle cache package directory."; return false;
  }
  if (!destination.absoluteDir().exists()) { error = "Particle package parent directory does not exist."; return false; }
  if (!supported("frame." + options.format, true)) {
    error = "No writer exists for the selected particle format."; return false;
  }
  const bool playable = supported("frame." + options.format, false);
  if (!playable && !options.allowLossy) {
    error = "Write-only particle formats require explicit lossy-export permission."; return false;
  }
  QTemporaryDir staging(destination.absoluteDir().filePath(".particle-package-XXXXXX"));
  if (!staging.isValid()) { error = "Cannot create particle package staging directory."; return false; }
  QDir stage(staging.path());
  if (!stage.mkdir("frames") || !stage.mkdir("assets")) { error = "Cannot create particle package folders."; return false; }
  QJsonArray frames, artwork;
  for (int i = 0; i < metadata.artworkPaths.size(); ++i) {
    if (cancelled(cancel, error)) return false;
    const QFileInfo source(metadata.artworkPaths[i]);
    if (!source.isFile()) { error = "Particle artwork dependency is missing."; return false; }
    const QString relative = QString("assets/%1_%2").arg(i, 6, 10, QChar('0')).arg(source.fileName());
    if (!QFile::copy(source.absoluteFilePath(), stage.filePath(relative))) {
      error = "Cannot copy particle artwork dependency."; return false;
    }
    artwork.append(relative);
  }
  const int total = metadata.lastFrame - metadata.firstFrame + 1;
  for (int offset = 0; offset < total; ++offset) {
    if (cancelled(cancel, error)) return false;
    const int frame = metadata.firstFrame + offset;
    const auto snapshot = provider(frame, error);
    if (!snapshot) { if (error.isEmpty()) error = "Particle frame provider returned no data."; return false; }
    if (snapshot->extra.value("native").toBool()) {
      error = "Native draw packages must use the native bake package writer to collect their sprite assets.";
      return false;
    }
    const QString relative = QString("frames/%1.%2%3").arg(frame, 6, 10, QChar('0'))
        .arg(options.format).arg(options.compressed ? ".gz" : "");
    if (!writeFrame(stage.filePath(relative), *snapshot, options, error, cancel)) return false;
    QJsonObject entry{{"frame", frame}, {"file", relative}};
    if (!snapshot->extra.isEmpty()) entry["extra"] = snapshot->extra;
    frames.append(entry);
    if (progress) progress(offset + 1, total);
  }
  QJsonObject manifest{{"format", playable ? "opentoonz-particle-cache" : "opentoonz-particle-export"}, {"version", 1},
      {"fps", metadata.framesPerSecond}, {"firstFrame", metadata.firstFrame},
      {"lastFrame", metadata.lastFrame}, {"units", metadata.units},
      {"mapping", mappingToJson(metadata.mapping)}, {"frames", frames},
      {"artwork", artwork}, {"extra", metadata.extra}};
  if (!playable) manifest["description"] = "Write-only particle interchange export; this is not a playable OT cache.";
  if (!saveJson(stage.filePath(playable ? "cache.otpc" : "export.json"), manifest, error) || cancelled(cancel, error)) return false;
  if (!QDir().rename(staging.path(), destination.absoluteFilePath())) {
    error = "Cannot publish particle package; the destination may already exist."; return false;
  }
  staging.setAutoRemove(false);
  return true;
} catch (const std::exception &exception) {
  error = "Cannot write particle package: " + QString::fromLocal8Bit(exception.what()); return false;
} catch (...) {
  error = "Cannot write particle package: unexpected library failure."; return false;
}

bool exportFiles(const Sequence &sequence, const QString &directory,
                 const WriteOptions &options, QString &error,
                 const Cancel &cancel, const Progress &progress) {
  Metadata metadata = sequence.metadata;
  metadata.mapping = options.preserveOriginalAttributes ? sequence.mapping : Mapping{};
  bool native = false;
  for (const auto &entry : sequence.frameMetadata)
    native = native || entry.second.value("native").toBool();
  if (native) {
    // Generic interchange exports particle data, never a dangling native draw
    // recipe. Native appearance is retained by the native package writer.
    metadata.artworkPaths.clear();
    metadata.extra.remove("native");
    metadata.extra.remove("nativeBake");
  }
  return writePackage(directory, metadata, [&](int frame, QString &failure) -> SnapshotPtr {
    const auto found = sequence.frames.find(frame);
    if (found == sequence.frames.end()) { failure = "Cannot export a sequence with missing frames."; return {}; }
    if (!options.preserveOriginalAttributes)
      return loadFrame(found->second, sequence.mapping, failure, cancel);
    const auto source = inspectFrame(found->second, failure, cancel);
    if (!source) return {};
    // Geometry codecs need their conventional position channel. Keep the
    // complete source schema and add a raw alias without applying render units,
    // axis changes, or validation of unrelated optional renderer mappings.
    const auto position = findAttribute(*source, "position");
    const auto mapped = findAttribute(*source, sequence.mapping.position);
    if (needsGeometryPosition(options.format.toLower()) && !position && mapped &&
        mapped->type != AttributeType::IndexedString && mapped->components >= 3) {
      auto result = std::make_shared<Snapshot>(*source);
      Attribute alias; alias.name = "position"; alias.type = AttributeType::Vector; alias.components = 3;
      alias.floats.reserve(source->particleCount * 3);
      for (std::size_t row = 0; row < source->particleCount; ++row)
        for (int c = 0; c < 3; ++c) alias.floats.push_back(float(number(mapped, row, c, 0)));
      result->attributes.push_back(std::move(alias));
      return result;
    }
    return source;
  }, options, error, cancel, progress);
}

void clearMemoryCache() {
  std::lock_guard<std::mutex> lock(cacheMutex);
  memoryCache.clear(); cacheBytes = 0;
}

void setMemoryCacheLimit(std::size_t bytes) {
  std::lock_guard<std::mutex> lock(cacheMutex);
  cacheLimit = std::min(bytes, maxDataBytes); trimCache();
}

}  // namespace ParticleCache
