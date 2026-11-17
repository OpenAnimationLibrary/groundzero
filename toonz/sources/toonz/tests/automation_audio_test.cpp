#include "automation.h"
#include "automation_audio.h"
#include "automation_dispatch.h"

#include "tsound.h"
#include "toonz/levelset.h"
#include "toonz/toonzscene.h"
#include "toonz/txshsoundcolumn.h"
#include "toonz/txshsoundlevel.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace GroundzeroAutomation {
namespace {

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

QByteArray le(quint32 value, int bytes) {
  QByteArray result;
  for (int index = 0; index < bytes; ++index)
    result.append(char((value >> (8 * index)) & 0xff));
  return result;
}

QByteArray be(quint32 value, int bytes) {
  QByteArray result;
  for (int index = bytes - 1; index >= 0; --index)
    result.append(char((value >> (8 * index)) & 0xff));
  return result;
}

struct WavSpec final {
  int tag      = 1;
  int channels = 1;
  int rate     = 8000;
  int bits     = 16;
  int align    = -1;
};

QByteArray wavFile(const WavSpec& spec, const QByteArray& pcm,
                   const QByteArray& extra = {}) {
  const int align =
      spec.align >= 0 ? spec.align : spec.channels * spec.bits / 8;
  QByteArray body = "WAVE";
  body += "fmt " + le(16, 4) + le(spec.tag, 2) + le(spec.channels, 2) +
          le(spec.rate, 4) + le(spec.rate * align, 4) + le(align, 2) +
          le(spec.bits, 2);
  body += extra;
  body += "data" + le(pcm.size(), 4) + pcm;
  if (pcm.size() % 2) body += '\0';
  return "RIFF" + le(body.size(), 4) + body;
}

QByteArray aiffRate(quint32 rate) {
  int exponent = 31;
  while (!(rate & (1u << exponent))) --exponent;
  const quint64 mantissa = quint64(rate) << (63 - exponent);
  QByteArray result      = be(16383 + exponent, 2);
  result += be(quint32(mantissa >> 32), 4) + be(quint32(mantissa), 4);
  return result;
}

QByteArray aiffFile(const QByteArray& form, int channels, int bits,
                    quint32 frames, const QByteArray& pcm, quint32 offset = 0) {
  QByteArray body = form;
  body += "COMM" + be(18, 4) + be(channels, 2) + be(frames, 4) + be(bits, 2) +
          aiffRate(8000);
  body += "SSND" + be(8 + offset + pcm.size(), 4) + be(offset, 4) + be(0, 4) +
          QByteArray(int(offset), '\0') + pcm;
  if (body.size() % 2) body += '\0';
  return "FORM" + be(body.size(), 4) + body;
}

QByteArray samples16(const std::vector<int>& values) {
  QByteArray result;
  for (int value : values) result += le(quint32(value) & 0xffff, 2);
  return result;
}

QByteArray dc16(int value, int count) {
  return samples16(std::vector<int>(count, value));
}

QString parseCode(const QByteArray& bytes) {
  try {
    parseAutomationAudio(bytes);
  } catch (const Error& error) {
    return error.code;
  }
  return "accepted";
}

void expectParse(const QByteArray& bytes, const QString& code,
                 const char* message) {
  require(parseCode(bytes) == code, message);
}

void acceptedSources() {
  const QByteArray pcm = samples16({1, -2, 300, -32768});
  const auto mono      = parseAutomationAudio(wavFile({}, pcm));
  require(
      mono.sampleCount == 4 && mono.sampleType == TSound::INT &&
          mono.pcmSha256 ==
              QCryptographicHash::hash(pcm, QCryptographicHash::Sha256).toHex(),
      "WAV PCM digest differs from its data chunk");
  QByteArray nativeWritten = wavFile({}, pcm);
  nativeWritten.replace(4, 4, le(nativeWritten.size() - 12, 4));
  expectParse(nativeWritten, "accepted",
              "A WAV with the native writer's RIFF size was refused");
  require(parseAutomationAudio(wavFile({1, 1, 8000, 8}, "\x80\x81\x82"))
                  .sampleType == TSound::UINT,
          "8-bit WAV is not unsigned");
  require(parseAutomationAudio(wavFile({1, 2, 96000, 24}, QByteArray(12, 1)))
                  .sampleCount == 2,
          "24-bit stereo WAV frame count is wrong");
  float half = 0.5f;
  QByteArray floats(reinterpret_cast<const char*>(&half), 4);
  require(parseAutomationAudio(wavFile({3, 1, 44100, 32}, floats + floats))
                  .sampleType == TSound::FLOAT,
          "Float WAV is not admitted");
  const auto aiff = parseAutomationAudio(
      aiffFile("AIFF", 1, 16, 2, be(0x0102, 2) + be(0x0304, 2)));
  require(aiff.sampleRate == 8000 &&
              aiff.pcmSha256 ==
                  QCryptographicHash::hash(QByteArray("\x02\x01\x04\x03", 4),
                                           QCryptographicHash::Sha256)
                      .toHex(),
          "AIFF samples are not hashed in native little-endian order");
  require(
      parseAutomationAudio(aiffFile("AIFF", 2, 8, 1, "\x01\xff")).sampleType ==
          TSound::INT,
      "8-bit AIFF is not signed");
}

void unsupportedSources() {
  const QByteArray pcm = dc16(5, 4);
  expectParse(wavFile({0xfffe, 1, 8000, 16}, pcm), "unsupported_format",
              "Extensible WAV was admitted");
  expectParse(wavFile({1, 1, 8000, 32}, pcm), "unsupported_format",
              "32-bit integer WAV was admitted");
  expectParse(wavFile({2, 1, 8000, 16}, pcm), "unsupported_format",
              "Compressed WAV was admitted");
  for (int channels = 3; channels <= 9; ++channels)
    expectParse(wavFile({1, channels, 8000, 8}, QByteArray(channels, 1)),
                "unsupported_format", "More than two channels were admitted");
  expectParse(wavFile({1, 1, 4000, 16}, pcm), "unsupported_format",
              "A 4000 Hz source was admitted");
  expectParse(aiffFile("AIFC", 1, 16, 2, pcm), "unsupported_format",
              "AIFC was admitted");
  expectParse(aiffFile("AIFF", 1, 24, 1, QByteArray(3, 1)),
              "unsupported_format", "24-bit AIFF was admitted");
  expectParse(aiffFile("AIFF", 1, 32, 1, QByteArray(4, 1)),
              "unsupported_format", "32-bit AIFF was admitted");
  expectParse("OggS" + QByteArray(64, 0), "unsupported_format",
              "A non-WAV container was admitted");
}

void corruptSources() {
  const QByteArray pcm = dc16(5, 4);
  expectParse(wavFile({1, 1, 8000, 16, 0}, pcm), "invalid_asset",
              "Zero block alignment reached admission");
  expectParse(wavFile({1, 1, 8000, 16, 4}, pcm), "invalid_asset",
              "Mismatched block alignment was admitted");
  expectParse(wavFile({1, 0, 8000, 16}, pcm), "invalid_asset",
              "Zero channels were admitted");
  expectParse(wavFile({1, 1, 0, 16}, pcm), "invalid_asset",
              "A zero sample rate was admitted");
  expectParse(wavFile({}, {}), "invalid_asset", "An empty WAV was admitted");
  expectParse(wavFile({}, pcm + "x"), "invalid_asset",
              "A partial sample frame was admitted");
  QByteArray oversized = wavFile({}, pcm);
  oversized.replace(oversized.size() - pcm.size() - 4, 4, le(0x7ffffff0, 4));
  expectParse(oversized, "invalid_asset",
              "An oversized data chunk reached admission");
  QByteArray truncated = wavFile({}, pcm);
  truncated.chop(2);
  expectParse(truncated, "invalid_asset", "A truncated WAV was admitted");
  expectParse(wavFile({}, pcm, "data" + le(2, 4) + "ab"), "invalid_asset",
              "A second data chunk was admitted");
  expectParse(wavFile({}, pcm, "fmt " + le(2, 4) + "ab"), "invalid_asset",
              "A second fmt chunk was admitted");
  float value = std::nanf("");
  expectParse(wavFile({3, 1, 8000, 32},
                      QByteArray(reinterpret_cast<const char*>(&value), 4)),
              "invalid_asset", "A NaN float sample was admitted");
  value = 3.0f;
  expectParse(wavFile({3, 1, 8000, 32},
                      QByteArray(reinterpret_cast<const char*>(&value), 4)),
              "invalid_asset", "An unbounded float sample was admitted");
  expectParse(aiffFile("AIFF", 1, 16, 3, pcm), "invalid_asset",
              "An AIFF frame count past its data was admitted");
  expectParse(aiffFile("AIFF", 1, 16, 2, pcm, 2), "invalid_asset",
              "An AIFF SSND offset over-read was admitted");
  expectParse("RIFF", "invalid_asset", "A truncated header was admitted");
}

void sourceLimits() {
  const QByteArray tooLong =
      wavFile({1, 1, 8000, 8}, QByteArray(600 * 8000 + 2, '\x80'));
  expectParse(tooLong, "resource_limit", "A source over 600 s was admitted");
  expectParse(wavFile({1, 1, 8000, 8}, QByteArray(600 * 8000, '\x80')),
              "accepted", "A 600 s source was refused");
}

// ---------------------------------------------------------------------------

QJsonObject request(Dispatcher& dispatcher, const QString& method,
                    QJsonObject params) {
  static int sequence = 0;
  if (method != "audio.inspect" && method != "scene.summary" &&
      method != "column.cells")
    params.insert("operation_id", "audio-test-" + QString::number(++sequence));
  return dispatcher.dispatch(
      {{"id", "audio"}, {"method", method}, {"params", params}});
}

QJsonObject result(Dispatcher& dispatcher, const QString& method,
                   const QJsonObject& params) {
  const auto response = request(dispatcher, method, params);
  if (!response.contains("result"))
    throw std::runtime_error(
        QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString());
  return response.value("result").toObject();
}

QJsonObject addressed(Document& document, QJsonObject params = {}) {
  params.insert("document_id", document.id);
  params.insert("expected_revision", document.revisionText());
  return params;
}

QString rejected(Dispatcher& dispatcher, Document& document,
                 const QString& method, const QJsonObject& params) {
  const auto revision = document.revision;
  const auto response = request(dispatcher, method, params);
  require(document.revision == revision,
          "An audio rejection changed the document revision");
  return response.value("error").toObject().value("code").toString();
}

Document& createDocument(Context& context, Dispatcher& dispatcher, double fps) {
  const auto created = result(dispatcher, "scene.create",
                              {{"name", "Audio tests"},
                               {"width_px", 32},
                               {"height_px", 32},
                               {"frame_rate", fps}});
  return context.document(created.value("document_id").toString());
}

QString writeInput(Context& context, const QString& name,
                   const QByteArray& bytes) {
  const QString path = QDir(context.workspace()).filePath(name);
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
          "Cannot write an audio fixture");
  return path;
}

