// SPDX-License-Identifier: BSD-3-Clause
// Exercise the pinned format readers through the production adapter. Raw
// malformed inputs must fail during inspection as well as ordinary loading.
#include "toonz/particlecache.h"

#include <Partio.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
using namespace ParticleCache;

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

void writeRaw(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly), "Cannot create raw format fixture");
  require(file.write(bytes) == bytes.size(), "Cannot write raw format fixture");
  file.close();
}

void reject(const QString &path, const QByteArray &bytes) {
  writeRaw(path, bytes);
  const std::string label = QFileInfo(path).fileName().toStdString();
  QString error;
  require(!inspectFrame(path, error), label + ": invalid frame inspection succeeded");
  require(!error.isEmpty(), label + ": frame inspection omitted its diagnostic");
  require(!loadFrame(path, Mapping{}, error), label + ": malformed frame loading succeeded");
  require(!error.isEmpty(), label + ": frame loading omitted its diagnostic");
}

const QByteArray geoHeader =
    "PGEOMETRY V5\n"
    "NPoints 1 NPrims 0\n"
    "NPointGroups 0 NPrimGroups 0\n"
    "NPointAttrib 1 NVertexAttrib 0 NPrimAttrib 0 NAttrib 0\n"
    "PointAttrib\n";

void geo(const QDir &dir) {
  // The old scanner wrote both the token and its terminator outside a fixed
  // 4096-byte stack buffer; these also exercise EOF after an escape/quote.
  reject(dir.filePath("long-unterminated.geo"),
         geoHeader + "group 1 index 1 \"" + QByteArray(4096, 'x'));
  reject(dir.filePath("escaped-eof.hclassic"),
         geoHeader + "group 1 index 1 \"unfinished\\");
  reject(dir.filePath("short-dictionary.geo"),
         geoHeader + "group 1 index 2 \"one\"\n");
  reject(dir.filePath("short-points.geo"),
         geoHeader + "density 1 float 0\n1 2 3 1 (\n");
  reject(dir.filePath("bad-vector.geo"),
         geoHeader + "velocity 4097 vector 0\n");

  Snapshot snapshot;
  Particle particle;
  particle.position = {{1, 2, 3}};
  snapshot.particles.push_back(particle);
  Attribute position;
  position.name = "position";
  position.type = AttributeType::Vector;
  position.components = 3;
  position.floats = {1, 2, 3};
  Attribute group;
  group.name = "group";
  group.type = AttributeType::IndexedString;
  group.strings = QStringList{QString(8192, QChar('x')) + " \"quoted\" \\escaped"};
  group.integers = {0};
  snapshot.attributes = {position, group};
  QString error;
  for (const char *format : {"geo", "hclassic"}) {
    WriteOptions options;
    options.format = format;
    const QString path = dir.filePath(QString("long-string.") + format);
    const bool written = writeFrame(path, snapshot, options, error);
    require(written, "Cannot export long GEO dictionary: " + error.toStdString());
    const auto loaded = loadFrame(path, Mapping{}, error);
    require(bool(loaded), error.toStdString());
    bool preserved = false;
    for (const auto &attribute : loaded->attributes)
      if (attribute.name == group.name)
        preserved = attribute.strings.size() == 1 &&
            attribute.strings.front() == group.strings.front() &&
            attribute.integers == group.integers;
    require(preserved, "Long escaped GEO dictionary string was changed");
  }

  // No PointAttrib marker is written for a position-only GEO file.
  writeRaw(dir.filePath("position-only.geo"),
      "PGEOMETRY V5\nNPoints 1 NPrims 0\n"
      "NPointAttrib 0 NVertexAttrib 0 NPrimAttrib 0 NAttrib 0\n1 2 3 1\n");
  const auto positionOnly = loadFrame(dir.filePath("position-only.geo"), Mapping{}, error);
  require(positionOnly && positionOnly->particles.size() == 1 &&
              positionOnly->particles.front().position[2] == 3,
          "Position-only GEO lost its particle payload");
}

