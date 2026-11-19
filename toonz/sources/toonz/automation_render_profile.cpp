#include "automation_render_profile.h"

#include "automation.h"

#include "toonz/boardsettings.h"
#include "toonz/sceneproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshsoundcolumn.h"
#include "toutputproperties.h"
#include "trasterfx.h"

#include <QtEndian>

#include <cmath>

namespace GroundzeroAutomation {

namespace {

constexpr qint64 MiB = 1024 * 1024;

const ProfileInfo Profiles[] = {
    {RenderProfile::Rgba8Png, "rgba8-png", "png", 32, 4, 20 * MiB, "uint8",
     "straight", false, false},
    {RenderProfile::Opaque8Mp4, "opaque8-mp4", "mp4", 32, 4, 0, "uint8",
     "opaque", true, false},
    {RenderProfile::RgbaHalfExr, "rgba-half-exr", "exr", 128, 8, 40 * MiB,
     "half", "premultiplied", false, true},
    {RenderProfile::RgbaFloatExr, "rgba-float-exr", "exr", 128, 16, 72 * MiB,
     "float", "premultiplied", false, true}};

// OpenEXR's numbers for the two compressions the profiles write and the lines
// each stores per chunk.
constexpr int ExrMagic       = 20000630;
constexpr int ExrNone        = 0;
constexpr int ExrZip         = 3;
constexpr int ExrHeaderLimit = 65536;

QJsonObject omission(const QString& setting, const QJsonValue& scene) {
  return {{"setting", setting}, {"scene", scene}};
}

QString fieldText(int field) {
  return field == TRenderSettings::OddField ? "odd" : "even";
}

QString aspectText(int mode) {
  static const char* modes[] = {"none", "letterbox", "crop"};
  return mode >= 0 && mode <= 2 ? modes[mode] : "none";
}

void addInherent(std::vector<Conversion>& result, RenderProfile profile) {
  QStringList kinds;
  switch (profile) {
  case RenderProfile::Rgba8Png:
    kinds = QStringList{"depremultiply_truncation"};
    break;
  case RenderProfile::Opaque8Mp4:
    kinds = QStringList{"yuv420_subsampling", "lossy_video_codec",
                        "alpha_flattened_black"};
    break;
  case RenderProfile::RgbaHalfExr:
    kinds = QStringList{"half_float_range"};
    break;
  case RenderProfile::RgbaFloatExr:
    break;
  }
  for (const QString& kind : kinds) {
    Conversion conversion;
    conversion.kind = kind;
    result.push_back(conversion);
  }
}

Conversion avoidable(const QString& kind, const QStringList& accepted) {
  Conversion conversion;
  conversion.kind      = kind;
  conversion.avoidable = true;
  conversion.accepted  = accepted.contains(kind);
  return conversion;
}

class ExrHeaderReader final {
public:
  explicit ExrHeaderReader(const QByteArray& data) : m_data(data) {}

  qint64 position = 0;

  QByteArray take(qint64 count) {
    if (count < 0 || position + count > m_data.size())
      fail("The EXR file is truncated.");
    const QByteArray chunk = m_data.mid(position, count);
    position += count;
    return chunk;
  }

  qint32 int32() {
    return qFromLittleEndian<qint32>(
        reinterpret_cast<const uchar*>(take(4).constData()));
  }

  quint64 uint64() {
    return qFromLittleEndian<quint64>(
        reinterpret_cast<const uchar*>(take(8).constData()));
  }

  QByteArray text() {
    const int end = m_data.indexOf('\0', position);
    if (end < 0 || end - position > 255) fail("An EXR name is malformed.");
    const QByteArray value = m_data.mid(position, end - position);
    position               = end + 1;
    return value;
  }