QString importSource(Context& context, Dispatcher& dispatcher,
                     Document& document, const QString& name,
                     const QByteArray& bytes) {
  const QString path = writeInput(context, name + ".wav", bytes);
  return result(dispatcher, "audio.import",
                addressed(document, {{"path", path}, {"name", name}}))
      .value("source")
      .toObject()
      .value("level_id")
      .toString();
}

QString createColumn(Dispatcher& dispatcher, Document& document,
                     int volume = 1000) {
  return result(dispatcher, "audio.column.create",
                addressed(document,
                          {{"name", "Sound"}, {"volume_permille", volume}}))
      .value("column")
      .toObject()
      .value("column_id")
      .toString();
}

QJsonObject placement(int timeline, int source, int count) {
  return {{"timeline_start_frame", timeline},
          {"source_start_frame", source},
          {"frame_count", count}};
}

QJsonObject placeParams(Document& document, const QString& column,
                        const QString& level, int timeline, int source,
                        int count) {
  return addressed(document, {{"column_id", column},
                              {"level_id", level},
                              {"placement", placement(timeline, source, count)},
                              {"overlap_policy", "reject"}});
}

void place(Dispatcher& dispatcher, Document& document, const QString& column,
           const QString& level, int timeline, int source, int count) {
  result(dispatcher, "audio.clip.place",
         placeParams(document, column, level, timeline, source, count));
}

