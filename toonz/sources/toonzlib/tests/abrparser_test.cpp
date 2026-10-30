// Regression test of the Photoshop brush (ABR) reader (AbrParser::parseAbr).
// The files are built in code, byte by byte, following the layout the reader
// documents, so that every field it reads is covered: versions 1, 2 and 6
// (sub versions 1 and 2), raw and PackBits rows, 8 and 16 bit, padding of the
// samples, skipped sections and computed brushes, and damaged files.

#include "toonz/abrparser.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

typedef std::vector<unsigned char> Bytes;

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

//------------------------------------------------------------------

void put8(Bytes &b, unsigned v) { b.push_back((unsigned char)v); }
void put16(Bytes &b, unsigned v) {
  put8(b, v >> 8);
  put8(b, v & 0xFF);
}
void put32(Bytes &b, unsigned v) {
  put16(b, v >> 16);
  put16(b, v & 0xFFFF);
}
void putBytes(Bytes &b, const Bytes &v) {
  b.insert(b.end(), v.begin(), v.end());
}
void putText(Bytes &b, const char *s) {
  while (*s) put8(b, (unsigned char)*s++);
}
void pad4(Bytes &b) {
  while (b.size() % 4) put8(b, 0);
}

// PackBits rows written by hand, so that the test does not depend on an
// encoder. Row of 4 pixels each: literal, repeat, and a mix with a no-op.
const unsigned char rowLiteral[] = {0x03, 10, 20, 30, 40};  // 10 20 30 40
const unsigned char rowRepeat[]  = {0xFD, 255};             // 255 x 4
const unsigned char rowMixed[]   = {0xFF, 0, 0x01,
                                    7,    9, 0x80};  // 0 0 7 9, a no-op

template <size_t N>
Bytes bytesOf(const unsigned char (&a)[N]) {
  return Bytes(a, a + N);
}

// Image data after the depth field of a 4x3 tip: compression flag, the row
// lengths and the rows.
Bytes imageRle4x3() {
  Bytes b;
  put8(b, 1);
  put16(b, sizeof(rowLiteral));
  put16(b, sizeof(rowRepeat));
  put16(b, sizeof(rowMixed));
  putBytes(b, bytesOf(rowLiteral));
  putBytes(b, bytesOf(rowRepeat));
  putBytes(b, bytesOf(rowMixed));
  return b;
}

void requireImage4x3(const AbrParser::Tip &tip) {
  const unsigned char expected[12] = {10,  20,  30, 40, 255, 255,
                                      255, 255, 0,  0,  7,   9};
  require(tip.width == 4 && tip.height == 3, "4x3: size");
  require(tip.pixels.size() == 12, "4x3: pixel count");
  for (int i = 0; i < 12; ++i)
    require(tip.pixels[i] == expected[i], "4x3: pixel");
}

//------------------------------------------------------------------

// Bounds and depth, then the image data, of a sample of any version.
Bytes boundsAndImage(int w, int h, int depth, const Bytes &image) {
  Bytes b;
  put32(b, 0);  // top
  put32(b, 0);  // left
  put32(b, h);  // bottom
  put32(b, w);  // right
  put16(b, depth);
  putBytes(b, image);
  return b;
}

// A sample of a version 6 'samp' section: the length, 37 bytes of id, the
// unused bytes of the sub version, the bounds and the image; padded.
Bytes sampleV6(int subVersion, const Bytes &boundsAndImageData) {
  Bytes body;
  put8(body, 36);
  putText(body, "01234567-89ab-cdef-0123-456789abcdef");
  body.resize(body.size() + (subVersion == 1 ? 10 : 264), 0);
  putBytes(body, boundsAndImageData);

  Bytes b;
  put32(b, (unsigned)body.size());
  putBytes(b, body);
  pad4(b);
  return b;
}

Bytes section(const char *tag, const Bytes &data) {
  Bytes b;
  putText(b, "8BIM");
  putText(b, tag);
  put32(b, (unsigned)data.size());
  putBytes(b, data);
  return b;
}

Bytes fileV6(int subVersion, const Bytes &sections) {
  Bytes b;
  put16(b, 6);
  put16(b, subVersion);
  putBytes(b, sections);
  return b;
}

// A sampled brush (type 2) of version 1 or 2, without the type and length.
Bytes sampleV12(int version, int spacing, const std::vector<unsigned> &name,
                const Bytes &boundsAndImageData) {
  Bytes b;
  put32(b, 0);  // misc
  put16(b, spacing);
  if (version == 2) {
    put32(b, (unsigned)name.size());
    for (size_t i = 0; i < name.size(); ++i) put16(b, name[i]);
  }
  put8(b, 1);                               // antialiasing
  for (int i = 0; i < 4; ++i) put16(b, 0);  // 16 bit bounds, not used
  putBytes(b, boundsAndImageData);
  return b;
}

