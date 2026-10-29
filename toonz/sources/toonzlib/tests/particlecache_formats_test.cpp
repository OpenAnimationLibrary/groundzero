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
    std::cout << "Particle format readers: long GEO strings and strict GEO/PTC/BIN inputs passed.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Particle format reader failure: " << error.what() << '\n';
    return 1;
  }
}
