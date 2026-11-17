#include "automation_audio.h"
#include "automation.h"

#include "tsound.h"

#include <QCryptographicHash>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr qint64 MaxDecodedBytes = 128LL * 1024 * 1024;
constexpr int MaxChunks          = 4096;
constexpr double MaxSeconds      = 600;
constexpr float MaxFloatSample   = 2.0f;
constexpr int HashBlock          = 64 * 1024;

[[noreturn]] void corrupt(const QString& message, QJsonObject details = {}) {
  throw Error("invalid_asset", message, details);
}

[[noreturn]] void unsupported(const QString& message,
                              QJsonObject details = {}) {
  throw Error("unsupported_format", message, details);
}

struct Chunk final {
  QByteArray id;
  qint64 body;
  qint64 length;
};

const uchar* at(const QByteArray& bytes, qint64 offset) {
  return reinterpret_cast<const uchar*>(bytes.constData()) + offset;
}

quint32 word32(const QByteArray& bytes, qint64 offset, bool bigEndian) {
  return bigEndian ? qFromBigEndian<quint32>(at(bytes, offset))
                   : qFromLittleEndian<quint32>(at(bytes, offset));
}

quint16 word16(const QByteArray& bytes, qint64 offset, bool bigEndian) {
  return bigEndian ? qFromBigEndian<quint16>(at(bytes, offset))
                   : qFromLittleEndian<quint16>(at(bytes, offset));
}

std::vector<Chunk> readChunks(const QByteArray& bytes, bool bigEndian) {
  std::vector<Chunk> result;
  const qint64 size = bytes.size();
  qint64 offset     = 12;
  while (offset < size) {
    if (size - offset < 8)
      corrupt("The audio file ends inside a chunk header.");
    if (int(result.size()) >= MaxChunks)
      corrupt("The audio file contains more than 4096 chunks.");
    const qint64 length = word32(bytes, offset + 4, bigEndian);
    const qint64 body   = offset + 8;
    if (length > size - body)
      corrupt("An audio chunk extends past the end of the file.",
              {{"chunk", QString::fromLatin1(bytes.mid(offset, 4).toHex())},
               {"offset", double(offset)}});
    result.push_back({bytes.mid(offset, 4), body, length});
    offset = body + length + (length & 1);
  }
  return result;
}

const Chunk& single(const std::vector<Chunk>& chunks, const char* id) {
  const Chunk* found = nullptr;
  for (const Chunk& chunk : chunks) {
    if (chunk.id != id) continue;
    if (found)
      corrupt(
          QString("The audio file contains more than one '%1' chunk.").arg(id));
    found = &chunk;
  }
  if (!found) corrupt(QString("The audio file has no '%1' chunk.").arg(id));
  return *found;
}

void requireLayout(const AutomationAudioInfo& info, qint64 blockAlign) {
  if (info.channels == 0) corrupt("The audio file declares zero channels.");
  if (info.channels > 2)
    unsupported("Only mono and stereo audio are admitted.",
                {{"channels", info.channels}});
  if (info.sampleRate == 0)
    corrupt("The audio file declares a zero sample rate.");
  if (info.sampleRate < 8000 || info.sampleRate > 96000)
    unsupported("Sample rates from 8000 through 96000 Hz are admitted.",
                {{"sample_rate", double(info.sampleRate)}});
  if (blockAlign == 0 || blockAlign != info.channels * info.bitsPerSample / 8)
    corrupt("The audio block alignment does not match its channels and bits.",
            {{"block_align", double(blockAlign)}});
}

int wavSampleType(int tag, int bits) {
  if (tag == 0xfffe)
    unsupported(
        "WAVE_FORMAT_EXTENSIBLE is not admitted; save plain PCM or IEEE "
        "float.");
  if (tag != 1 && tag != 3)
    unsupported("Only PCM and IEEE float WAV encodings are admitted.",
                {{"encoding", tag}});
  if (bits == 0 || bits % 8 != 0)
    corrupt("The WAV sample size is not a whole number of bytes.",
            {{"bits_per_sample", bits}});
  if (tag == 3 && bits == 32) return TSound::FLOAT;
  if (tag == 1 && bits == 8) return TSound::UINT;
  if (tag == 1 && (bits == 16 || bits == 24)) return TSound::INT;
  unsupported("This WAV sample size is not admitted.",
              {{"bits_per_sample", bits}, {"encoding", tag}});
}

