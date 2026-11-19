#include "automation.h"
#include "automation_render_profile.h"

#include "toutputproperties.h"
#include "trasterfx.h"

#include <QtEndian>

#include <stdexcept>

namespace GroundzeroAutomation {
namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QStringList kinds(const std::vector<Conversion>& conversions,
                  bool avoidableOnly) {
  QStringList result;
  for (const Conversion& conversion : conversions)
    if (!avoidableOnly || conversion.avoidable) result.append(conversion.kind);
  return result;
}

RenderPipeline pipeline(int bpp, const QString& source = "profile_default") {
  RenderPipeline result;
  result.source = source;
  result.bpp    = bpp;
  return result;
}

void profileTable() {
  require(profileNames() == QStringList({"rgba8-png", "opaque8-mp4",
                                         "rgba-half-exr", "rgba-float-exr"}),
          "The profile names changed");
  require(
      profileInfo(RenderProfile::RgbaHalfExr).reserveBytesPerPixel == 8 &&
          profileInfo(RenderProfile::RgbaFloatExr).reserveBytesPerPixel == 16,
      "EXR reservations do not match their sample sizes");
  require(profileInfo(RenderProfile::RgbaHalfExr).defaultBpp == 128,
          "EXR does not render in a float pipeline by default");
  bool refused = false;
  try {
    profileFromText("rgba16-png");
  } catch (const Error& error) {
    refused = error.code == "invalid_params";
  }
  require(refused, "An unadmitted profile was accepted");
  // 120 frames at 2048 x 2048 fit one job's budget in 8-bit PNG.
  const qint64 frames = 120 * (2048LL * 2048 * 4 + 65536);
  require(frames <= RenderJobBytes,
          "The job budget does not hold the journey's sequence");
}

void inherentConversions() {
  const std::vector<EffectPrecision> none;
  require(kinds(renderConversions(RenderProfile::Rgba8Png, pipeline(32), none,
                                  {}, {}),
                false) == QStringList{"depremultiply_truncation"},
          "8-bit PNG lists other than its inherent conversion");
  require(
      kinds(renderConversions(RenderProfile::Opaque8Mp4, pipeline(32), none, {},
                              {}),
            false) == QStringList({"yuv420_subsampling", "lossy_video_codec",
                                   "alpha_flattened_black"}),
      "MP4 inherent conversions changed");
  require(kinds(renderConversions(RenderProfile::RgbaFloatExr, pipeline(128),
                                  none, {}, {}),
                false)
              .isEmpty(),
          "A float EXR from a float pipeline converted something");
}

void avoidableConversions() {
  const std::vector<EffectPrecision> none;
  require(kinds(renderConversions(RenderProfile::Rgba8Png, pipeline(128), none,
                                  {}, {}),
                true) == QStringList({"quantize_to_8bit", "clip_hdr"}),
          "A float pipeline to 8-bit PNG did not quantize and clip");
  require(kinds(renderConversions(RenderProfile::Rgba8Png, pipeline(64), none,
                                  {}, {}),
                true) == QStringList{"quantize_to_8bit"},
          "A 16-bit pipeline to 8-bit PNG did not quantize only");
  require(kinds(renderConversions(RenderProfile::RgbaHalfExr, pipeline(32),
                                  none, {}, {}),
                true) == QStringList{"linearize_quantized"},
          "An 8-bit pipeline to EXR was not linearized at integer depth");
  EffectPrecision integer;
  integer.fxId = "fx_a";
  EffectPrecision floating;
  floating.fxId    = "fx_b";
  floating.float32 = true;
  const std::vector<Conversion> effects =
      renderConversions(RenderProfile::RgbaHalfExr, pipeline(128),
                        {integer, floating}, {}, {"fx_integer_intermediate"});
  require(kinds(effects, true) == QStringList{"fx_integer_intermediate"} &&
              effects.back().fxId == "fx_a" && effects.back().accepted,
          "Only the integer effect is an accepted intermediate");
  const std::vector<EffectPrecision> many(EffectRowLimit + 3, integer);
  const std::vector<Conversion> capped = renderConversions(
      RenderProfile::RgbaFloatExr, pipeline(128), many, {}, {});
  require(capped.size() == size_t(EffectRowLimit + 1) &&
              capped.back().count == 3 && capped.back().fxId.isEmpty(),
          "Effect rows past the limit are not one counted row");
  const std::vector<Conversion> integerOnly = renderConversions(
      RenderProfile::Rgba8Png, pipeline(64), {integer}, {}, {});
  require(!kinds(integerOnly, true).contains("fx_integer_intermediate"),
          "An integer pipeline reported a float intermediate");
}

void omissionsAndSceneSettings() {
  SceneOutputFacts scene;
  require(outputOmissions(scene, 0, 1.0).isEmpty(),
          "Default settings were reported as omitted");
  scene.stretchTo            = 2;
  scene.stereoscopic         = true;
  scene.field                = TRenderSettings::OddField;
  scene.shrink               = 2;
  scene.multimedia           = 1;
  scene.clapperboard         = true;
  scene.renderGamma          = 1.5;
  scene.aspectMode           = TOutputProperties::AspectCrop;
  scene.aspectRatio          = 2.0;
  scene.sound                = true;
  const QJsonArray omissions = outputOmissions(scene, 0, 1.0);
  QStringList settings;
  for (const QJsonValue& row : omissions)
    settings.append(row.toObject().value("setting").toString());
  require(settings ==
              QStringList({"time_stretch", "stereoscopic", "field_prevalence",
                           "shrink", "multimedia", "clapperboard",
                           "render_gamma", "aspect_export", "audio"}),
          "Scene output settings were not all reported");
  require(outputOmissions(scene, TOutputProperties::AspectCrop, 2.0).size() ==
              omissions.size() - 1,
          "A requested aspect equal to the scene's was reported");
  const std::vector<Conversion> scenePipeline = renderConversions(
      RenderProfile::Rgba8Png, pipeline(32, "scene"), {}, omissions, {});
  require(kinds(scenePipeline, true).count("output_setting_ignored") == 8,
          "A scene pipeline did not refuse each ignored setting but audio");
  require(kinds(renderConversions(RenderProfile::Rgba8Png, pipeline(32), {},
                                  omissions, {}),
                true)
              .isEmpty(),
          "profile_default turned omissions into conversions");
  scene.bpp    = 48;
  bool refused = false;
  try {
    resolvePipeline(RenderProfile::Rgba8Png, "scene", scene);
  } catch (const Error& error) {
    refused = error.code == "unsupported_render_settings";
  }
  require(refused, "A scene pipeline accepted an unknown channel depth");
}

void putInt(QByteArray& data, qint32 value) {
  char bytes[4];
  qToLittleEndian(value, bytes);
  data.append(bytes, 4);
}

void attribute(QByteArray& data, const char* name, const char* type,
               const QByteArray& value) {
  data.append(name).append('\0').append(type).append('\0');
  putInt(data, value.size());
  data.append(value);
}

// A 2 x 3 half RGBA scanline file without compression.
QByteArray exrFixture() {
  QByteArray data;
  putInt(data, 20000630);
  putInt(data, 2);
  QByteArray channels;
  for (const char* name : {"A", "B", "G", "R"}) {
    channels.append(name).append('\0');
    putInt(channels, 1);
    channels.append(4, '\0');
    putInt(channels, 1);
    putInt(channels, 1);
  }
  channels.append('\0');
  attribute(data, "channels", "chlist", channels);
  attribute(data, "compression", "compression", QByteArray(1, '\0'));
  QByteArray window;
  for (qint32 value : {0, 0, 1, 2}) putInt(window, value);
  attribute(data, "dataWindow", "box2i", window);
  attribute(data, "lineOrder", "lineOrder", QByteArray(1, '\0'));
  data.append('\0');
  const qint64 table = data.size() + 3 * 8;
  const int chunk    = 8 + 2 * 4 * 2;
  for (int row = 0; row < 3; ++row) {
    char bytes[8];
    qToLittleEndian<quint64>(table + row * chunk, bytes);
    data.append(bytes, 8);
  }
  for (int row = 0; row < 3; ++row) {
    putInt(data, row);
    putInt(data, 16);
    data.append(16, '\0');
  }
  return data;
}

bool exrRefused(const QByteArray& data) {
  try {
    readExrFacts(data);
  } catch (const Error& error) {
    return error.code == "invalid_artifact";
  }
  return false;
}

void exrFacts() {
  const QByteArray fixture = exrFixture();
  const ExrFacts facts     = readExrFacts(fixture);
  require(facts.width == 2 && facts.height == 3 && facts.sample == "half" &&
              facts.channels == QStringList({"A", "B", "G", "R"}) &&
              facts.compression == 0,
          "The EXR fixture's facts were misread");
  require(exrRefused(fixture.left(fixture.size() - 1)),
          "A truncated EXR file was accepted");
  require(exrRefused(fixture + QByteArray(1, '\0')),
          "An EXR file with trailing bytes was accepted");
  QByteArray tiled = fixture;
  tiled[4]         = char(2);
  tiled[5]         = char(2);
  require(exrRefused(tiled), "A tiled EXR file was accepted");
  QByteArray moved   = fixture;
  const int offsets  = fixture.size() - 3 * 24 - 3 * 8;
  moved[offsets + 8] = char(moved[offsets + 8] + 1);
  require(exrRefused(moved), "An inconsistent offset table was accepted");
  require(exrRefused(QByteArray("not an exr file at all")),
          "A non-EXR file was accepted");
}

}  // namespace

void runAutomationRenderProfileTests() {
  profileTable();
  inherentConversions();
  avoidableConversions();
  omissionsAndSceneSettings();
  exrFacts();
}

}  // namespace GroundzeroAutomation
