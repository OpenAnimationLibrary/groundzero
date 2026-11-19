// SPDX-License-Identifier: BSD-3-Clause

// The WAV reader on files built byte by byte in this test. A file whose header
// contradicts its data (a block size that is not the size of a frame, chunks
// longer than the file, a fmt chunk that is too short) must be refused with a
// TException, without dividing by zero or reading or writing past a buffer.

#include "tcommon.h"
#include "tfilepath.h"
#include "tfilepath_io.h"
#include "tsound.h"
#include "tsound_io.h"
#include "tsound_t.h"
#include "tsystem.h"

#include "wav/tsio_wav.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <cstdint>
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
  b.push_back(UCHAR(v));
  b.push_back(UCHAR(v >> 8));
}

void put32(Bytes& b, uint32_t v) {
  put16(b, v & 0xFFFF);
  put16(b, v >> 16);
}

void putTag(Bytes& b, const char* tag) { b.insert(b.end(), tag, tag + 4); }

struct Format {
  unsigned encoding;
  unsigned channels;
  uint32_t rate;
  unsigned blockSize;
  unsigned bits;
};

Bytes fmtBody(const Format& f) {
  Bytes b;
  put16(b, f.encoding);
  put16(b, f.channels);
  put32(b, f.rate);
  put32(b, f.rate * f.blockSize);
  put16(b, f.blockSize);
  put16(b, f.bits);
  return b;
}

Format pcm(unsigned channels, unsigned bits) {
  return {1, channels, 44100, channels * (bits / 8), bits};
}

// A chunk whose declared length need not be the length of its body.
Bytes chunk(const char* tag, uint32_t declared, const Bytes& body) {
  Bytes c;
  putTag(c, tag);
  put32(c, declared);
  c.insert(c.end(), body.begin(), body.end());
  if (declared == body.size() && body.size() % 2) c.push_back(0);
  return c;
}

Bytes chunk(const char* tag, const Bytes& body) {
  return chunk(tag, uint32_t(body.size()), body);
}

Bytes riff(const std::vector<Bytes>& chunks) {
  Bytes body;
  putTag(body, "WAVE");
  for (const Bytes& c : chunks) body.insert(body.end(), c.begin(), c.end());
  Bytes file;
  putTag(file, "RIFF");
  put32(file, uint32_t(body.size()));
  file.insert(file.end(), body.begin(), body.end());
  return file;
}

TFilePath writeFile(const char* name, const Bytes& bytes) {
  QDir dir(QDir::tempPath());
  QFile::remove(dir.filePath(name));
  TFilePath path(dir.filePath(name).toStdWString());
  Tofstream os(path, std::ios::binary);
  os.write(reinterpret_cast<const char*>(bytes.data()),
           std::streamsize(bytes.size()));
  require(bool(os), "the fixture file is written");
  return path;
}

TSoundTrackP readFile(const TFilePath& path) {
  TSoundTrackReaderWav reader(path);
  return reader.load();
}

Bytes samples(size_t count) {
  Bytes b;
  for (size_t i = 0; i < count; ++i) b.push_back(UCHAR(i * 7 + 1));
  return b;
}

void validFilesAreRead() {
  struct Case {
    Format format;
    size_t bytes;
    int frames;
  };
  const Case cases[] = {
      {pcm(1, 8), 10, 10},
      {pcm(2, 8), 20, 10},
      {pcm(1, 16), 20, 10},
      {pcm(2, 16), 40, 10},
      {pcm(1, 24), 30, 10},
      {pcm(2, 24), 60, 10},
      {{3, 1, 44100, 4, 32}, 40, 10},
      // A data length that is not a whole number of frames drops the rest.
      {pcm(2, 16), 42, 10},
  };
  for (const Case& c : cases) {
    const Bytes data     = samples(c.bytes);
    const TFilePath path = writeFile(
        "wav_reader_valid.wav",
        riff({chunk("fmt ", fmtBody(c.format)), chunk("data", data)}));
    TSoundTrackP track = readFile(path);
    require(track, "a consistent file is read");
    require(track->getSampleCount() == c.frames, "the frame count is read");
    require(int(track->getSampleRate()) == 44100, "the sample rate is read");
    require(memcmp(track->getRawData(), data.data(),
                   size_t(c.frames) * c.format.blockSize) == 0,
            "the samples are copied");
    TSystem::deleteFile(path);
  }
  // Other chunks around the two that matter, an odd-length one before them, and
  // a trailing chunk that the end of the file cuts.
  const Bytes data     = samples(8);
  const TFilePath path = writeFile(
      "wav_reader_valid.wav",
      riff({chunk("LIST", Bytes(3, 1)), chunk("fmt ", fmtBody(pcm(1, 16))),
            chunk("data", data), chunk("junk", 1000, Bytes(3, 0))}));
  TSoundTrackP track = readFile(path);
  require(track && track->getSampleCount() == 4, "other chunks are skipped");
  TSystem::deleteFile(path);
}