int aiffSampleType(int bits) {
  if (bits == 0 || bits % 8 != 0)
    corrupt("The AIFF sample size is not a whole number of bytes.",
            {{"bits_per_sample", bits}});
  if (bits == 8 || bits == 16) return TSound::INT;
  // The native AIFF reader leaves the last 24-bit sample unconverted and reads
  // 32-bit integers as floats; neither preserves the source samples.
  unsupported(
      "Only 8- and 16-bit AIFF samples are admitted; use WAV for "
      "24-bit or float audio.",
      {{"bits_per_sample", bits}});
}

qint64 aiffRate(const QByteArray& bytes, qint64 offset) {
  const uchar* value     = at(bytes, offset);
  const quint64 mantissa = qFromBigEndian<quint64>(value + 2);
  const int exponent     = ((value[0] & 0x7f) << 8 | value[1]) - 16383;
  if (value[0] != 0x40 || exponent < 0 || exponent > 30 || !(mantissa >> 63) ||
      (mantissa << (exponent + 1)) != 0)
    unsupported(
        "The AIFF sample rate must be a whole number of hertz from "
        "8000 through 96000.");
  return qint64(mantissa >> (63 - exponent));
}

class PcmHash final {
public:
  void add(const char* data, qint64 length) {
    while (length > 0) {
      const int part = int(std::min<qint64>(length, HashBlock));
      m_hash.addData(data, part);
      data += part;
      length -= part;
    }
  }
  QByteArray hex() const { return m_hash.result().toHex(); }

private:
  QCryptographicHash m_hash{QCryptographicHash::Sha256};
};

void addSwapped(PcmHash& hash, const char* data, qint64 length, int width) {
  QByteArray block;
  while (length > 0) {
    const qint64 part = std::min<qint64>(length, HashBlock - HashBlock % width);
    block.resize(int(part));
    for (qint64 index = 0; index < part; index += width)
      for (int byte = 0; byte < width; ++byte)
        block[int(index + byte)] = data[index + width - 1 - byte];
    hash.add(block.constData(), part);
    data += part;
    length -= part;
  }
}

void requireFloatSamples(const char* data, qint64 length) {
  for (qint64 offset = 0; offset < length; offset += 4) {
    float value = 0;
    std::memcpy(&value, data + offset, 4);
    value = qFromLittleEndian(value);
    if (!std::isfinite(value) || std::fabs(value) > MaxFloatSample)
      corrupt("Float samples must be finite and within -2.0 through 2.0.",
              {{"sample_index", double(offset / 4)}});
  }
}

AutomationAudioInfo parseWav(const QByteArray& bytes) {
  const auto chunks = readChunks(bytes, false);
  const Chunk& fmt  = single(chunks, "fmt ");
  const Chunk& data = single(chunks, "data");
  if (fmt.length < 16) corrupt("The WAV fmt chunk is shorter than 16 bytes.");
  AutomationAudioInfo info;
  info.container     = "wav";
  info.channels      = word16(bytes, fmt.body + 2, false);
  info.sampleRate    = word32(bytes, fmt.body + 4, false);
  info.bitsPerSample = word16(bytes, fmt.body + 14, false);
  info.sampleType =
      wavSampleType(word16(bytes, fmt.body, false), info.bitsPerSample);
  const qint64 align = word16(bytes, fmt.body + 12, false);
  requireLayout(info, align);
  if (data.length == 0 || data.length % align != 0)
    corrupt("WAV sample data must contain a nonzero whole number of frames.",
            {{"data_bytes", double(data.length)}});
  info.pcmBytes    = data.length;
  info.sampleCount = data.length / align;
  const char* pcm  = bytes.constData() + data.body;
  if (info.sampleType == TSound::FLOAT) requireFloatSamples(pcm, data.length);
  PcmHash hash;
  hash.add(pcm, data.length);
  info.pcmSha256 = hash.hex();
  return info;
}