void append32(QByteArray &bytes, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8)
    bytes.append(char((value >> shift) & 255));
}

void append64(QByteArray &bytes, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8)
    bytes.append(char((value >> shift) & 255));
}

void put32(QByteArray &bytes, int offset, std::uint32_t value) {
  QByteArray word;
  append32(word, value);
  bytes.replace(offset, 4, word);
}

QByteArray binFrame(int version, std::uint64_t id = 42) {
  // RealFlow wire layouts: v7 adds 36 emitter bytes; later particle layouts
  // add normal, neighbors, UVW/info bits, vorticity and finally a 64-bit ID.
  const int recordBytes[] = {68, 68, 80, 84, 98, 98, 98, 98,
                            110, 110, 110, 114, 114};
  require(version >= 1 && version <= 13, "Invalid BIN fixture version");
  const int headerBytes = version >= 7 ? 356 : 320;
  QByteArray bytes(headerBytes + recordBytes[version - 1], '\0');
  put32(bytes, 0, 0x00fabada);  // BIN magic.
  bytes[254] = char(version);
  put32(bytes, 256, 0x3f800000);  // Scene scale 1.
  put32(bytes, 272, 24);          // Frame rate.
  put32(bytes, 276, 1);           // Declared particle count.
  put32(bytes, headerBytes, 0x3f800000);
  put32(bytes, headerBytes + 4, 0x40000000);
  put32(bytes, headerBytes + 8, 0x40400000);  // Position (1,2,3).
  QByteArray identity;
  if (version >= 12) append64(identity, id);
  else append32(identity, static_cast<std::uint32_t>(id));
  bytes.replace(bytes.size() - identity.size(), identity.size(), identity);
  return bytes;
}

struct PartioRelease {
  void operator()(Partio::ParticlesInfo *value) const {
    if (value) value->release();
  }
};

void rejectBinData(const QString &path) {
  // Exercise the full reader directly too: the adapter normally rejects short
  // payloads during readHeaders, before this separate per-row safety guard runs.
  const QByteArray filename = QFile::encodeName(path);
  std::ostringstream diagnostics;
  std::unique_ptr<Partio::ParticlesDataMutable, PartioRelease> data(
      Partio::read(filename.constData(), true, diagnostics));
  require(!data, QFileInfo(path).fileName().toStdString() + ": BIN data reader accepted malformed data");
  require(!diagnostics.str().empty(), "BIN data reader omitted its diagnostic");
}

