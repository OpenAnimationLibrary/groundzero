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
// Reader of Photoshop brush files (.abr), sampled brush tips only.
//
// The parser works on a byte buffer and does not depend on Qt or on any
// other part of Toonz: it turns the file into a list of 8-bit grayscale tip
// images.
//
// Supported:
//  - ABR versions 1 and 2 (type 2, "sampled" brushes, 8 or 16 bit, raw or
//    PackBits compressed; the name is read for version 2 and the spacing
//    for both).
//  - ABR versions 6 to 10 (Photoshop CS2 and later): the '8BIM' 'samp'
//    section, sub versions 1 and 2, 8 or 16 bit, raw or PackBits compressed.
//    The tips are named "tip_001", "tip_002", ... as the names live in the
//    'desc' descriptor section, which is not read.
//
// Not supported (these are counted or reported as warnings, never read):
//  - Computed brushes of versions 1 and 2 (type 1, an ellipse described by
//    diameter, hardness, angle, roundness and spacing): they have no image.
//  - The 'desc' descriptor section of versions 6 and up: the dynamics of the
//    presets (shape dynamics, scattering, textures, dual brush, color
//    dynamics, transfer, ...), the names and the spacing. Those are
//    proprietary and Photoshop-specific; the tips are imported as plain
//    stamps.
//  - The 'patt' pattern section and any other section.
//  - Versions 3 to 5 and above 10.
//
// The pixels of a tip are a coverage mask: 0 is no paint and 255 is full
// paint, as stored in the file (16 bit tips keep their high byte).
//=============================================================================

namespace AbrParser {

// Budgets apply to the entire file, including multiple samp sections and
// rejected records. Callers can lower them for previews and untrusted imports.
struct Limits {
  size_t maxFileBytes = 256u * 1024u * 1024u;
  size_t maxDecodedBytes = 256u * 1024u * 1024u;
  size_t maxRecords = 2000;
  size_t maxNameUnits = 4096;
};

struct Tip {
  std::string name;  // UTF-8; "tip_001"... when the file has no name
  int width   = 0;
  int height  = 0;
  int spacing = -1;  // percent of the diameter, -1 when not stored
  int depth   = 8;   // bits per pixel in the file, 8 or 16
  std::vector<unsigned char> pixels;  // width * height, top row first
};

struct Result {
  int version    = 0;
  int subVersion = 0;  // versions 6 and up only
  std::vector<Tip> tips;
  int computedBrushes = 0;  // type 1 brushes of versions 1 and 2, skipped
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