AutomationAudioInfo parseAiff(const QByteArray& bytes) {
  const auto chunks = readChunks(bytes, true);
  const Chunk& comm = single(chunks, "COMM");
  const Chunk& ssnd = single(chunks, "SSND");
  if (comm.length != 18)
    corrupt("The AIFF COMM chunk must contain exactly 18 bytes.");
  AutomationAudioInfo info;
  info.container     = "aiff";
  info.channels      = word16(bytes, comm.body, true);
  info.bitsPerSample = word16(bytes, comm.body + 6, true);
  info.sampleType    = aiffSampleType(info.bitsPerSample);
  info.sampleRate    = aiffRate(bytes, comm.body + 8);
  const qint64 align = qint64(info.channels) * info.bitsPerSample / 8;
  requireLayout(info, align);
  info.sampleCount = word32(bytes, comm.body + 2, true);
  info.pcmBytes    = info.sampleCount * align;
  if (info.sampleCount == 0) corrupt("The AIFF file declares zero frames.");
  if (ssnd.length < 8) corrupt("The AIFF SSND chunk is shorter than 8 bytes.");
  const qint64 offset = word32(bytes, ssnd.body, true);
  if (offset > ssnd.length - 8 || ssnd.length - 8 - offset != info.pcmBytes)
    corrupt("AIFF sample data must hold exactly the declared frame count.",
            {{"frames", double(info.sampleCount)}});
  PcmHash hash;
  const char* pcm = bytes.constData() + ssnd.body + 8 + offset;
  if (info.bitsPerSample == 8)
    hash.add(pcm, info.pcmBytes);
  else
    addSwapped(hash, pcm, info.pcmBytes, info.bitsPerSample / 8);
  info.pcmSha256 = hash.hex();
  return info;
}

void requireSourceLimits(const AutomationAudioInfo& info) {
  if (info.pcmBytes > MaxDecodedBytes)
    throw Error("resource_limit", "Decoded audio exceeds 128 MiB.",
                {{"decoded_bytes", double(info.pcmBytes)}});
  if (double(info.sampleCount) / info.sampleRate > MaxSeconds)
    throw Error("resource_limit", "Audio sources are limited to 600 seconds.",
                {{"sample_count", double(info.sampleCount)}});
}

}  // namespace

AutomationAudioInfo parseAutomationAudio(const QByteArray& bytes) {
  if (bytes.size() > AutomationAudioFileBytes)
    throw Error("resource_limit", "Audio source files are limited to 160 MiB.");
  if (bytes.size() < 12) corrupt("The audio file is too short.");
  const QByteArray form = bytes.left(4);
  const QByteArray type = bytes.mid(8, 4);
  AutomationAudioInfo info;
  // The native readers ignore the RIFF/FORM size (the native WAV writer stores
  // it four bytes short) and read chunks up to the end of the file, as here.
  if (form == "RIFF" && type == "WAVE") {
    info = parseWav(bytes);
  } else if (form == "FORM" && type == "AIFF") {
    info = parseAiff(bytes);
  } else if (form == "FORM" && type == "AIFC") {
    unsupported("AIFC files are not admitted; use AIFF or WAV.");
  } else {
    unsupported("Only RIFF/WAVE and FORM/AIFF audio files are admitted.");
  }
  requireSourceLimits(info);
  return info;
}

QByteArray automationTrackSha256(const TSoundTrackP& track) {
  PcmHash hash;
  hash.add(reinterpret_cast<const char*>(track->getRawData()),
           qint64(track->getSampleCount()) * track->getSampleSize());
  return hash.hex();
}

}  // namespace GroundzeroAutomation