void putBrush(Bytes &file, int type, const Bytes &body) {
  put16(file, type);
  put32(file, (unsigned)body.size());
  putBytes(file, body);
}

Bytes fileV12(int version, int count) {
  Bytes b;
  put16(b, version);
  put16(b, count);
  return b;
}

//------------------------------------------------------------------

bool parse(const Bytes &file, AbrParser::Result &out, std::string &error) {
  return AbrParser::parseAbr(file.empty() ? 0 : &file[0], file.size(), out,
                             error);
}

//------------------------------------------------------------------

void testV6PackBits() {
  Bytes sections;
  putBytes(sections, section("desc", Bytes(7, 0xAA)));  // skipped
  putBytes(
      sections,
      section("samp", sampleV6(2, boundsAndImage(4, 3, 8, imageRle4x3()))));
  putBytes(sections, section("patt", Bytes(5, 0x55)));  // skipped

  AbrParser::Result r;
  std::string error;
  require(parse(fileV6(2, sections), r, error), "v6: parse failed");
  require(r.version == 6 && r.subVersion == 2, "v6: version");
  require(r.tips.size() == 1, "v6: tip count");
  require(r.skippedSamples == 0 && r.warnings.size() == 2,
          "v6: skipped behavioral metadata must be reported");
  require(r.warnings[0].find("descriptors") != std::string::npos &&
              r.warnings[1].find("Pattern") != std::string::npos,
          "v6: incorrect compatibility diagnostics");
  require(r.tips[0].name == "tip_001", "v6: name");
  require(r.tips[0].depth == 8, "v6: depth");
  requireImage4x3(r.tips[0]);
}

void testV6RawPaddedSamples() {
  // sub version 1; two raw samples of an odd length, the second one after
  // the padding of the first
  Bytes img1;
  put8(img1, 0);
  const unsigned char px1[4] = {1, 2, 3, 4};
  img1.insert(img1.end(), px1, px1 + 4);
  Bytes img2;
  put8(img2, 0);
  const unsigned char px2[3] = {200, 100, 50};
  img2.insert(img2.end(), px2, px2 + 3);

  Bytes samples;
  putBytes(samples, sampleV6(1, boundsAndImage(2, 2, 8, img1)));
  putBytes(samples, sampleV6(1, boundsAndImage(3, 1, 8, img2)));

  AbrParser::Result r;
  std::string error;
  require(parse(fileV6(1, section("samp", samples)), r, error),
          "v6 raw: parse failed");
  require(r.subVersion == 1, "v6 raw: sub version");
  require(r.tips.size() == 2, "v6 raw: tip count");
  require(r.tips[0].width == 2 && r.tips[0].height == 2, "v6 raw: size 1");
  for (int i = 0; i < 4; ++i)
    require(r.tips[0].pixels[i] == px1[i], "v6 raw: pixel 1");
  require(r.tips[1].width == 3 && r.tips[1].height == 1, "v6 raw: size 2");
  for (int i = 0; i < 3; ++i)
    require(r.tips[1].pixels[i] == px2[i], "v6 raw: pixel 2");
  require(r.tips[1].name == "tip_002", "v6 raw: name 2");
}

void testV6SixteenBit() {
  // 2x1 tip, 16 bit, raw: the high byte is kept
  Bytes img;
  put8(img, 0);
  put16(img, 0x8001);
  put16(img, 0xFFFF);

  AbrParser::Result r;
  std::string error;
  require(parse(fileV6(2, section("samp",
                                  sampleV6(2, boundsAndImage(2, 1, 16, img)))),
                r, error),
          "16 bit: parse failed");
  require(r.tips.size() == 1, "16 bit: tip count");
  require(r.tips[0].depth == 16, "16 bit: depth");
  require(r.tips[0].pixels.size() == 2 && r.tips[0].pixels[0] == 0x80 &&
              r.tips[0].pixels[1] == 0xFF,
          "16 bit: pixels");
}