void bin(const QDir &dir) {
  QString error;
  for (int version = 1; version <= 13; ++version) {
    const QString path = dir.filePath(QString("valid-v%1.bin").arg(version));
    writeRaw(path, binFrame(version));
    const auto inspected = inspectFrame(path, error);
    require(inspected && inspected->particleCount == 1,
            "Cannot inspect BIN v" + std::to_string(version) + ": " + error.toStdString());
    const auto loaded = loadFrame(path, Mapping{}, error);
    require(loaded && loaded->particles.size() == 1 &&
                loaded->particles[0].id == 42 &&
                loaded->particles[0].position[0] == 1 &&
                loaded->particles[0].position[2] == 3,
            "BIN v" + std::to_string(version) + " changed particle data: " + error.toStdString());
  }

  // Header-only scanning must accept empty files and optional trailing RF data.
  QByteArray empty = binFrame(11).left(356);
  put32(empty, 276, 0);
  writeRaw(dir.filePath("empty.bin"), empty);
  const auto noParticles = loadFrame(dir.filePath("empty.bin"), Mapping{}, error);
  require(noParticles && noParticles->particles.empty(), "Empty BIN file failed");
  writeRaw(dir.filePath("with-trailer.bin"), binFrame(11) + QByteArray(6, '\0'));
  require(bool(loadFrame(dir.filePath("with-trailer.bin"), Mapping{}, error)),
          "BIN optional trailing data was rejected");

  // Cover both sides of the v6/v7 header-size boundary and the later ID width.
  for (int version : {2, 6, 7, 11, 12, 13}) {
    const QByteArray bytes = binFrame(version);
    const QString path = dir.filePath(QString("short-v%1.bin").arg(version));
    reject(path, bytes.left(bytes.size() - 1));
    rejectBinData(path);
  }
  const QString missingRow = dir.filePath("missing-row.bin");
  reject(missingRow, binFrame(11).left(356));
  rejectBinData(missingRow);
  const QByteArray missingName = QFile::encodeName(missingRow);
  std::ostringstream diagnostics;
  std::unique_ptr<Partio::ParticlesInfo, PartioRelease> headers(
      Partio::readHeaders(missingName.constData(), true, diagnostics));
  require(!headers, "BIN readHeaders accepted a missing particle payload");

  reject(dir.filePath("short-header-v6.bin"), binFrame(6).left(319));
  reject(dir.filePath("short-header-v7.bin"), binFrame(7).left(355));
  for (int version : {0, 14, 65535}) {
    QByteArray bytes = binFrame(11);
    bytes[254] = char(version & 255);
    bytes[255] = char((version >> 8) & 255);
    reject(dir.filePath(QString("invalid-version-%1.bin").arg(version)), bytes);
  }
  for (std::uint32_t count : {UINT32_C(0xffffffff), UINT32_C(10000001)}) {
    QByteArray bytes = binFrame(11);
    put32(bytes, 276, count);
    reject(dir.filePath(QString("invalid-count-%1.bin").arg(qulonglong(count))), bytes);
  }
  QByteArray secondRow = binFrame(11);
  put32(secondRow, 276, 2);
  reject(dir.filePath("missing-second-row.bin"), secondRow);

  // Legacy signed IDs retain their sign; v12+ unsigned IDs must fit INT32_MAX.
  for (std::int32_t id : {std::int32_t(-1), (std::numeric_limits<std::int32_t>::min)()}) {
    const QString path = dir.filePath(QString("signed-%1.bin").arg(qlonglong(id)));
    writeRaw(path, binFrame(11, static_cast<std::uint32_t>(id)));
    const auto loaded = loadFrame(path, Mapping{}, error);
    require(loaded && loaded->particles[0].id == id, "Legacy signed BIN ID changed");
  }
  const std::uint64_t largestId = (std::numeric_limits<std::int32_t>::max)();
  writeRaw(dir.filePath("largest-v12-id.bin"), binFrame(12, largestId));
  const auto largest = loadFrame(dir.filePath("largest-v12-id.bin"), Mapping{}, error);
  require(largest && largest->particles[0].id == std::int64_t(largestId),
          "BIN v12 rejected an exactly representable ID");
  for (std::uint64_t id : {largestId + 1, UINT64_C(0x10000002a)}) {
    const QString path = dir.filePath(QString("wide-id-%1.bin").arg(qulonglong(id)));
    reject(path, binFrame(12, id));
    rejectBinData(path);
  }
}

QByteArray ptcHeader(std::uint64_t count = UINT64_C(0x3ff0000000000000)) {
  QByteArray bytes;
  append32(bytes, 0x63747070);  // Little-endian PTC magic: "pptc".
  append32(bytes, 0);           // PTC v0.
  append64(bytes, count);       // Particle count is stored as a double.
  // Bounding box, two 4x4 matrices, and image width/height/depth.
  for (int i = 0; i < 6 + 16 + 16 + 3; ++i) append32(bytes, 0);
  append32(bytes, 1);           // One extra channel.
  append32(bytes, 1);           // One float per particle in that channel.
  return bytes;
}

QByteArray ptcPayload() {
  QByteArray bytes;
  for (int i = 0; i < 3; ++i) append32(bytes, 0);  // Position.
  append32(bytes, 0xffffffff);                     // Zero normal sentinel.
  append32(bytes, 0x3f800000);                     // Radius 1.
  append32(bytes, 0x3f000000);                     // Density 0.5.
  return bytes;
}

