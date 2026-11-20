#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

class ToonzScene;
class TOutputProperties;

namespace GroundzeroAutomation {

// The output profiles render.submit admits (contract w3-fidelity §2).
enum class RenderProfile { Rgba8Png, Opaque8Mp4, RgbaHalfExr, RgbaFloatExr };

struct ProfileInfo final {
  RenderProfile id;
  const char* name;
  const char* extension;
  // The pipeline channel depth profile_default renders at: 32, 64 or 128 bpp.
  int defaultBpp;
  int reserveBytesPerPixel;
  qint64 frameFileLimit;
  // "uint8", "half" or "float".
  const char* sample;
  // "straight", "opaque" or "premultiplied".
  const char* alpha;
  bool movie;
  // The writer stores linear-light values (EXR); otherwise power-curve coded.
  bool linearOutput;
};

const ProfileInfo& profileInfo(RenderProfile profile);
// The profile a request names; invalid_params for any other text.
RenderProfile profileFromText(const QString& text);
QStringList profileNames();
// Whether `profile` writes a numbered image sequence (not a movie).
bool profileIsSequence(RenderProfile profile);

// The channel depth and color space a render computes in.
struct RenderPipeline final {
  // "profile_default" or "scene".
  QString source = "profile_default";
  int bpp        = 32;
  bool linear    = false;
  double gamma   = 2.2;
};

// What the scene stores and the worker does not apply, read before the render
// snapshot is configured.
struct SceneOutputFacts final {
  int bpp            = 32;
  bool linear        = false;
  double gamma       = 2.2;
  double renderGamma = 1.0;
  double stretchFrom = 1.0;
  double stretchTo   = 1.0;
  int shrink         = 1;
  int field          = 0;
  bool stereoscopic  = false;
  int multimedia     = 0;
  bool clapperboard  = false;
  int aspectMode     = 0;
  double aspectRatio = 1.0;
  bool sound         = false;
};

SceneOutputFacts readSceneOutputFacts(ToonzScene& scene);
RenderPipeline resolvePipeline(RenderProfile profile, const QString& source,
                               const SceneOutputFacts& scene);

// An effect the render evaluates and whether it computes in float.
struct EffectPrecision final {
  QString fxId;
  QString memberKey;
  QString type;
  bool float32 = false;
};

struct Conversion final {
  QString kind;
  bool avoidable = false;
  bool accepted  = true;
  QString fxId;
  QString memberKey;
  QString setting;
  // Effects a summary row stands for, past EffectRowLimit; 0 on other rows.
  int count = 0;

  QJsonObject toJson() const;
};

// Effects listed one row each before the rest share a counted row.
constexpr int EffectRowLimit = 16;

// Scene output settings the request does not apply, each {setting, scene};
// `requestAspectMode` and `requestAspectRatio` are the explicit aspect export.
QJsonArray outputOmissions(const SceneOutputFacts& scene, int requestAspectMode,
                           double requestAspectRatio);

// Every conversion a render of `profile` through `pipeline` makes, inherent
// first, then avoidable ones marked accepted when `accepted` names their kind.
std::vector<Conversion> renderConversions(
    RenderProfile profile, const RenderPipeline& pipeline,
    const std::vector<EffectPrecision>& effects, const QJsonArray& omissions,
    const QStringList& accepted);
// The avoidable conversion kinds every conversion name may list.
QStringList avoidableConversionKinds();

// effective.color: the pipeline, the written output and the scene's settings.
QJsonObject colorReport(RenderProfile profile, const RenderPipeline& pipeline,
                        const SceneOutputFacts& scene);

// What a written EXR file's header says (contract w3-fidelity §5).
struct ExrFacts final {
  int width  = 0;
  int height = 0;
  QString sample;
  QStringList channels;
  int compression = -1;
};

// Read and check an EXR file's header, line offset table and chunk framing
// without decoding pixels; invalid_artifact on any departure from a
// single-part scanline file.
ExrFacts readExrFacts(const QByteArray& data);

// Storage the render lane may reserve (contract w3-fidelity §6).
constexpr qint64 RenderJobBytes        = 2LL * 1024 * 1024 * 1024;
constexpr qint64 RenderWorkerBytes     = 4LL * 1024 * 1024 * 1024;
constexpr qint64 RenderFreeSpaceMargin = 1LL * 1024 * 1024 * 1024;

#ifdef BUILD_UI_PORT_TESTS
void runAutomationRenderProfileTests();
#endif

}  // namespace GroundzeroAutomation