void testV2ComputedAndNamed() {
  Bytes file = fileV12(2, 2);

  // computed brush (type 1): skipped
  putBrush(file, 1, Bytes(14, 0x11));

  // sampled brush named "Rn" + e acute + a U+1F600 surrogate pair + null
  std::vector<unsigned> name;
  name.push_back('R');
  name.push_back('n');
  name.push_back(0x00E9);
  name.push_back(0xD83D);
  name.push_back(0xDE00);
  name.push_back(0);
  putBrush(file, 2,
           sampleV12(2, 25, name, boundsAndImage(4, 3, 8, imageRle4x3())));

  AbrParser::Result r;
  std::string error;
  require(parse(file, r, error), "v2: parse failed");
  require(r.version == 2, "v2: version");
  require(r.computedBrushes == 1, "v2: computed count");
  require(r.tips.size() == 1, "v2: tip count");
  require(r.tips[0].name == "Rn\xC3\xA9\xF0\x9F\x98\x80", "v2: name");
  require(r.tips[0].spacing == 25, "v2: spacing");
  requireImage4x3(r.tips[0]);
}

void testV1RawAndUnknownType() {
  Bytes img;
  put8(img, 0);
  const unsigned char px[6] = {0, 255, 128, 64, 32, 16};
  img.insert(img.end(), px, px + 6);

  Bytes file = fileV12(1, 3);
  putBrush(
      file, 2,
      sampleV12(1, 10, std::vector<unsigned>(), boundsAndImage(3, 2, 8, img)));
  putBrush(file, 9, Bytes(3, 0));  // unknown type: warning
  putBrush(file, 1, Bytes(14, 0));

  AbrParser::Result r;
  std::string error;
  require(parse(file, r, error), "v1: parse failed");
  require(r.tips.size() == 1, "v1: tip count");
  require(r.tips[0].name == "tip_001", "v1: name");
  require(r.tips[0].spacing == 10, "v1: spacing");
  require(r.tips[0].width == 3 && r.tips[0].height == 2, "v1: size");
  for (int i = 0; i < 6; ++i)
    require(r.tips[0].pixels[i] == px[i], "v1: pixel");
  require(r.computedBrushes == 1, "v1: computed count");
  require(r.warnings.size() == 1, "v1: unknown type warning");
}

void testBadSampleIsSkipped() {
  // The first sample has a PackBits row which unpacks to 3 pixels instead of
  // 4; it is skipped and the second sample is still read.
  const unsigned char shortRow[] = {0x02, 1, 2, 3};
  Bytes bad;
  put8(bad, 1);
  put16(bad, sizeof(shortRow));
  put16(bad, sizeof(rowRepeat));
  put16(bad, sizeof(rowRepeat));
  putBytes(bad, bytesOf(shortRow));
  putBytes(bad, bytesOf(rowRepeat));
  putBytes(bad, bytesOf(rowRepeat));

  Bytes samples;
  putBytes(samples, sampleV6(2, boundsAndImage(4, 3, 8, bad)));
  putBytes(samples, sampleV6(2, boundsAndImage(4, 3, 8, imageRle4x3())));

  AbrParser::Result r;
  std::string error;
  require(parse(fileV6(2, section("samp", samples)), r, error),
          "bad sample: parse failed");
  require(r.skippedSamples == 1, "bad sample: skipped count");
  require(r.warnings.size() == 1, "bad sample: warning");
  require(r.tips.size() == 1, "bad sample: tip count");
  require(r.tips[0].name == "tip_002", "bad sample: name");
  requireImage4x3(r.tips[0]);
}

void testRejected() {
  AbrParser::Result r;
  std::string error;
  require(!parse(Bytes(), r, error) && !error.empty(), "empty file accepted");

  Bytes three;
  put16(three, 3);
  put16(three, 1);
  require(!parse(three, r, error) && !error.empty(), "version 3 accepted");

  // unusable sizes and depths are skipped, not read
  Bytes samples;
  putBytes(samples, sampleV6(2, boundsAndImage(0, 3, 8, Bytes(1, 0))));
  putBytes(samples, sampleV6(2, boundsAndImage(2, 2, 12, Bytes(1, 0))));
  putBytes(samples, sampleV6(2, boundsAndImage(20000, 20000, 8, Bytes(1, 0))));
  require(parse(fileV6(2, section("samp", samples)), r, error),
          "unusable samples: parse failed");
  require(r.tips.empty() && r.skippedSamples == 3, "unusable samples: read");
}