struct MalformedFile {
  const char* name;
  Bytes bytes;
};

std::vector<MalformedFile> malformedFiles() {
  const Bytes data = samples(40);
  const Bytes fmt  = fmtBody(pcm(2, 16));
  auto withFormat  = [&](const Format& f) {
    return riff({chunk("fmt ", fmtBody(f)), chunk("data", data)});
  };
  std::vector<MalformedFile> files;
  files.push_back({"block-size-zero", withFormat({1, 2, 44100, 0, 16})});
  files.push_back(
      {"block-size-larger-than-a-frame", withFormat({1, 2, 44100, 16, 16})});
  files.push_back(
      {"block-size-smaller-than-a-frame", withFormat({1, 2, 44100, 2, 16})});
  files.push_back({"zero-channels", withFormat({1, 0, 44100, 0, 16})});
  files.push_back({"zero-sample-size", withFormat({1, 2, 44100, 0, 0})});
  files.push_back(
      {"unsupported-sample-size", withFormat({1, 2, 44100, 3, 12})});
  files.push_back({"three-channels", withFormat({1, 3, 44100, 6, 16})});
  files.push_back({"fmt-too-short",
                   riff({chunk("fmt ", Bytes(8, 1)), chunk("data", data)})});
  files.push_back(
      {"fmt-cut-by-end-of-file", riff({chunk("fmt ", 16, Bytes(6, 1))})});
  files.push_back(
      {"fmt-length-past-end-of-file",
       riff({chunk("fmt ", 0x7FFFFFF0u, fmt), chunk("data", data)})});
  files.push_back(
      {"data-length-past-end-of-file",
       riff({chunk("fmt ", fmt), chunk("data", 0x10000000u, data)})});
  files.push_back(
      {"data-negative-length",
       riff({chunk("fmt ", fmt), chunk("data", 0xFFFFFFFFu, data)})});
  files.push_back({"data-cut-by-end-of-file",
                   riff({chunk("fmt ", fmt), chunk("data", 400, data)})});
  files.push_back({"negative-chunk-length",
                   riff({chunk("LIST", 0xFFFFFFF0u, Bytes(4, 0)),
                         chunk("fmt ", fmt), chunk("data", data)})});
  files.push_back({"header-cut-by-end-of-file", Bytes{'R', 'I', 'F', 'F', 0}});
  return files;
}

void malformedFilesAreRefused(const std::string& only) {
  for (const MalformedFile& file : malformedFiles()) {
    if (!only.empty() && only != file.name) continue;
    const TFilePath path = writeFile("wav_reader_bad.wav", file.bytes);
    bool refused         = false;
    try {
      TSoundTrackP track = readFile(path);
    } catch (const TException&) {
      refused = true;
    }
    TSystem::deleteFile(path);
    require(refused, std::string("the file '") + file.name +
                         "' is refused with a TException");
  }
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    if (argc > 1) {
      malformedFilesAreRefused(argv[1]);
      std::cout << "wav_reader: " << argv[1] << " refused\n";
      return 0;
    }
    validFilesAreRead();
    malformedFilesAreRefused("");
  } catch (const std::exception& error) {
    std::cerr << "wav_reader: FAILED: " << error.what() << '\n';
    return 1;
  } catch (const TException&) {
    std::cerr << "wav_reader: FAILED: TException\n";
    return 1;
  }
  std::cout << "wav_reader: PASS\n";
  return 0;
}
