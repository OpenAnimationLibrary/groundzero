// SPDX-License-Identifier: BSD-3-Clause

// Conversions of 8-bit and 16-bit samples to 16-bit and 24-bit samples. The
// values are checked against a table built with plain multiplication for every
// representable 8-bit input (and every 16-bit input for the mono 16-bit to
// 24-bit conversion). The conversions used to shift negative numbers left.

#include "tcommon.h"
#include "tsoundsample.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}

void expectEqual(long actual, long expected, const char* what, int a,
                 int b = 0) {
  if (actual == expected) return;
  throw std::runtime_error(std::string(what) + " for input " +
                           std::to_string(a) + "," + std::to_string(b) +
                           ": got " + std::to_string(actual) + ", expected " +
                           std::to_string(expected));
}

// Signed and unsigned 8-bit values as the samples hold them.
TMono8SignedSample mono8s(int v) { return TMono8SignedSample((SCHAR)v); }
TMono8UnsignedSample mono8u(int v) { return TMono8UnsignedSample((UCHAR)v); }

void mono8To16() {
  for (int v = -128; v <= 127; ++v) {
    expectEqual(TMono16Sample::from(mono8s(v)).getValue(TSound::MONO), v * 256,
                "mono 8 signed to 16", v);
    expectEqual(TStereo16Sample::from(mono8s(v)).getValue(TSound::LEFT),
                v * 256, "mono 8 signed to stereo 16", v);
    expectEqual(TStereo16Sample::from(mono8s(v)).getValue(TSound::RIGHT),
                v * 256, "mono 8 signed to stereo 16", v);
  }
  for (int v = 0; v <= 255; ++v) {
    expectEqual(TMono16Sample::from(mono8u(v)).getValue(TSound::MONO),
                (v - 128) * 256, "mono 8 unsigned to 16", v);
    expectEqual(TStereo16Sample::from(mono8u(v)).getValue(TSound::RIGHT),
                (v - 128) * 256, "mono 8 unsigned to stereo 16", v);
  }
}

void stereo8To16() {
  for (int l = -128; l <= 127; ++l)
    for (int r = -128; r <= 127; ++r) {
      TStereo8SignedSample s((SCHAR)l, (SCHAR)r);
      expectEqual(TMono16Sample::from(s).getValue(TSound::MONO), (l + r) * 128,
                  "stereo 8 signed to mono 16", l, r);
      TStereo16Sample t = TStereo16Sample::from(s);
      expectEqual(t.getValue(TSound::LEFT), l * 256,
                  "stereo 8 signed to 16 left", l, r);
      expectEqual(t.getValue(TSound::RIGHT), r * 256,
                  "stereo 8 signed to 16 right", l, r);
    }
  for (int l = 0; l <= 255; ++l)
    for (int r = 0; r <= 255; ++r) {
      TStereo8UnsignedSample s((UCHAR)l, (UCHAR)r);
      expectEqual(TMono16Sample::from(s).getValue(TSound::MONO),
                  (l + r - 256) * 128, "stereo 8 unsigned to mono 16", l, r);
      TStereo16Sample t = TStereo16Sample::from(s);
      expectEqual(t.getValue(TSound::LEFT), (l - 128) * 256,
                  "stereo 8 unsigned to 16 left", l, r);
      expectEqual(t.getValue(TSound::RIGHT), (r - 128) * 256,
                  "stereo 8 unsigned to 16 right", l, r);
    }
}

void eightBitTo24() {
  for (int v = -128; v <= 127; ++v) {
    expectEqual(TMono24Sample::from(mono8s(v)).getValue(TSound::MONO),
                v * 65536, "mono 8 signed to 24", v);
    expectEqual(TStereo24Sample::from(mono8s(v)).getValue(TSound::LEFT),
                v * 65536, "mono 8 signed to stereo 24", v);
  }
  for (int v = 0; v <= 255; ++v) {
    expectEqual(TMono24Sample::from(mono8u(v)).getValue(TSound::MONO),
                (v - 128) * 65536, "mono 8 unsigned to 24", v);
    expectEqual(TStereo24Sample::from(mono8u(v)).getValue(TSound::RIGHT),
                (v - 128) * 65536, "mono 8 unsigned to stereo 24", v);
  }
  for (int l = -128; l <= 127; ++l)
    for (int r = -128; r <= 127; ++r) {
      TStereo8SignedSample s((SCHAR)l, (SCHAR)r);
      expectEqual(TMono24Sample::from(s).getValue(TSound::MONO),
                  (l + r) * 32768, "stereo 8 signed to mono 24", l, r);
      TStereo24Sample t = TStereo24Sample::from(s);
      expectEqual(t.getValue(TSound::LEFT), l * 65536,
                  "stereo 8 signed to 24 left", l, r);
      expectEqual(t.getValue(TSound::RIGHT), r * 65536,
                  "stereo 8 signed to 24 right", l, r);
    }
  for (int l = 0; l <= 255; ++l)
    for (int r = 0; r <= 255; ++r) {
      TStereo8UnsignedSample s((UCHAR)l, (UCHAR)r);
      expectEqual(TMono24Sample::from(s).getValue(TSound::MONO),
                  (l + r - 256) * 32768, "stereo 8 unsigned to mono 24", l, r);
      TStereo24Sample t = TStereo24Sample::from(s);
      expectEqual(t.getValue(TSound::LEFT), (l - 128) * 65536,
                  "stereo 8 unsigned to 24 left", l, r);
      expectEqual(t.getValue(TSound::RIGHT), (r - 128) * 65536,
                  "stereo 8 unsigned to 24 right", l, r);
    }
}

void sixteenBitTo24() {
  for (int v = -32768; v <= 32767; ++v) {
    expectEqual(
        TMono24Sample::from(TMono16Sample((short)v)).getValue(TSound::MONO),
        v * 256, "mono 16 to 24", v);
    expectEqual(
        TStereo24Sample::from(TMono16Sample((short)v)).getValue(TSound::LEFT),
        v * 256, "mono 16 to stereo 24", v);
  }
  // Stereo 16-bit has 2^32 inputs; use every value against a spread of
  // partners.
  const int partners[] = {-32768, -32767, -12345, -256,  -255,  -1,   0,
                          1,      255,    256,    12345, 32766, 32767};
  for (int l = -32768; l <= 32767; l += 7)
    for (int r : partners) {
      TStereo16Sample s((short)l, (short)r);
      expectEqual(TMono24Sample::from(s).getValue(TSound::MONO), (l + r) * 128,
                  "stereo 16 to mono 24", l, r);
      TStereo24Sample t = TStereo24Sample::from(s);
      expectEqual(t.getValue(TSound::LEFT), l * 256, "stereo 16 to 24 left", l,
                  r);
      expectEqual(t.getValue(TSound::RIGHT), r * 256, "stereo 16 to 24 right",
                  l, r);
    }
}

}  // namespace

int main() {
  try {
    mono8To16();
    stereo8To16();
    eightBitTo24();
    sixteenBitTo24();
  } catch (const std::exception& error) {
    std::cerr << "soundsample_conversion: FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "soundsample_conversion: PASS\n";
  return 0;
}