void testBudgetsAndVersions() {
  AbrParser::Result r;
  std::string error;
  for (int version = 6; version <= 10; ++version) {
    for (int subversion = 1; subversion <= 2; ++subversion) {
      Bytes file = fileV6(subversion, section("samp", sampleV6(
          subversion, boundsAndImage(4, 3, 8, imageRle4x3()))));
      file[1] = static_cast<unsigned char>(version);
      require(parse(file, r, error), "documented version pair rejected");
      require(r.tips.size() == 1, "documented version pair lost tip");
      requireImage4x3(r.tips[0]);
    }
    for (unsigned subversion : {0u, 3u, 65535u}) {
      Bytes file;
      put16(file, version);
      put16(file, subversion);
      require(!parse(file, r, error), "unknown subversion accepted");
      require(r.tips.empty() && !error.empty(), "rejection retained tips");
    }
  }

  // Each sample fits alone; together they exceed the aggregate allocation
  // budget. Separate sections must not reset accounting.
  Bytes sections;
  for (int i = 0; i < 2; ++i)
    putBytes(sections, section("samp", sampleV6(
        2, boundsAndImage(4, 3, 8, imageRle4x3()))));
  const Bytes file = fileV6(2, sections);
  AbrParser::Limits limits;
  limits.maxDecodedBytes = 23;
  require(!AbrParser::parseAbr(file.data(), file.size(), r, error, limits),
          "aggregate allocation limit ignored");
  require(r.tips.empty(), "budget failure returned partial import");
  limits.maxDecodedBytes = 24;
  require(AbrParser::parseAbr(file.data(), file.size(), r, error, limits) &&
              r.tips.size() == 2,
          "exact allocation budget rejected");
  limits.maxRecords = 3;  // Two sections plus two sample records need four.
  require(!AbrParser::parseAbr(file.data(), file.size(), r, error, limits) &&
              r.tips.empty(), "section records escaped budget");
  limits = AbrParser::Limits();
  limits.maxFileBytes = file.size() - 1;
  require(!AbrParser::parseAbr(file.data(), file.size(), r, error, limits),
          "input byte limit ignored");

  Bytes named = fileV12(2, 1);
  putBrush(named, 2, sampleV12(2, 25, std::vector<unsigned>(5, 'A'),
                               boundsAndImage(4, 3, 8, imageRle4x3())));
  limits = AbrParser::Limits();
  limits.maxNameUnits = 3;
  require(!AbrParser::parseAbr(named.data(), named.size(), r, error, limits) &&
              r.tips.empty(), "name allocation escaped budget");

  Bytes withBehavior;
  putBytes(withBehavior, section("desc", Bytes(4, 0)));
  putBytes(withBehavior, section("patt", Bytes(4, 0)));
  putBytes(withBehavior, sections);
  require(parse(fileV6(2, withBehavior), r, error) && r.tips.size() == 2,
          "behavioral metadata prevented tip extraction");
  require(r.warnings.size() == 2,
          "tip-only extraction hid unimported behavior");
}

void testTruncation() {
  // Every prefix of a valid file must be handled without reading past the
  // end; whatever tips come out are complete.
  Bytes v6;
  {
    Bytes sections;
    putBytes(sections, section("desc", Bytes(7, 0xAA)));
    putBytes(
        sections,
        section("samp", sampleV6(2, boundsAndImage(4, 3, 8, imageRle4x3()))));
    v6 = fileV6(2, sections);
  }
  Bytes v2 = fileV12(2, 2);
  putBrush(v2, 1, Bytes(14, 0x11));
  std::vector<unsigned> name(1, 'A');
  putBrush(v2, 2,
           sampleV12(2, 25, name, boundsAndImage(4, 3, 8, imageRle4x3())));

  const Bytes *files[2] = {&v6, &v2};
  for (int f = 0; f < 2; ++f) {
    for (size_t n = 0; n < files[f]->size(); ++n) {
      Bytes prefix(files[f]->begin(), files[f]->begin() + n);
      AbrParser::Result r;
      std::string error;
      bool ok = parse(prefix, r, error);
      require(ok || n < 4, "truncation: a prefix with a header was rejected");
      for (size_t i = 0; i < r.tips.size(); ++i)
        require(r.tips[i].pixels.size() ==
                    (size_t)r.tips[i].width * r.tips[i].height,
                "truncation: incomplete tip");
      // the last bytes of a version 6 file are padding
      if (n + 4 <= files[f]->size())
        require(r.tips.empty(), "truncation: tip from damaged data");
    }
  }
}

}  // namespace

int main() {
  try {
    testV6PackBits();
    testV6RawPaddedSamples();
    testV6SixteenBit();
    testV2ComputedAndNamed();
    testV1RawAndUnknownType();
    testBadSampleIsSkipped();
    testRejected();
    testBudgetsAndVersions();
    testTruncation();
  } catch (const std::exception &e) {
    std::cerr << "abrparser_test failed: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "abrparser_test passed" << std::endl;
  return 0;
}
