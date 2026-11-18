// SPDX-License-Identifier: BSD-3-Clause

// The AIFF reader and writer on files built byte by byte in this test. The
// sample rate is an 80-bit extended number that the code used to read and
// write through a 32-bit pointer at an odd 2-byte offset, and the sample data
// is read from the SSND chunk at the offset the file gives, which may be odd.

#include "tcommon.h"
#include "tfilepath.h"
#include "tfilepath_io.h"
#include "tsound.h"
#include "tsound_io.h"
#include "tsound_t.h"
#include "tsystem.h"

#include "aiff/tsio_aiff.h"

#include <QCoreApplication>
#include <QDir>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

typedef std::vector<unsigned char> Bytes;

void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}

void put16(Bytes& b, unsigned v) {
  b.push_back(UCHAR(v >> 8));
  b.push_back(UCHAR(v));
}

void put32(Bytes& b, uint32_t v) {
  put16(b, v >> 16);
  put16(b, v & 0xFFFF);
}

void putTag(Bytes& b, const char* tag) { b.insert(b.end(), tag, tag + 4); }

// 80-bit IEEE extended number for a positive integer.
void putExtended(Bytes& b, uint32_t rate) {
  int top = 31;
  while (!((rate >> top) & 1)) --top;
  put16(b, 16383 + top);
  put32(b, rate << (31 - top));
  put32(b, 0);
}

Bytes commChunk(int channels, uint32_t frames, int bits, uint32_t rate) {
  Bytes c;
  putTag(c, "COMM");
  put32(c, 18);
  put16(c, channels);
  put32(c, frames);
  put16(c, bits);
  putExtended(c, rate);
  return c;
}

// The sample bytes are the 'payload', preceded by 'skip' bytes that the offset
// field of the chunk steps over.
Bytes ssndChunk(const Bytes& payload, uint32_t offset, size_t skip) {
  Bytes c;
  putTag(c, "SSND");
  put32(c, UINT32_C(8) + uint32_t(skip + payload.size()));
  put32(c, offset);
  put32(c, 0);
  c.insert(c.end(), skip, UCHAR(0x7E));
  c.insert(c.end(), payload.begin(), payload.end());
  if (c.size() % 2) c.push_back(0);
  return c;
}

Bytes oddChunk(const char* tag, size_t length) {
  Bytes c;
  putTag(c, tag);
  put32(c, uint32_t(length));
  c.insert(c.end(), length, UCHAR(0x55));
  if (length % 2) c.push_back(0);
  return c;
}

Bytes form(const std::vector<Bytes>& chunks) {
  Bytes body;
  putTag(body, "AIFF");
  for (const Bytes& chunk : chunks)
    body.insert(body.end(), chunk.begin(), chunk.end());
  Bytes file;
  putTag(file, "FORM");
  put32(file, uint32_t(body.size()));
  file.insert(file.end(), body.begin(), body.end());
  return file;
}

TFilePath writeFile(const QDir& dir, const char* name, const Bytes& bytes) {
  TFilePath path(dir.filePath(name).toStdWString());
  Tofstream os(path, std::ios::binary);
  os.write(reinterpret_cast<const char*>(bytes.data()),
           std::streamsize(bytes.size()));
  require(bool(os), "the fixture file is written");
  return path;
}

TSoundTrackP readFile(const TFilePath& path) {
  TSoundTrackReaderAiff reader(path);
  return reader.load();
}

// Samples as signed 16-bit values, most significant byte first in the file.
Bytes pcm16(const std::vector<int>& values) {
  Bytes b;
  for (int v : values) put16(b, unsigned(v) & 0xFFFF);
  return b;
}

const int kRates[] = {8000,  11025, 22050, 32000, 44100,
                      48000, 88200, 96000, 192000};

void sampleRatesAreReadFromTheExtendedNumber() {
  QDir dir(QDir::tempPath());
  const std::vector<int> samples = {0,      1,     -1,     32767,
                                    -32768, 12345, -12345, 256};
  for (int rate : kRates) {
    const TFilePath path =
        writeFile(dir, "aiff_reader_rate.aiff",
                  form({commChunk(1, uint32_t(samples.size()), 16, rate),
                        ssndChunk(pcm16(samples), 0, 0)}));
    TSoundTrackP track = readFile(path);
    require(track, "the file is read");
    require(int(track->getSampleRate()) == rate,
            "the sample rate " + std::to_string(rate) + " is read back");
    require(track->getSampleCount() == TINT32(samples.size()),
            "the frame count is read");
    const short* data = reinterpret_cast<const short*>(track->getRawData());
    for (size_t i = 0; i < samples.size(); ++i)
      require(data[i] == short(samples[i]), "a 16-bit sample is byte-swapped");
    TSystem::deleteFile(path);
  }
}

void everyOffsetOfTheSampleDataIsRead() {
  QDir dir(QDir::tempPath());
  const std::vector<int> samples = {100, -200, 300, -32768, 32767, -1};
  for (uint32_t offset : {0u, 1u, 2u, 3u, 5u, 7u}) {
    const TFilePath path =
        writeFile(dir, "aiff_reader_offset.aiff",
                  form({commChunk(1, uint32_t(samples.size()), 16, 44100),
                        ssndChunk(pcm16(samples), offset, offset)}));
    TSoundTrackP track = readFile(path);
    require(track, "the file with a sample offset is read");
    const short* data = reinterpret_cast<const short*>(track->getRawData());
    for (size_t i = 0; i < samples.size(); ++i)
      require(data[i] == short(samples[i]),
              "samples are read from the given offset");
    TSystem::deleteFile(path);
  }
}