void ptc(const QDir &dir) {
  const QByteArray header = ptcHeader();
  reject(dir.filePath("truncated-spec.ptc"), header + "float density");
  reject(dir.filePath("missing-name.ptf"), header + "float \n");
  reject(dir.filePath("missing-separator.ptc"), header + "float\n");
  reject(dir.filePath("short-ptc-header.ptc"), header.left(header.size() - 1));
  reject(dir.filePath("short-ptc-payload.ptc"),
         header + "float density\n" + ptcPayload().left(21));
  reject(dir.filePath("fractional-count.ptc"),
         ptcHeader(UINT64_C(0x3ff8000000000000)) + "float density\n" + ptcPayload());
  reject(dir.filePath("nan-count.ptc"),
         ptcHeader(UINT64_C(0x7ff8000000000000)) + "float density\n" + ptcPayload());

  const QString path = dir.filePath("valid.ptc");
  writeRaw(path, header + "float density\n" + ptcPayload());
  QString error;
  const auto loaded = loadFrame(path, Mapping{}, error);
  require(loaded && loaded->particles.size() == 1, error.toStdString());
  bool density = false;
  for (const auto &attribute : loaded->attributes)
    if (attribute.name == "density")
      density = attribute.floats.size() == 1 && attribute.floats[0] == 0.5f;
  require(density, "Valid PTC channel did not survive parsing");
  for (const char *format : {"ptc", "ptf"}) {
    WriteOptions options;
    options.format = format;
    const bool written = writeFrame(dir.filePath(QString("roundtrip.") + format),
                                    *loaded, options, error);
    require(written, "Cannot export PTC roundtrip: " + error.toStdString());
  }
}

// Big-endian fixtures for the classic Houdini and Maya binary formats.
void be16(QByteArray &bytes, std::uint16_t value) {
  bytes.append(char(value >> 8)).append(char(value & 255));
}

void be32(QByteArray &bytes, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    bytes.append(char((value >> shift) & 255));
}

void beFloat(QByteArray &bytes, float value) {
  std::uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  be32(bytes, bits);
}

void beDouble(QByteArray &bytes, double value) {
  std::uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  be32(bytes, std::uint32_t(bits >> 32));
  be32(bytes, std::uint32_t(bits));
}

// Both Partio entry points must fail on their own, not only through the
// adapter's header checks: readHeaders when fullOnly is false, and read.
void rejectReaders(const QString &path, bool fullOnly = false) {
  const QByteArray filename = QFile::encodeName(path);
  const std::string label   = QFileInfo(path).fileName().toStdString();
  std::ostringstream diagnostics;
  std::unique_ptr<Partio::ParticlesDataMutable, PartioRelease> data(
      Partio::read(filename.constData(), true, diagnostics));
  require(!data, label + ": Partio::read accepted malformed data");
  if (fullOnly) return;
  std::unique_ptr<Partio::ParticlesInfo, PartioRelease> headers(
      Partio::readHeaders(filename.constData(), true, diagnostics));
  require(!headers, label + ": Partio::readHeaders accepted malformed data");
  require(!diagnostics.str().empty(),
          label + ": reader omitted its diagnostic");
}

void rejectAll(const QString &path, const QByteArray &bytes,
               bool fullOnly = false) {
  reject(path, bytes);
  rejectReaders(path, fullOnly);
}

const Attribute *attribute(const Snapshot &snapshot, const char *name) {
  for (const auto &a : snapshot.attributes)
    if (a.name == name) return &a;
  return nullptr;
}

struct BgeoAttr {
  QByteArray name;
  std::uint16_t size;
  int type;
  QList<QByteArray> strings;  // Index (type 4) dictionaries.
};

QByteArray bgeoHeader(int points, int prims, int pointAttrs, int primAttrs,
                      int details) {
  QByteArray bytes("BgeoV");  // Magic and version character.
  for (int value : {5, points, prims, 0, 0, pointAttrs, 0, primAttrs, details})
    be32(bytes, std::uint32_t(value));
  return bytes;
}

