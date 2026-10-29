#include "toonz/particlecache.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace ParticleCache;

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void near(double actual, double expected, const char *message) {
  require(std::abs(actual - expected) < 1e-5, message);
}

Snapshot fixture(double displacement = 0) {
  Snapshot snapshot;
  snapshot.hasStableIds = true;
  snapshot.hasVelocity = true;
  snapshot.hasArtworkFrame = true;
  Particle p;
  p.id = 42; p.position = {{1 + displacement, 2, 3}};
  p.velocity = {{2, 0, 0}}; p.size = 2; p.rotation = 350;
  p.tint = {{0.25, 0.5, 0.75, 0.5}};
  p.age = 0.5; p.lifetime = 3; p.artworkIndex = 2; p.artworkFrame = 7;
  snapshot.particles.push_back(p);
  p.id = 9; p.position[0] = 10 + displacement;
  snapshot.particles.push_back(p);
  return snapshot;
}

void writeJson(const QString &path, const QJsonObject &json) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly), "open JSON fixture");
  const QByteArray bytes = QJsonDocument(json).toJson();
  require(file.write(bytes) == bytes.size(), "write JSON fixture");
}

void capabilitiesAndMappings() {
  bool mc = false, rib = false, bgeo = false;
  for (const auto &f : capabilities()) {
    if (f.extension == "mc") mc = f.readable && !f.writable;
    if (f.extension == "rib") rib = !f.readable && f.writable;
    if (f.extension == "bgeo") bgeo = f.readable && f.writable;
    require(f.extension != "sc", "modern compressed BGEO is not supported");
  }
  require(mc && rib && bgeo, "format direction registry");
  Mapping original, loaded;
  original.axes = {{2, 0, 1}}; original.axisSigns = {{-1, 1, -1}};
  original.units = 10; original.color = "colour";
  QString error;
  require(mappingFromJson(mappingToJson(original), loaded, error), "mapping round-trip");
  require(loaded.axes == original.axes && loaded.axisSigns == original.axisSigns &&
          loaded.units == 10 && loaded.color == "colour", "mapping values preserved");
  QJsonObject invalid = mappingToJson(original);
  invalid["axes"] = QJsonArray{0, 0, 2};
  require(!mappingFromJson(invalid, loaded, error), "duplicate coordinate axes rejected");
  invalid = mappingToJson(original); invalid["units"] = -1;
  require(!mappingFromJson(invalid, loaded, error), "negative units rejected");
}

void interpolation() {
  auto a = fixture(); auto b = fixture(10);
  b.particles[0].rotation = 10;
  std::swap(b.particles[0], b.particles[1]);
  QString error;
  const auto middle = interpolate(a, b, 0.5, error);
  require(bool(middle), "interpolation succeeds");
  near(middle->particles[0].position[0], 6, "IDs match despite particle reorder");
  near(middle->particles[0].rotation, 360, "rotation uses shortest arc");
  b.particles.pop_back();
  auto birth = b.particles.front(); birth.id = 500; b.particles.push_back(birth);
  const auto membership = interpolate(a, b, 0.5, error);
  require(membership && membership->particles.size() == 2 &&
          membership->particles[0].id == 42, "birth/death preserves left sample membership");
  b.particles[1].id = b.particles[0].id;
  require(!interpolate(a, b, 0.5, error), "duplicate IDs reject interpolation");
  a.hasStableIds = false;
  const auto held = interpolate(a, b, 0.5, error);
  require(bool(held), "missing IDs hold safely");
  near(held->particles[0].position[0], 1, "missing IDs do not interpolate array order");
  a.hasStableIds = true; a.extra["native"] = true;
  require(bool(interpolate(a, b, 0.5, error)), "native draws hold rather than interpolate particles");
}