QJsonObject clipKey(const QString& column, int timeline) {
  return {{"column_id", column}, {"timeline_start_frame", timeline}};
}

void importAndRateAdmission(Context& context) {
  Dispatcher dispatcher(context);
  auto& document       = createDocument(context, dispatcher, 24);
  const QByteArray wav = wavFile({}, dc16(1000, 8000));
  const QString level =
      importSource(context, dispatcher, document, "Line", wav);
  const auto inspected =
      result(dispatcher, "audio.inspect", addressed(document));
  const auto source = inspected.value("sources").toArray().at(0).toObject();
  require(source.value("level_id").toString() == level &&
              source.value("native_frame_count").toInt() == 24 &&
              source.value("source_sha256").toString() ==
                  QCryptographicHash::hash(wav, QCryptographicHash::Sha256)
                      .toHex() &&
              source.value("decoded_by").toString() == "native_wav",
          "Imported source descriptor differs from its file");
  const QString path = writeInput(context, "Line.wav", wav);
  require(rejected(dispatcher, document, "audio.import",
                   addressed(document, {{"path", path}, {"name", "Line"}})) ==
              "name_conflict",
          "A duplicate source name was admitted");
  const QString bad =
      writeInput(context, "bad.wav", wavFile({1, 1, 8000, 16, 0}, dc16(1, 4)));
  require(rejected(dispatcher, document, "audio.import",
                   addressed(document, {{"path", bad}, {"name", "Bad"}})) ==
              "invalid_asset",
          "A corrupt import reached the cast");
  require(document.scene->getLevelSet()->getLevelCount() == 1,
          "A rejected import changed the cast");
  result(dispatcher, "scene.settings",
         addressed(document, {{"frame_rate", 24000.0 / 1001}}));
  require(rejected(dispatcher, document, "audio.import",
                   addressed(document, {{"path", path}, {"name", "Other"}})) ==
              "timing_conflict",
          "An unstable frame rate admitted an import");
  require(document.scene->getLevelSet()
                  ->getLevel(0)
                  ->getSoundLevel()
                  ->getFrameRate() == 24000.0 / 1001,
          "A rate change left a source at its old rate");
  context.removeDocument(document.id);
}