QByteArray bgeoAttributes(const QList<BgeoAttr> &attributes) {
  QByteArray bytes;
  for (const auto &a : attributes) {
    be16(bytes, std::uint16_t(a.name.size()));
    bytes += a.name;
    be16(bytes, a.size);
    be32(bytes, std::uint32_t(a.type));
    if (a.type == 4) {
      be32(bytes, std::uint32_t(a.strings.size()));
      for (const auto &value : a.strings) {
        be16(bytes, std::uint16_t(value.size()));
        bytes += value;
      }
    } else {
      for (int i = 0; i < a.size; ++i) be32(bytes, 0);
    }
  }
  return bytes;
}

// Two points with an integer id, one particle-system primitive and an
// integer detail attribute, as classic Houdini writes them.
QByteArray bgeoFrame(std::uint32_t primitiveType = 0x8000,
                     std::uint32_t vertices      = 2) {
  QByteArray bytes =
      bgeoHeader(2, 1, 1, 0, 1) + bgeoAttributes({{"id", 1, 1, {}}});
  for (int point = 0; point < 2; ++point) {
    for (float value : {1.0f + point, 2.0f, 3.0f, 1.0f}) beFloat(bytes, value);
    be32(bytes, std::uint32_t(40 + point));
  }
  be32(bytes, primitiveType);
  be32(bytes, vertices);
  be16(bytes, 0);
  be16(bytes, 1);
  bytes += bgeoAttributes({{"count", 1, 1, {}}});
  be32(bytes, 2);
  return bytes + QByteArray("\0\xff", 2);
}

void bgeo(const QDir &dir) {
  QString error;
  const QString validPath = dir.filePath("valid.bgeo");
  writeRaw(validPath, bgeoFrame());
  const auto valid = loadFrame(validPath, Mapping{}, error);
  require(valid && valid->particles.size() == 2 &&
              valid->particles[1].id == 41 &&
              valid->particles[1].position[0] == 2,
          "Valid BGEO points failed: " + error.toStdString());
  const auto *count = attribute(*valid, "count");
  require(
      count && count->fixed && count->integers == std::vector<std::int32_t>{2},
      "Valid BGEO detail attribute was not read");
  // Upstream keeps the points of an unknown primitive and skips detail data.
  writeRaw(dir.filePath("polygon.bgeo"), bgeoFrame(1));
  const auto polygon =
      loadFrame(dir.filePath("polygon.bgeo"), Mapping{}, error);
  require(
      polygon && polygon->particles.size() == 2 && !polygon->warnings.isEmpty(),
      "BGEO with a non-particle primitive lost its points");

  // Unknown and string attribute types: upstream released the particles and
  // returned them anyway (heap use after free).
  for (int type : {2, 3, -7})
    rejectAll(dir.filePath(QString("attr-type-%1.bgeo").arg(type)),
              bgeoHeader(1, 0, 1, 0, 0) +
                  bgeoAttributes({{"foo", 1, type, {}}}) +
                  QByteArray(64, '\0'));
  // An unknown primitive attribute type released a null particle object.
  rejectAll(dir.filePath("primitive-attr.bgeo"),
            bgeoHeader(1, 0, 0, 1, 0) + QByteArray(16, '\0') +
                bgeoAttributes({{"bar", 1, 3, {}}}) + QByteArray(32, '\0'));
  // Duplicate names gave an attribute handle with index -1.
  rejectAll(dir.filePath("duplicate-position.bgeo"),
            bgeoHeader(1, 0, 1, 0, 0) +
                bgeoAttributes({{"position", 1, 4, {"a"}}}) +
                QByteArray(64, '\0'));
  rejectAll(dir.filePath("duplicate-attr.bgeo"),
            bgeoHeader(1, 0, 2, 0, 0) +
                bgeoAttributes({{"foo", 1, 0, {}}, {"foo", 1, 1, {}}}) +
                QByteArray(64, '\0'));
  rejectAll(dir.filePath("duplicate-string.bgeo"),
            bgeoHeader(1, 0, 1, 0, 0) +
                bgeoAttributes({{"name", 1, 4, {"a", "a"}}}) +
                QByteArray(20, '\0'),
            true);
  // A negative point count was skipped as ~2^64 bytes (hang).
  for (int points : {-1, -1000, 10000001})
    rejectAll(dir.filePath(QString("count-%1.bgeo").arg(points)),
              bgeoHeader(points, 0, 0, 0, 0));
  QByteArray negativePrims = bgeoHeader(1, -1, 0, 0, 0);
  rejectAll(dir.filePath("negative-prims.bgeo"),
            negativePrims + QByteArray(16, '\0'));
  QByteArray negativeTable = bgeoHeader(1, 0, 1, 0, 0);
  be16(negativeTable, 4);
  negativeTable += "name";
  be16(negativeTable, 1);
  be32(negativeTable, 4);
  be32(negativeTable, 0xffffffffu);
  rejectAll(dir.filePath("negative-table.bgeo"),
            negativeTable + QByteArray(20, '\0'));
  rejectAll(dir.filePath("zero-size.bgeo"),
            bgeoHeader(1, 0, 1, 0, 0) + bgeoAttributes({{"foo", 0, 0, {}}}) +
                QByteArray(16, '\0'));
  // Truncation anywhere must fail, never load stale or uninitialized points.
  const QByteArray frame = bgeoFrame();
  for (int size : {3, 20, 44, 50, 60, 70, frame.size() - 12, frame.size() - 6,
                   frame.size() - 3})
    rejectAll(dir.filePath(QString("truncated-%1.bgeo").arg(size)),
              frame.left(size));
  QByteArray shortPoints = bgeoHeader(1000, 0, 0, 0, 0);
  for (int i = 0; i < 40; ++i) beFloat(shortPoints, 1);
  rejectAll(dir.filePath("short-points.bhclassic"), shortPoints);
  for (std::uint32_t vertices : {3u, 0x7fffffffu, 0xffffffffu})
    rejectAll(dir.filePath(QString("primitive-vertices-%1.bgeo").arg(vertices)),
              bgeoFrame(0x8000, vertices));
}