void roundTrips(const QDir &dir) {
  QString error;
  WriteOptions options;
  const QString path = dir.filePath("cache.0001.bgeo");
  require(writeFrame(path, fixture(), options, error), qPrintable(error));
  require(!writeFrame(path, fixture(100), options, error), "existing frame is never overwritten");
  const auto loaded = loadFrame(path, Mapping{}, error);
  require(loaded && loaded->particles.size() == 2, "BGEO frame load");
  const auto inspection = inspectFrame(path, error);
  require(inspection && inspection->particleCount == 2 && inspection->particles.empty() &&
          !inspection->attributes.empty(), "inspect discovers schema without a position mapping");
  near(loaded->particles[0].position[0], 1, "position round-trip");
  near(loaded->particles[0].tint[3], 0.5, "opacity round-trip");
  require(loaded->particles[0].id == 42 && loaded->particles[0].artworkIndex == 2,
          "identity and artwork round-trip");
  Mapping remap; remap.axes = {{1, 0, 2}}; remap.axisSigns = {{-1, 1, 1}};
  remap.units = 2;
  const auto transformed = loadFrame(path, remap, error);
  require(bool(transformed), "coordinate mapping loads");
  near(transformed->particles[0].position[0], -4, "coordinate mapping applied");
  Snapshot custom = *loaded;
  Attribute weights; weights.name = "customWeights"; weights.components = 2;
  weights.floats = {0.2f, 0.8f, 0.4f, 0.6f};
  custom.attributes.push_back(weights);
  Attribute labels; labels.name = "group"; labels.type = AttributeType::IndexedString;
  labels.strings = QStringList{"leaf", "spark"}; labels.integers = {0, 1};
  custom.attributes.push_back(labels);
  Attribute fixed; fixed.name = "customScale"; fixed.fixed = true; fixed.floats = {12};
  custom.attributes.push_back(fixed);
  options.compressed = true;
  const QString compressed = dir.filePath("custom.bgeo.gz");
  require(writeFrame(compressed, custom, options, error), qPrintable(error));
  const auto restored = loadFrame(compressed, Mapping{}, error);
  require(restored && restored->attributes.size() == custom.attributes.size(),
          "unknown attributes and fixed metadata retained");
  bool strings = false, global = false;
  for (const auto &attribute : restored->attributes) {
    if (attribute.name == "group") strings = attribute.strings == labels.strings && attribute.integers == labels.integers;
    if (attribute.name == "customScale") global = attribute.fixed && attribute.floats == fixed.floats;
  }
  require(strings && global, "indexed strings and fixed metadata round-trip");
  // Exercise the real zlib PRT path on every platform, including Windows where
  // the pinned upstream default otherwise compiles unsupported-format stubs.
  Snapshot prt = fixture();
  Attribute prtPosition; prtPosition.name = "position";
  prtPosition.type = AttributeType::Vector; prtPosition.components = 3;
  prtPosition.floats = {1, 2, 3, 10, 2, 3};
  Attribute prtId; prtId.name = "id"; prtId.type = AttributeType::Int;
  prtId.integers = {42, 9};
  Attribute prtTail; prtTail.name = "tailVector";
  prtTail.type = AttributeType::Vector; prtTail.components = 3;
  prtTail.floats = {0.25f, 0.5f, 0.75f, 1.25f, 1.5f, 1.75f};
  // Last-channel arity exercises the upstream stride bug; mixed-case names
  // must survive because an OT mapping identifies the exact attribute name.
  prt.attributes = {prtPosition, prtId, prtTail};
  WriteOptions prtOptions; prtOptions.format = "prt";
  const QString prtPath = dir.filePath("particles.prt");
  require(writeFrame(prtPath, prt, prtOptions, error), qPrintable(error));
  const auto prtLoaded = loadFrame(prtPath, Mapping{}, error);
  require(prtLoaded && prtLoaded->particles.size() == 2 &&
          prtLoaded->particles[0].id == 42, "PRT decoder preserves particle identities");
  near(prtLoaded->particles[1].position[0], 10, "PRT zlib payload round-trip");
  bool tailPreserved = false;
  for (const auto &a : prtLoaded->attributes)
    if (a.name == "tailVector") tailPreserved = a.floats == prtTail.floats;
  require(tailPreserved, "PRT final vector channel and exact name preserved");
  Snapshot empty;
  require(writeFrame(dir.filePath("empty.prt"), empty, prtOptions, error), qPrintable(error));
  const auto emptyPrt = loadFrame(dir.filePath("empty.prt"), Mapping{}, error);
  require(emptyPrt && emptyPrt->particles.empty(), "empty PRT cache round-trip");
  QFile prtFile(prtPath);
  require(prtFile.open(QIODevice::ReadOnly), "open PRT mutation fixture");
  const QByteArray validPrt = prtFile.readAll();
  prtFile.close();
  auto rejectPrt = [&](const char *name, QByteArray bytes) {
    QFile file(dir.filePath(name));
    require(file.open(QIODevice::WriteOnly), "create malformed PRT fixture");
    require(file.write(bytes) == bytes.size(), "write malformed PRT fixture");
    file.close();
    require(!loadFrame(file.fileName(), Mapping{}, error), "malformed PRT must fail safely");
  };
  // v1 header 56 bytes, followed by reserved/count/entry-size (12 bytes).
  require(validPrt.size() > 112, "PRT fixture has a channel table");
  QByteArray malformedType = validPrt;
  malformedType[100] = char(0xff);  // first channel type (68 + 32)
  rejectPrt("invalid-type.prt", malformedType);
  QByteArray malformedName = validPrt;
  for (int i = 68; i < 100; ++i) malformedName[i] = 'x';
  rejectPrt("invalid-name.prt", malformedName);
  QByteArray malformedArity = validPrt;
  for (int i = 104; i < 108; ++i) malformedArity[i] = char(0xff);
  rejectPrt("invalid-arity.prt", malformedArity);
  rejectPrt("truncated.prt", validPrt.left(validPrt.size() - 4));
  WriteOptions refreshed; refreshed.preserveOriginalAttributes = false;
  require(writeFrame(dir.filePath("canonical.bgeo"), custom, refreshed, error), qPrintable(error));
  const auto canonical = loadFrame(dir.filePath("canonical.bgeo"), Mapping{}, error);
  require(canonical && canonical->attributes.size() == custom.attributes.size(),
          "canonical refresh preserves nonclashing custom attributes");
  require(!loadFrame(dir.filePath("missing.bgeo"), Mapping{}, error), "missing frame fails");
  QFile malformed(dir.filePath("invalid.bgeo"));
  require(malformed.open(QIODevice::WriteOnly), "open malformed fixture");
  malformed.write("not a particle cache"); malformed.close();
  require(!loadFrame(malformed.fileName(), Mapping{}, error), "invalid format rejected");
  Snapshot invalid = fixture(); invalid.particles[0].position[0] = std::numeric_limits<double>::infinity();
  options.compressed = false;
  require(!writeFrame(dir.filePath("infinite.bgeo"), invalid, options, error), "nonfinite export rejected");
  invalid = fixture(); invalid.particles[0].id = std::int64_t(1) << 40;
  require(!writeFrame(dir.filePath("wide-id.bgeo"), invalid, options, error), "wide integer ID does not silently truncate");
  require(!writeFrame(dir.filePath("cancelled.bgeo"), fixture(), options, error, [] { return true; }),
          "cancelled export fails");
  require(!QFileInfo::exists(dir.filePath("cancelled.bgeo")), "cancelled export leaves no frame");
  require(QFile::remove(path), "remove fixture for replacement");
  require(writeFrame(path, fixture(20), options, error), "replace cached filename");
  const auto changed = loadFrame(path, Mapping{}, error);
  require(bool(changed), "changed file loads");
  near(changed->particles[0].position[0], 21, "file replacement invalidates cached frame");
  setMemoryCacheLimit(0);
  const auto uncached = loadFrame(path, Mapping{}, error);
  require(bool(uncached), "zero memory cache still loads");
  setMemoryCacheLimit(256 * 1024 * 1024);
}