void clipPreflight(Context& context) {
  Dispatcher dispatcher(context);
  auto& document       = createDocument(context, dispatcher, 24);
  const QString level  = importSource(context, dispatcher, document, "Clip",
                                      wavFile({}, dc16(7, 8000)));
  const QString column = createColumn(dispatcher, document);
  place(dispatcher, document, column, level, 10, 2, 4);
  const auto overlap = request(dispatcher, "audio.clip.place",
                               placeParams(document, column, level, 12, 0, 2));
  require(overlap.value("error").toObject().value("code") == "audio_overlap" &&
              overlap.value("error")
                      .toObject()
                      .value("details")
                      .toObject()
                      .value("conflicts")
                      .toArray()
                      .at(0)
                      .toObject() == clipKey(column, 10),
          "Overlap did not name the conflicting clip");
  require(rejected(dispatcher, document, "audio.clip.place",
                   placeParams(document, column, level, 14, 6, 2)) ==
              "invalid_params",
          "A contiguous same-source clip was silently joined");
  require(rejected(dispatcher, document, "audio.clip.place",
                   placeParams(document, column, level, 30, 20, 5)) ==
              "invalid_params",
          "A placement past the native frame count was admitted");
  auto* sound = document.column(column)->getSoundColumn();
  require(sound->getClipCount() == 1 &&
              sound->getClip(0)->getStartFrame() == 8 &&
              sound->getClip(0)->getEndOffset() == 18,
          "Native clip fields differ from the placement");
  sound->lock(true);
  require(rejected(dispatcher, document, "audio.clip.remove",
                   addressed(document, {{"clip", clipKey(column, 10)}})) ==
              "locked",
          "A locked sound column was edited");
  sound->lock(false);
  context.removeDocument(document.id);
}