void chunksAtOddFileOffsetsAreRead() {
  QDir dir(QDir::tempPath());
  const std::vector<int> samples = {5, -6, 7, -8};
  // An odd-length chunk before COMM puts COMM at an offset that is a multiple
  // of 2 only because of the pad byte; two of them shift the data by 2 more.
  const TFilePath path =
      writeFile(dir, "aiff_reader_chunks.aiff",
                form({oddChunk("NAME", 3), oddChunk("ANNO", 5),
                      commChunk(2, uint32_t(samples.size() / 2), 16, 22050),
                      oddChunk("APPL", 1), ssndChunk(pcm16(samples), 0, 0)}));
  TSoundTrackP track = readFile(path);
  require(track, "a file with other chunks is read");
  require(track->getChannelCount() == 2, "the channel count is read");
  require(track->getSampleCount() == 2, "the frame count is read");
  require(int(track->getSampleRate()) == 22050, "the rate after other chunks");
  const short* data = reinterpret_cast<const short*>(track->getRawData());
  for (size_t i = 0; i < samples.size(); ++i)
    require(data[i] == short(samples[i]), "stereo samples are byte-swapped");
  TSystem::deleteFile(path);
}

void eightAndTwentyFourBitSamplesAreRead() {
  QDir dir(QDir::tempPath());
  Bytes eight        = {0x00, 0x7F, 0x80, 0xFF, 0x01};
  TSoundTrackP track = readFile(
      writeFile(dir, "aiff_reader_8.aiff",
                form({commChunk(1, 5, 8, 11025), ssndChunk(eight, 0, 0)})));
  require(track && track->getBitPerSample() == 8, "an 8-bit file is read");
  const unsigned char* raw = track->getRawData();
  for (size_t i = 0; i < eight.size(); ++i)
    require(raw[i] == eight[i], "8-bit samples are copied");

  Bytes three = {0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD, 0x80, 0x00, 0x01};
  track       = readFile(
      writeFile(dir, "aiff_reader_24.aiff",
                form({commChunk(1, 3, 24, 48000), ssndChunk(three, 0, 0)})));
  require(track && track->getBitPerSample() == 24, "a 24-bit file is read");
  raw = track->getRawData();
  for (size_t i = 0; i < three.size(); i += 3)
    require(raw[i] == three[i + 2] && raw[i + 1] == three[i + 1] &&
                raw[i + 2] == three[i],
            "24-bit samples are byte-swapped");
  // 32-bit samples are floats; with an odd offset they start at an odd byte.
  const float values[] = {1.0f, -0.5f, 0.25f};
  Bytes floats;
  for (float value : values) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put32(floats, bits);
  }
  for (uint32_t offset : {0u, 1u, 3u}) {
    track = readFile(writeFile(
        dir, "aiff_reader_32.aiff",
        form({commChunk(1, 3, 32, 44100), ssndChunk(floats, offset, offset)})));
    require(track && track->getBitPerSample() == 32, "a 32-bit file is read");
    const float* decoded = reinterpret_cast<const float*>(track->getRawData());
    for (int i = 0; i < 3; ++i)
      require(decoded[i] == values[i], "32-bit samples are byte-swapped");
  }
  TSystem::deleteFile(
      TFilePath(dir.filePath("aiff_reader_32.aiff").toStdWString()));
  TSystem::deleteFile(
      TFilePath(dir.filePath("aiff_reader_8.aiff").toStdWString()));
  TSystem::deleteFile(
      TFilePath(dir.filePath("aiff_reader_24.aiff").toStdWString()));
}

void writtenFilesKeepTheirSampleRate() {
  QDir dir(QDir::tempPath());
  for (int rate : kRates) {
    short data[4];
    for (int i = 0; i < 4; ++i) data[i] = short(i * 1000 - 1500);
    TSoundTrackP track = TSoundTrack::create(
        TSoundTrackFormat(rate, 16, 1, TSound::INT), 4, data);
    const TFilePath path(dir.filePath("aiff_writer_rate.aiff").toStdWString());
    TSoundTrackWriterAiff writer(path);
    require(writer.save(track), "the file is written");
    TSoundTrackP back = readFile(path);
    require(
        back && int(back->getSampleRate()) == rate,
        "a written sample rate of " + std::to_string(rate) + " is read back");
    const short* again = reinterpret_cast<const short*>(back->getRawData());
    for (int i = 0; i < 4; ++i)
      require(again[i] == short(i * 1000 - 1500),
              "written samples are read back");
    TSystem::deleteFile(path);
  }
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    sampleRatesAreReadFromTheExtendedNumber();
    everyOffsetOfTheSampleDataIsRead();
    chunksAtOddFileOffsetsAreRead();
    eightAndTwentyFourBitSamplesAreRead();
    writtenFilesKeepTheirSampleRate();
  } catch (const std::exception& error) {
    std::cerr << "aiff_reader: FAILED: " << error.what() << '\n';
    return 1;
  } catch (const TException& error) {
    std::cerr << "aiff_reader: FAILED: TException\n";
    return 1;
  }
  std::cout << "aiff_reader: PASS\n";
  return 0;
}
