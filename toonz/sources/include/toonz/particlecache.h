#pragma once

#include "tcommon.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#undef DVAPI
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#else
#define DVAPI DV_IMPORT_API
#endif

namespace ParticleCache {

// Values consumed by the OT sprite renderer. Position/size are in OT world
// units after Mapping; velocity is world units per second, rotation is degrees,
// tint is straight RGBA. IDs are identities, never array offsets.
struct Particle {
  std::int64_t id = 0;
  std::array<double, 3> position{{0, 0, 0}};
  std::array<double, 3> velocity{{0, 0, 0}};
  double size = 1.0;
  double rotation = 0.0;
  std::array<double, 4> tint{{1, 1, 1, 1}};
  double age = 0.0;
  double lifetime = 0.0;
  int artworkIndex = 0;
  double artworkFrame = 0.0;
};

enum class AttributeType { Float, Vector, Int, IndexedString };
// Unmapped attributes are retained, including fixed attributes and string
// dictionaries. Values are flattened particle-major, components per particle.
struct Attribute {
  QString name;
  AttributeType type = AttributeType::Float;
  int components = 1;
  bool fixed = false;
  std::vector<float> floats;
  std::vector<std::int32_t> integers;
  QStringList strings;
};

struct Mapping {
  QString position = "position";
  QString velocity = "velocity";
  QString id = "id";
  QString size = "pscale";
  QString rotation = "rotation";
  QString color = "Cd";
  QString opacity = "opacity";
  QString age = "age";
  QString lifetime = "life";
  QString artworkIndex = "ot_artwork";
  QString artworkFrame = "ot_frame";
  // Output X/Y/Z = input axis[0/1/2] * axisSign[0/1/2].
  std::array<int, 3> axes{{0, 1, 2}};
  std::array<int, 3> axisSigns{{1, 1, 1}};
  double units = 1.0;
  double defaultSize = 1.0;
  bool sizeIsDiameter = false;
  bool rotationInRadians = false;
};

struct Metadata {
  double framesPerSecond = 24.0;
  int firstFrame = 1;
  int lastFrame = 1;
  QString units = "ot-world";
  QStringList artworkPaths;
  QJsonObject extra;
  Mapping mapping;
};

struct Snapshot {
  std::vector<Particle> particles;
  std::size_t particleCount = 0;  // Also set by inspectFrame, which does not map particles.
  std::vector<Attribute> attributes;
  bool hasStableIds = false;
  bool hasVelocity = false;
  bool hasArtworkFrame = false;
  QStringList warnings;
  std::size_t memoryBytes = 0;
  QJsonObject extra;  // Per-frame native draw metadata, if present.
  QString sourceDirectory;  // Base directory for relative metadata asset paths.
  QString revision;  // Opaque content/mapping identity for render-cache aliases.
};
using SnapshotPtr = std::shared_ptr<const Snapshot>;

struct Sequence {
  QString manifestPath;
  std::map<int, QString> frames;  // Absolute decoded paths, indexed by frame.
  Metadata metadata;
  Mapping mapping;
  std::map<int, QJsonObject> frameMetadata;
};

enum class MissingFrame { Error, Hold, Empty };
enum class EndBehavior { Hold, Loop, Empty };
struct Playback {
  // sourceFrame = metadata.firstFrame + (sceneFrame - startFrame) *
  // speed * metadata.framesPerSecond / sceneFramesPerSecond + offset.
  double sceneFramesPerSecond = 24.0;
  double startFrame = 0.0;
  double offset = 0.0;
  double speed = 1.0;
  bool interpolate = true;
  MissingFrame missing = MissingFrame::Error;
  EndBehavior end = EndBehavior::Hold;
};

// The cache frames a sample reads after end, missing-frame and interpolation
// rules: left alone, or left blended toward right by weight. No file is read.
struct SamplePlan {
  bool empty    = false;  // The sample is an empty snapshot.
  int left      = 0;      // Key of Sequence::frames, read unless empty.
  int right     = 0;      // Key blended with left when weight > 0.
  double weight = 0.0;
  QString rightError;  // Right is missing and missing frames are errors.
};

struct FormatCapability {
  QString extension;
  QString description;
  bool readable = false;
  bool writable = false;
};

struct WriteOptions {
  QString format = "bgeo";
  bool compressed = false;
  // Attribute loss is rejected unless explicitly permitted by the caller.
  bool allowLossy = false;
  bool preserveOriginalAttributes = true;
};

using Cancel = std::function<bool()>;
using Progress = std::function<void(int completed, int total)>;
using FrameProvider = std::function<SnapshotPtr(int frame, QString &error)>;

DVAPI std::vector<FormatCapability> capabilities();
DVAPI QJsonObject mappingToJson(const Mapping &mapping);
DVAPI bool mappingFromJson(const QJsonObject &json, Mapping &mapping,
                          QString &error);
DVAPI bool openSequence(const QString &path, Sequence &sequence, QString &error);
DVAPI QStringList sequenceFiles(const Sequence &sequence);
DVAPI SnapshotPtr inspectFrame(const QString &path, QString &error,
                              const Cancel &cancel = {});
DVAPI SnapshotPtr loadFrame(const QString &path, const Mapping &mapping,
                           QString &error, const Cancel &cancel = {});
DVAPI SnapshotPtr sampleSequence(const Sequence &sequence, double sceneFrame,
                                const Playback &playback, const Mapping &mapping,
                                QString &error, const Cancel &cancel = {});
DVAPI SnapshotPtr interpolate(const Snapshot &a, const Snapshot &b, double weight,
                             QString &error);
DVAPI bool planSample(const Sequence &sequence, double sceneFrame,
                      const Playback &playback, SamplePlan &plan,
                      QString &error);

// Writes a new directory containing frames and cache.otpc (or export.json for a
// write-only format). Existing destinations are never overwritten. Only a fully
// validated package is published; write-only formats need explicit allowLossy.
DVAPI bool writePackage(const QString &directory, const Metadata &metadata,
                        const FrameProvider &provider,
                        const WriteOptions &options, QString &error,
                        const Cancel &cancel = {}, const Progress &progress = {});
// Writes a single file atomically; preserves generic attributes when requested.
DVAPI bool writeFrame(const QString &path, const Snapshot &snapshot,
                      const WriteOptions &options, QString &error,
                      const Cancel &cancel = {});
DVAPI bool exportFiles(const Sequence &sequence, const QString &directory,
                       const WriteOptions &options, QString &error,
                       const Cancel &cancel = {}, const Progress &progress = {});

DVAPI void clearMemoryCache();
DVAPI void setMemoryCacheLimit(std::size_t bytes);

}  // namespace ParticleCache