  [[noreturn]] static void fail(const QString& message) {
    throw Error("invalid_artifact", message);
  }

private:
  const QByteArray& m_data;
};

void readChannels(const QByteArray& value, ExrFacts& facts) {
  ExrHeaderReader reader(value);
  QStringList types;
  for (QByteArray name = reader.text(); !name.isEmpty(); name = reader.text()) {
    const qint32 type = reader.int32();
    reader.take(4);
    const qint32 xSampling = reader.int32();
    const qint32 ySampling = reader.int32();
    if ((type != 1 && type != 2) || xSampling != 1 || ySampling != 1)
      ExrHeaderReader::fail("An EXR channel has an unexpected sample form.");
    facts.channels.append(QString::fromLatin1(name));
    types.append(type == 1 ? "half" : "float");
  }
  types.removeDuplicates();
  if (types.size() != 1)
    ExrHeaderReader::fail("The EXR channels do not share one sample type.");
  facts.sample = types.first();
}

void readWindow(const QByteArray& value, ExrFacts& facts) {
  if (value.size() != 16) ExrHeaderReader::fail("The EXR window is malformed.");
  ExrHeaderReader reader(value);
  const qint32 x0 = reader.int32(), y0 = reader.int32();
  const qint32 x1 = reader.int32(), y1 = reader.int32();
  if (x0 != 0 || y0 != 0 || x1 < x0 || y1 < y0 || x1 >= 65536 || y1 >= 65536)
    ExrHeaderReader::fail("The EXR data window is not the whole frame.");
  facts.width  = x1 + 1;
  facts.height = y1 + 1;
}

void readChunks(ExrHeaderReader& reader, const ExrFacts& facts, qint64 size) {
  const int lines  = facts.compression == ExrZip ? 16 : 1;
  const int chunks = (facts.height + lines - 1) / lines;
  std::vector<quint64> offsets;
  for (int index = 0; index < chunks; ++index)
    offsets.push_back(reader.uint64());
  qint64 expected = reader.position;
  for (int index = 0; index < chunks; ++index) {
    if (offsets[index] != quint64(expected))
      ExrHeaderReader::fail("The EXR line offset table is inconsistent.");
    reader.position    = expected;
    const qint32 y     = reader.int32();
    const qint32 bytes = reader.int32();
    if (y != index * lines || bytes <= 0)
      ExrHeaderReader::fail("An EXR chunk is out of place.");
    reader.take(bytes);
    expected = reader.position;
  }
  if (expected != size)
    ExrHeaderReader::fail("The EXR file has bytes after its last chunk.");
}

}  // namespace

const ProfileInfo& profileInfo(RenderProfile profile) {
  for (const ProfileInfo& info : Profiles)
    if (info.id == profile) return info;
  throw Error("internal_error", "Unknown render profile.");
}

RenderProfile profileFromText(const QString& text) {
  for (const ProfileInfo& info : Profiles)
    if (text == info.name) return info.id;
  throw Error("invalid_params", "Unknown render profile.",
              {{"profile", text},
               {"profiles", QJsonArray::fromStringList(profileNames())}});
}

QStringList profileNames() {
  QStringList names;
  for (const ProfileInfo& info : Profiles) names.append(info.name);
  return names;
}

bool profileIsSequence(RenderProfile profile) {
  return !profileInfo(profile).movie;
}

SceneOutputFacts readSceneOutputFacts(ToonzScene& scene) {
  TOutputProperties& output     = *scene.getProperties()->getOutputProperties();
  const TRenderSettings& stored = output.getRenderSettings();
  SceneOutputFacts facts;
  facts.bpp          = stored.m_bpp;
  facts.linear       = stored.m_linearColorSpace;
  facts.gamma        = stored.m_colorSpaceGamma;
  facts.renderGamma  = stored.m_gamma;
  facts.stretchFrom  = stored.m_timeStretchFrom;
  facts.stretchTo    = stored.m_timeStretchTo;
  facts.shrink       = stored.m_shrinkX;
  facts.field        = stored.m_fieldPrevalence;
  facts.stereoscopic = stored.m_stereoscopic;
  facts.multimedia   = output.getMultimediaRendering();
  facts.clapperboard =
      output.getBoardSettings() && output.getBoardSettings()->isActive();
  facts.aspectMode  = output.getAspectMode();
  facts.aspectRatio = output.getAspectRatio();
  TXsheet* sheet    = scene.getXsheet();
  for (int index = 0; index < sheet->getColumnCount(); ++index) {
    TXshColumn* column = sheet->getColumn(index);
    if (column && column->getSoundColumn() && !column->isEmpty())
      facts.sound = true;
  }
  return facts;
}

RenderPipeline resolvePipeline(RenderProfile profile, const QString& source,
                               const SceneOutputFacts& scene) {
  RenderPipeline pipeline;
  pipeline.source = source;
  if (source == "scene") {
    if (scene.bpp != 32 && scene.bpp != 64 && scene.bpp != 128)
      throw Error("unsupported_render_settings",
                  "The scene's stored channel depth is not 32, 64 or 128 bits "
                  "per pixel.",
                  {{"reason", "scene_bpp"}, {"bpp", scene.bpp}});
    if (!(scene.gamma >= 0.1 && scene.gamma <= 10))
      throw Error("unsupported_render_settings",
                  "The scene's stored color space gamma is outside 0.1 to 10.",
                  {{"reason", "scene_gamma"}});
    pipeline.bpp    = scene.bpp;
    pipeline.linear = scene.linear;
    pipeline.gamma  = scene.gamma;
    return pipeline;
  }
  pipeline.bpp = profileInfo(profile).defaultBpp;
  return pipeline;
}

QJsonObject Conversion::toJson() const {
  QJsonObject result{{"kind", kind},
                     {"class", avoidable ? "avoidable" : "inherent"},
                     {"accepted", accepted}};
  if (!fxId.isEmpty()) result.insert("fx_id", fxId);
  if (!memberKey.isEmpty()) result.insert("member_key", memberKey);
  if (!setting.isEmpty()) result.insert("setting", setting);
  if (count > 0) result.insert("count", count);
  return result;
}

QJsonArray outputOmissions(const SceneOutputFacts& scene, int requestAspectMode,
                           double requestAspectRatio) {
  QJsonArray result;
  if (scene.stretchFrom != scene.stretchTo)
    result.append(omission(
        "time_stretch",
        QJsonObject{{"from", scene.stretchFrom}, {"to", scene.stretchTo}}));
  if (scene.stereoscopic) result.append(omission("stereoscopic", true));
  if (scene.field != TRenderSettings::NoField)
    result.append(omission("field_prevalence", fieldText(scene.field)));
  if (scene.shrink != 1) result.append(omission("shrink", scene.shrink));
  if (scene.multimedia != 0)
    result.append(
        omission("multimedia", scene.multimedia == 1 ? "columns" : "layers"));
  if (scene.clapperboard) result.append(omission("clapperboard", true));
  if (scene.renderGamma != 1.0)
    result.append(omission("render_gamma", scene.renderGamma));
  const bool sameAspect =
      scene.aspectMode == requestAspectMode &&
      (scene.aspectMode == TOutputProperties::NoAspectExport ||
       scene.aspectRatio == requestAspectRatio);
  if (scene.aspectMode != TOutputProperties::NoAspectExport && !sameAspect)
    result.append(omission("aspect_export",
                           QJsonObject{{"mode", aspectText(scene.aspectMode)},
                                       {"ratio", scene.aspectRatio}}));
  if (scene.sound) result.append(omission("audio", true));
  return result;
}

QStringList avoidableConversionKinds() {
  return {"quantize_to_8bit", "clip_hdr", "fx_integer_intermediate",
          "linearize_quantized", "output_setting_ignored"};
}

std::vector<Conversion> renderConversions(
    RenderProfile profile, const RenderPipeline& pipeline,
    const std::vector<EffectPrecision>& effects, const QJsonArray& omissions,
    const QStringList& accepted) {
  std::vector<Conversion> result;
  addInherent(result, profile);
  const bool exr = profileInfo(profile).linearOutput;
  if (!exr && pipeline.bpp > 32)
    result.push_back(avoidable("quantize_to_8bit", accepted));
  if (!exr && pipeline.bpp == 128)
    result.push_back(avoidable("clip_hdr", accepted));
  if (exr && pipeline.bpp < 128)
    result.push_back(avoidable("linearize_quantized", accepted));
  if (pipeline.bpp == 128) {
    int listed = 0, more = 0;
    for (const EffectPrecision& effect : effects) {
      if (effect.float32) continue;
      if (listed == EffectRowLimit) {
        ++more;
        continue;
      }
      Conversion conversion = avoidable("fx_integer_intermediate", accepted);
      conversion.fxId       = effect.fxId;
      conversion.memberKey  = effect.memberKey;
      result.push_back(conversion);
      ++listed;
    }
    // Effects past the limit share one row with their count, so job records
    // stay within the control receipt bound.
    if (more > 0) {
      Conversion conversion = avoidable("fx_integer_intermediate", accepted);
      conversion.count      = more;
      result.push_back(conversion);
    }
  }
  if (pipeline.source == "scene")
    for (const QJsonValue& row : omissions) {
      const QString setting = row.toObject().value("setting").toString();
      if (setting == "audio") continue;
      Conversion conversion = avoidable("output_setting_ignored", accepted);
      conversion.setting    = setting;
      result.push_back(conversion);
    }
  return result;
}

QJsonObject colorReport(RenderProfile profile, const RenderPipeline& pipeline,
                        const SceneOutputFacts& scene) {
  const ProfileInfo& info = profileInfo(profile);
  const int channelBits   = pipeline.bpp / 4;
  return {
      {"pipeline", QJsonObject{{"source", pipeline.source},
                               {"bpp", pipeline.bpp},
                               {"channel_bits", channelBits},
                               {"float", pipeline.bpp == 128},
                               {"linear", pipeline.linear},
                               {"gamma", pipeline.gamma}}},
      {"output",
       QJsonObject{{"sample", info.sample},
                   {"channels", info.movie ? "rgb" : "rgba"},
                   {"alpha", info.alpha},
                   {"transfer", info.linearOutput ? "linear" : "power"},
                   {"gamma", pipeline.gamma},
                   {"referred", info.linearOutput ? "linear_light" : "display"},
                   {"primaries", "unspecified"},
                   {"tagged", false},
                   {"range", info.movie ? "limited" : "full"}}},
      {"scene_settings", QJsonObject{{"bpp", scene.bpp},
                                     {"linear", scene.linear},
                                     {"gamma", scene.gamma}}}};
}

ExrFacts readExrFacts(const QByteArray& data) {
  ExrHeaderReader reader(data);
  if (reader.int32() != ExrMagic) ExrHeaderReader::fail("Not an EXR file.");
  if (reader.int32() != 2)
    ExrHeaderReader::fail("The EXR file is not a single-part scanline file.");
  ExrFacts facts;
  bool window = false, lineOrder = false;
  for (QByteArray name = reader.text(); !name.isEmpty(); name = reader.text()) {
    if (reader.position > ExrHeaderLimit)
      ExrHeaderReader::fail("The EXR header exceeds its bound.");
    reader.text();
    const QByteArray value = reader.take(reader.int32());
    if (name == "channels") readChannels(value, facts);
    if (name == "dataWindow") {
      readWindow(value, facts);
      window = true;
    }
    if (name == "compression" && value.size() == 1)
      facts.compression = quint8(value[0]);
    if (name == "lineOrder") lineOrder = value.size() == 1 && value[0] == 0;
  }
  if (facts.channels.isEmpty() || !window || !lineOrder ||
      (facts.compression != ExrNone && facts.compression != ExrZip))
    ExrHeaderReader::fail(
        "The EXR header lacks channels, a data window, increasing line order "
        "or a supported compression.");
  readChunks(reader, facts, data.size());
  return facts;
}

}  // namespace GroundzeroAutomation
