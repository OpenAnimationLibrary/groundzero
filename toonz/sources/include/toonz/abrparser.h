#pragma once

#ifndef ABRPARSER_H
#define ABRPARSER_H

#include "tcommon.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

//=============================================================================
// Reader of Photoshop brush files (.abr): sampled tips and typed metadata.
//
// The parser works on a byte buffer and does not depend on Qt or on any
// other part of Toonz: it returns 8-bit grayscale tip images, legacy computed
// geometry and typed descriptor metadata as separate data.
//
// Supported:
//  - ABR versions 1 and 2 (type 2, "sampled" brushes, 8 or 16 bit, raw or
//    PackBits compressed; the name is read for version 2 and the spacing
//    for both).
//  - ABR 1/2 fixed type-1 computed ellipse records, retained as signed
//    diameter, roundness, angle, hardness and spacing fields without pixels.
//  - ABR versions 6 to 10 (Photoshop CS2 and later): the '8BIM' 'samp'
//    section, sub versions 1 and 2, 8 or 16 bit, raw or PackBits compressed.
//    The tips are named "tip_001", "tip_002", ... as the names live in the
//    'desc' section and are not yet associated with sampled resources.
//  - Version-16 Action Descriptor objects, lists, strings, enums, scalar
//    numbers/booleans, classes and opaque data. Decoding preserves types and
//    units; it does not implement the brush settings described by the values.
//
// Not interpreted (reported explicitly; never implied by decoded metadata):
//  - Computed brush geometry is decoded separately from sampled images; its
//    source rasterization/spacing behavior is not implemented by this reader.
//  - Brush behavior in 'desc' descriptors of versions 6 and up: the dynamics of
//  the
//    presets (shape dynamics, scattering, textures, dual brush, color
//    dynamics, transfer, ...), names and spacing. These need independently
//    specified adapters; current tip conversion imports plain stamps.
//  - Descriptor references ('obj ') retain documented property, class,
//    enumerated and raw-offset items as typed metadata only. Undocumented
//    reference layouts and unknown types reject their descriptor section.
//    No alias or external reference is resolved or executed.
//  - Pattern texture/brush association and unqualified channel encodings.
//    Version-1 'patt' resources with version-3 VM arrays are structurally
//    decoded; raw/VM-ZIP 8- and 16-bit channels yield bytes. No color
//    conversion or mask interpretation is inferred. Unknown sections remain
//    uninterpreted.
//  - Versions 3 to 5 and above 10.
//
// The pixels of a tip are a coverage mask: 0 is no paint and 255 is full
// paint, as stored in the file (16 bit tips keep their high byte).
//=============================================================================

namespace AbrParser {

// Budgets apply to the entire file, including multiple samp sections and
// rejected records. Callers can lower them for previews and untrusted imports.
struct Limits {
  size_t maxFileBytes       = 256u * 1024u * 1024u;
  size_t maxDecodedBytes    = 256u * 1024u * 1024u;
  size_t maxRecords         = 2000;
  size_t maxNameUnits       = 4096;
  size_t maxDescriptorNodes = 16384;
  size_t maxDescriptorBytes = 4u * 1024u * 1024u;
  size_t maxDescriptorDepth = 16;
};

// Typed descriptor data is separate from interpreted brush behavior. Unknown
// types cannot be skipped safely; their section remains explicitly undecoded.
struct DescriptorValue {
  std::string key, type, name, classId, unit, text;
  double number        = 0;
  std::int64_t integer = 0;
  bool boolean         = false;
  std::vector<DescriptorValue> children;
};

struct Tip {
  std::string name;  // UTF-8; "tip_001"... when the file has no name
  std::string
      sampleId;  // Explicit v6+ identifier; never match by list position.
  int width   = 0;
  int height  = 0;
  int spacing = -1;  // percent of the diameter, -1 when not stored
  int depth   = 8;   // bits per pixel in the file, 8 or 16
  std::vector<unsigned char> pixels;  // width * height, top row first
};

// Pattern resource channels retain their documented slot and bounds. Raw and
// documented VM-ZIP 8/16-bit channels decode; other encodings do not. These are
// image resources, not a claim about texture blend or brush association.
struct PatternChannel {
  bool written = false, decoded = false;
  int top = 0, left = 0, bottom = 0, right = 0;
  unsigned depth = 0, compression = 0;
  std::string encodingProfile;  // Explicit decoding interpretation, not parity.
  // Exact channel bytes, top-row-first; 16-bit samples retain both big-endian
  // bytes. No normalization, high-byte truncation or alpha inference.
  std::vector<unsigned char> pixels;
};
struct Pattern {
  std::string id, name;
  unsigned mode = 0, pointY = 0, pointX = 0;
  int top = 0, left = 0, bottom = 0, right = 0;
  std::vector<unsigned char> palette;
  std::vector<PatternChannel> channels;
};

struct Result {
  int version    = 0;
  int subVersion = 0;  // versions 6 and up only
  std::vector<Tip> tips;
  std::vector<DescriptorValue> descriptors;
  std::vector<Pattern> patterns;
  bool patternDataComplete = true;  // False after any malformed patt section.
  struct Computed {
    std::string name;
    int spacing = 0, diameter = 0, roundness = 0, angle = 0, hardness = 0;
  };
  std::vector<Computed> computed;
  int computedBrushes = 0;  // all type-1 records, including malformed entries
  int skippedSamples  = 0;  // sampled brushes which could not be decoded
  std::vector<std::string> warnings;
};

//! Parses the ABR file held by \b data. Returns false, and sets \b error, if
//! the buffer is not an ABR file of a supported version. Otherwise returns
//! true, even when some brushes had to be skipped or the file is truncated:
//! \b out holds the tips which could be read and the warnings tell what was
//! left out.
DVAPI bool parseAbr(const unsigned char *data, size_t size, Result &out,
                    std::string &error);

// Limit failures reject the entire import and leave no partial tips.
DVAPI bool parseAbr(const unsigned char *data, size_t size, Result &out,
                    std::string &error, const Limits &limits);

}  // namespace AbrParser

#endif  // ABRPARSER_H