void clipUpdateAndRemove(Context& context) {
  Dispatcher dispatcher(context);
  auto& document       = createDocument(context, dispatcher, 24);
  const QString level  = importSource(context, dispatcher, document, "Edit",
                                      wavFile({}, dc16(7, 8000)));
  const QString column = createColumn(dispatcher, document);
  place(dispatcher, document, column, level, 0, 2, 3);
  const auto revision = document.revision;
  const auto same =
      result(dispatcher, "audio.clip.update",
             addressed(document, {{"clip", clipKey(column, 0)},
                                  {"placement", placement(0, 2, 3)},
                                  {"overlap_policy", "reject"}}));
  require(!same.value("changed").toBool() && document.revision == revision,
          "An identical update changed the revision");
  result(dispatcher, "audio.clip.update",
         addressed(document, {{"clip", clipKey(column, 0)},
                              {"placement", placement(5, 0, 6)},
                              {"overlap_policy", "reject"}}));
  auto* sound = document.column(column)->getSoundColumn();
  require(sound->getClipCount() == 1 &&
              sound->getClip(0)->getVisibleStartFrame() == 5 &&
              sound->getClip(0)->getVisibleFrameCount() == 6,
          "The updated clip differs from its placement");
  require(rejected(dispatcher, document, "audio.clip.remove",
                   addressed(document, {{"clip", clipKey(column, 0)}})) ==
              "not_found",
          "A stale clip key was accepted");
  result(dispatcher, "audio.clip.remove",
         addressed(document, {{"clip", clipKey(column, 5)}}));
  require(sound->getClipCount() == 0 &&
              document.scene->getLevelSet()->getLevelCount() == 1,
          "Clip removal changed the cast or left the clip");
  context.removeDocument(document.id);
}

// ---------------------------------------------------------------------------
// An independent model of the native offline mix for 16-bit mono sources.

struct ModelClip final {
  const std::vector<int>* source;
  int timeline, start, count;
};

std::vector<int> modelColumn(const std::vector<ModelClip>& clips, int from,
                             int to, double perFrame, int samples) {
  std::vector<int> track(samples, 0);
  for (const ModelClip& clip : clips) {
    const int begin = clip.timeline, end = clip.timeline + clip.count;
    if (begin < from && end < from) continue;
    if (begin > to && end > to) break;
    const int s0 = int((clip.start + std::max(0, from - begin)) * perFrame);
    const int s1 =
        int((clip.start + clip.count - std::max(0, end - to)) * perFrame);
    if (s1 <= 0 || s1 < s0) continue;
    const int last  = int(clip.source->size()) - 1;
    const int first = std::clamp(s0, 0, last);
    int target = std::clamp(int((begin - from) * perFrame), 0, samples - 1);
    for (int index = first;
         index <= std::clamp(s1, 0, last) && target < samples; ++index)
      track[target++] = (*clip.source)[index];
  }
  return track;
}

std::vector<int> modelMix(
    const std::vector<std::pair<std::vector<ModelClip>, double>>& columns,
    int from, int to, double fps, int rate) {
  const double perFrame = rate / fps;
  const double exact    = double(to - from) / fps * rate;
  int samples           = int(exact);
  if (samples < exact - 1e-8) ++samples;
  std::vector<int> mix(samples, 0);
  for (std::size_t index = 0; index < columns.size(); ++index) {
    if (index > 0 && columns[index].second == 0) continue;
    const auto track =
        modelColumn(columns[index].first, from, to, perFrame, samples);
    for (int sample = 0; sample < samples; ++sample)
      mix[sample] = std::clamp(
          int(mix[sample] * 1.0 + track[sample] * columns[index].second),
          -32768, 32767);
  }
  return mix;
}