void packages(const QDir &dir) {
  Metadata metadata; metadata.firstFrame = 10; metadata.lastFrame = 12;
  metadata.framesPerSecond = 30;
  QString error;
  WriteOptions options;
  const QString package = dir.filePath("package");
  int progress = 0;
  auto provider = [](int frame, QString &) -> SnapshotPtr {
    return std::make_shared<Snapshot>(fixture(frame - 10));
  };
  require(writePackage(package, metadata, provider, options, error, {},
                       [&](int completed, int total) { progress = completed; require(total == 3, "progress total"); }),
          qPrintable(error));
  require(progress == 3, "all package frames completed");
  Sequence sequence;
  require(openSequence(package, sequence, error), "open package directory");
  require(sequence.frames.size() == 3 && sequenceFiles(sequence).size() == 4,
          "package dependencies enumerated");
  Playback playback; playback.sceneFramesPerSecond = 30;
  auto middle = sampleSequence(sequence, 0.5, playback, sequence.mapping, error);
  require(bool(middle), "sample package subframe");
  near(middle->particles[0].position[0], 1.5, "scene FPS and source first frame mapping");
  playback.end = EndBehavior::Loop;
  auto loop = sampleSequence(sequence, 3, playback, sequence.mapping, error);
  require(bool(loop), "loop sample");
  near(loop->particles[0].position[0], 1, "loop wraps to source start");
  playback.speed = -1; playback.offset = 2;
  auto reverse = sampleSequence(sequence, 1, playback, sequence.mapping, error);
  require(bool(reverse), "reverse sample");
  near(reverse->particles[0].position[0], 2, "reverse time mapping");
  require(!writePackage(package, metadata, provider, options, error), "package overwrite rejected");
  const QString cancelledPath = dir.filePath("cancelled-package");
  int frames = 0;
  require(!writePackage(cancelledPath, metadata, [&](int frame, QString &failure) {
    ++frames; return provider(frame, failure);
  }, options, error, [&] { return frames >= 1; }), "mid-package cancellation");
  require(!QFileInfo::exists(cancelledPath), "cancelled package is unpublished");
  const QString throwingPath = dir.filePath("throwing-package");
  require(!writePackage(throwingPath, metadata, [](int, QString &) -> SnapshotPtr {
    throw std::runtime_error("expected frame provider failure");
  }, options, error), "frame provider exception reported");
  require(!QFileInfo::exists(throwingPath), "throwing provider leaves package unpublished");
  WriteOptions rib; rib.format = "rib";
  const QString ribPath = dir.filePath("rib-export");
  require(!writePackage(ribPath, metadata, provider, rib, error), "write-only export needs explicit consent");
  rib.allowLossy = true;
  require(writePackage(ribPath, metadata, provider, rib, error), qPrintable(error));
  require(QFileInfo::exists(QDir(ribPath).filePath("export.json")) &&
          !QFileInfo::exists(QDir(ribPath).filePath("cache.otpc")),
          "write-only export never advertises a playable cache");
  Sequence raw;
  require(openSequence(sequence.frames.begin()->second, raw, error) && raw.frames.size() == 3,
          "numbered raw sequence discovery");
  QJsonObject bad{{"format", "opentoonz-particle-cache"}, {"version", 1},
      {"fps", 24}, {"firstFrame", 1}, {"lastFrame", 2},
      {"frames", QJsonArray{QJsonObject{{"frame", 1}, {"file", "a.bgeo"}},
                             QJsonObject{{"frame", 1}, {"file", "b.bgeo"}}}}};
  writeJson(dir.filePath("invalid.otpc"), bad);
  require(!openSequence(dir.filePath("invalid.otpc"), raw, error), "duplicate manifest frames rejected");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary test directory");
    capabilitiesAndMappings();
    interpolation();
    roundTrips(QDir(temporary.path()));
    packages(QDir(temporary.path()));
    std::cout << "Particle cache core: format directions, schema, interpolation, IO, metadata, invalidation and atomic packages passed.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Particle cache core failure: " << error.what() << '\n';
    return 1;
  }
}