struct McChannel {
  QByteArray name;
  QByteArray type;
  std::uint32_t count;
  QByteArray data;
};

QByteArray mcChannel(const McChannel &c,
                     std::uint32_t declaredBytes = 0xffffffffu) {
  QByteArray bytes("CHNM");
  be32(bytes, std::uint32_t(c.name.size() + 1));
  QByteArray name = c.name + QByteArray(1, '\0');
  while (name.size() % 4) name.append('\0');
  bytes += name + "SIZE";
  be32(bytes, 4);
  be32(bytes, c.count);
  bytes += c.type;
  be32(bytes, declaredBytes == 0xffffffffu ? std::uint32_t(c.data.size())
                                           : declaredBytes);
  return bytes + c.data;
}

QByteArray mcFile(const QByteArray &channels, int blockAdjust = 0) {
  QByteArray bytes("FOR4");
  be32(bytes, 40);
  bytes += "CACHVRSN";
  be32(bytes, 4);
  bytes += QByteArray("0.1\0STIM", 8);
  be32(bytes, 4);
  be32(bytes, 250);
  bytes += "ETIM";
  be32(bytes, 4);
  be32(bytes, 250);
  bytes += "FOR4";
  be32(bytes, std::uint32_t(channels.size() + 4 + blockAdjust));
  return bytes + "MYCH" + channels;
}

QByteArray doubles(std::initializer_list<double> values) {
  QByteArray bytes;
  for (double value : values) beDouble(bytes, value);
  return bytes;
}

QByteArray floats(std::initializer_list<float> values) {
  QByteArray bytes;
  for (float value : values) beFloat(bytes, value);
  return bytes;
}