std::vector<int> ramp(int count, int scale) {
  std::vector<int> values(count);
  for (int index = 0; index < count; ++index)
    values[index] = (index * scale) % 30000 - 15000;
  return values;
}

std::vector<int> decodedMix(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Cannot read the mixdown file");
  const QByteArray bytes = file.readAll();
  const auto info        = parseAutomationAudio(bytes);
  require(info.bitsPerSample == 16 && info.channels == 1,
          "The mixdown format differs from its first source");
  std::vector<int> values;
  for (qint64 offset = 44; offset < bytes.size(); offset += 2)
    values.push_back(qFromLittleEndian<qint16>(bytes.constData() + offset));
  return values;
}

void mixdownMatchesModel(Context& context) {
  Dispatcher dispatcher(context);
  auto& document = createDocument(context, dispatcher, 23.976);
  const auto one = ramp(8000, 7), two = ramp(9000, 13);
  const QString a      = importSource(context, dispatcher, document, "A",
                                      wavFile({}, samples16(one)));
  const QString b      = importSource(context, dispatcher, document, "B",
                                      wavFile({}, samples16(two)));
  const QString first  = createColumn(dispatcher, document, 1000);
  const QString second = createColumn(dispatcher, document, 750);
  const QString silent = createColumn(dispatcher, document, 0);
  place(dispatcher, document, first, a, 0, 1, 5);
  place(dispatcher, document, first, b, 7, 3, 4);
  place(dispatcher, document, second, b, 2, 0, 9);
  place(dispatcher, document, silent, a, 0, 0, 9);
  const QString output = QDir(context.workspace()).filePath("model-mix.wav");
  const auto mixed =
      result(dispatcher, "audio.mixdown",
             addressed(document, {{"start_frame", 1},
                                  {"end_frame", 9},
                                  {"output_path", output},
                                  {"format_policy", "native_mix"}}));
  const auto expected = modelMix({{{{&one, 0, 1, 5}, {&two, 7, 3, 4}}, 1.0},
                                  {{{&two, 2, 0, 9}}, 0.75},
                                  {{{&one, 0, 0, 9}}, 0.0}},
                                 1, 10, 23.976, 8000);
  require(decodedMix(output) == expected,
          "The offline mixdown differs from the independent model");
  require(mixed.value("excluded_columns").toArray().size() == 1 &&
              mixed.value("included_columns").toArray().size() == 2,
          "A later zero-volume column was not excluded");
  context.removeDocument(document.id);
}

void offlineMatchesDeviceMix(Context& context) {
  Dispatcher dispatcher(context);
  auto& document  = createDocument(context, dispatcher, 24);
  const QString a = importSource(context, dispatcher, document, "Device",
                                 wavFile({1, 1, 44100, 16}, dc16(900, 44100)));
  const QString column = createColumn(dispatcher, document);
  place(dispatcher, document, column, a, 3, 0, 10);
  auto* sound = document.column(column)->getSoundColumn();
  const std::vector<TXshSoundColumn*> columns{sound};
  const TSoundTrackP offline = sound->mixingTogetherOffline(columns, 0, 12, 24);
  require(offline && offline->getSampleRate() == 44100,
          "The offline mix needs or adapts to an audio device");
  try {
    const TSoundTrackP device = sound->mixingTogether(columns, 0, 12, 24);
    if (device->getFormat() == offline->getFormat())
      require(automationTrackSha256(device) == automationTrackSha256(offline),
              "The offline mix differs from the device mix in its format");
  } catch (const TSoundDeviceException&) {
  }
  context.removeDocument(document.id);
}

QJsonObject withoutIds(QJsonObject audio) {
  for (const QString& key : {"document_id", "revision"}) audio.remove(key);
  QJsonArray sources;
  for (const auto& value : audio.value("sources").toArray()) {
    auto source = value.toObject();
    source.remove("level_id");
    sources.append(source);
  }
  audio.insert("sources", sources);
  QJsonArray columns;
  for (const auto& value : audio.value("columns").toArray()) {
    auto column = value.toObject();
    for (const QString& key : {"column_id", "object_id"}) column.remove(key);
    QJsonArray clips;
    for (const auto& clip : column.value("clips").toArray()) {
      auto stripped = clip.toObject();
      for (const QString& key : {"column_id", "level_id"}) stripped.remove(key);
      clips.append(stripped);
    }
    column.insert("clips", clips);
    columns.append(column);
  }
  audio.insert("columns", columns);
  audio.insert("mix", audio.value("mix").toObject().value("format"));
  return audio;
}

void packageRoundTrip(Context& context) {
  Dispatcher dispatcher(context);
  auto& document  = createDocument(context, dispatcher, 29.97);
  const QString a = importSource(context, dispatcher, document, "Round",
                                 wavFile({1, 2, 22050, 16}, dc16(-77, 22050)));
  const QString column = createColumn(dispatcher, document, 333);
  place(dispatcher, document, column, a, 0, 3, 8);
  const QString empty = createColumn(dispatcher, document);
  result(dispatcher, "column.update",
         addressed(document, {{"column_id", empty}, {"locked", true}}));
  const QString directory = QDir(context.workspace()).filePath("audio-package");
  require(rejected(dispatcher, document, "scene.save_copy",
                   addressed(document, {{"directory", directory}})) ==
              "unsupported_column",
          "An empty locked sound column was saved without its lock");
  result(dispatcher, "column.update",
         addressed(document, {{"column_id", empty}, {"locked", false}}));
  const auto before = result(dispatcher, "audio.inspect", addressed(document));
  result(dispatcher, "scene.save_copy",
         addressed(document, {{"directory", directory}}));
  const auto opened  = result(dispatcher, "scene.open", {{"path", directory}});
  auto& reopened     = context.document(opened.value("document_id").toString());
  auto after         = result(dispatcher, "audio.inspect", addressed(reopened));
  auto expected      = withoutIds(before);
  QJsonArray columns = expected.value("columns").toArray();
  columns.removeLast();
  expected.insert("columns", columns);
  require(withoutIds(after) == expected,
          "A reopened package differs in sound sources or clips");
  context.removeDocument(reopened.id);
  context.removeDocument(document.id);
}

void soundColumnsNeverAllocateCells(Context& context) {
  const int before = TXshSoundColumn::allocatedCellCount();
  Dispatcher dispatcher(context);
  auto& document       = createDocument(context, dispatcher, 24);
  const QString a      = importSource(context, dispatcher, document, "Cells",
                                      wavFile({}, dc16(3, 8000)));
  const QString column = createColumn(dispatcher, document);
  place(dispatcher, document, column, a, 0, 0, 5);
  result(dispatcher, "scene.summary", {{"document_id", document.id}});
  require(rejected(dispatcher, document, "column.cells",
                   {{"document_id", document.id},
                    {"column_id", column},
                    {"start_frame", 0},
                    {"length", 5}}) == "incompatible_column",
          "column.cells read a sound column");
  result(dispatcher, "scene.save_copy",
         addressed(document,
                   {{"directory",
                     QDir(context.workspace()).filePath("cells-package")}}));
  require(TXshSoundColumn::allocatedCellCount() == before,
          "Worker paths called the allocating sound getCell");
  context.removeDocument(document.id);
}

}  // namespace

void runAutomationAudioTests(Context& context) {
  acceptedSources();
  unsupportedSources();
  corruptSources();
  sourceLimits();
  importAndRateAdmission(context);
  clipPreflight(context);
  clipUpdateAndRemove(context);
  mixdownMatchesModel(context);
  offlineMatchesDeviceMix(context);
  packageRoundTrip(context);
  soundColumnsNeverAllocateCells(context);
  std::cout << "automation-audio-test: parser, clips, mixdown and packages "
               "PASS\n";
}

}  // namespace GroundzeroAutomation