void mc(const QDir &dir) {
  const McChannel id{"shape_id", "DBLA", 2, doubles({7, 9})};
  const McChannel count{"shape_count", "DBLA", 1, doubles({2})};
  const McChannel position{"shape_position", "FVCA", 2,
                           floats({1, 2, 3, 4, 5, 6})};
  const QByteArray channels =
      mcChannel(id) + mcChannel(count) + mcChannel(position);
  QString error;
  writeRaw(dir.filePath("valid.mc"), mcFile(channels));
  const auto valid = loadFrame(dir.filePath("valid.mc"), Mapping{}, error);
  require(
      valid && valid->particles.size() == 2 && valid->particles[1].id == 9 &&
          valid->particles[1].position[2] == 6 && !attribute(*valid, "count"),
      "Valid MC cache failed: " + error.toStdString());

  // A truncated channel left tellg() at -1 and the scan never terminated.
  QByteArray truncatedName = mcFile(QByteArray()).left(52);
  be32(truncatedName, 1000);
  truncatedName += "MYCHCHNM";
  be32(truncatedName, 8);
  rejectAll(dir.filePath("truncated-name.mc"), truncatedName + "a_id");
  rejectAll(dir.filePath("truncated-size.mc"),
            truncatedName + QByteArray("a_id\0\0\0\0", 8));
  for (int size : {6, 30, 56, 70, 90, channels.size() + 50})
    rejectAll(dir.filePath(QString("truncated-%1.mc").arg(size)),
              mcFile(channels).left(size));
  // The name buffer used to be read as an unterminated C string.
  QByteArray unterminated = QByteArray("CHNM");
  be32(unterminated, 4);
  unterminated += "abcd";
  rejectAll(dir.filePath("unterminated-name.mc"),
            mcFile(unterminated + mcChannel(id)));
  rejectAll(dir.filePath("block-overrun.mc"), mcFile(channels, 8));
  rejectAll(dir.filePath("block-underrun.mc"), mcFile(channels, -8));
  rejectAll(dir.filePath("negative-block.mc"),
            mcFile(channels, -int(channels.size()) - 8));
  rejectAll(dir.filePath("channel-bytes.mc"),
            mcFile(mcChannel({"shape_id", "DBLA", 2, doubles({7})}) +
                   mcChannel(position)));
  rejectAll(dir.filePath("channel-overrun.mc"),
            mcFile(mcChannel(id) + mcChannel(position, 4000)));
  rejectAll(dir.filePath("short-channel.mc"),
            mcFile(mcChannel(id) +
                   mcChannel({"shape_mass", "DBLA", 3, doubles({1, 2, 3})})));
  rejectAll(dir.filePath("missing-id.mc"), mcFile(mcChannel(position)));
  rejectAll(dir.filePath("duplicate.mc"),
            mcFile(mcChannel(id) + mcChannel(position) +
                   mcChannel({"other_position", "FVCA", 2,
                              floats({1, 2, 3, 4, 5, 6})})));
  rejectAll(dir.filePath("fractional-id.mc"),
            mcFile(mcChannel({"shape_id", "DBLA", 2, doubles({7, 9.5})}) +
                   mcChannel(position)),
            true);
  rejectAll(dir.filePath("huge-value.mc"),
            mcFile(channels +
                   mcChannel({"shape_mass", "DBLA", 2, doubles({1, 1e300})})),
            true);
  QByteArray badHeader = mcFile(channels);
  put32(badHeader, 4,
        0xfcffffffu);  // Header size -4 (little-endian bytes of 0xfffffffc).
  rejectAll(dir.filePath("negative-header.mc"), badHeader);
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir temporary;
    require(temporary.isValid(), "Cannot create format test directory");
    const QDir dir(temporary.path());
    geo(dir);
    ptc(dir);
    bin(dir);
    bgeo(dir);
    mc(dir);
    std::cout << "Particle format readers: long GEO strings and strict GEO/PTC/BIN/BGEO/MC inputs passed.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Particle format reader failure: " << error.what() << '\n';
    return 1;
  }
}
