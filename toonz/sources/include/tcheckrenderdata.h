#pragma once

#ifndef TCHECKRENDERDATA_H
#define TCHECKRENDERDATA_H

#include "tpixel.h"

#include <sstream>
#include <string>
#include <vector>

// A value snapshot of the View checks. Render workers never consult the live
// Viewer, palette selection or Preferences. The values match ToonzCheck::Type.
struct TRenderCheckSettings {
  enum Check {
    Transparency = 0x01,
    BlackBg      = 0x02,
    Ink          = 0x04,
    Paint        = 0x08,
    Fill         = 0x10,
    Gap          = 0x20,
    InksOnly     = 0x40,
    Ink1         = 0x80
  };
  enum {
    SupportedChecks =
        Transparency | BlackBg | Ink | Paint | Fill | InksOnly | Ink1
  };

  bool m_enabled         = false;
  int m_checks           = 0;
  int m_colorIndex       = -1;
  bool m_fillOnlySavebox = true;
  // Ancestor (column, row) pairs, then the selected column in that xsheet.
  // Rows identify the child level when a column contains several sub-xsheets.
  std::vector<int> m_columnPath;
  // Session-local FX identity is resolved from columnPath after loading.
  // FX cloning preserves this identity, unlike object addresses.
  unsigned long m_targetFxId = 0;

  TPixel32 m_inkColor               = TPixel32::Red;
  TPixel32 m_ink1Color              = TPixel32::Red;
  TPixel32 m_paintColor             = TPixel32::Red;
  TPixel32 m_transparencyInkOnWhite = TPixel32::Black;
  TPixel32 m_transparencyInkOnBlack = TPixel32::White;
  TPixel32 m_transparencyPaint      = TPixel32(128, 128, 128);

  int activeChecks() const {
    return m_enabled ? (m_checks & SupportedChecks) : 0;
  }

  bool appliesTo(unsigned long fxId) const {
    return m_targetFxId != 0 && m_targetFxId == fxId &&
           (activeChecks() & ~BlackBg) != 0;
  }

  // Stored in an optional attribute on the existing output tag, so older
  // applications can still load the scene and ignore the new rendering mode.
  std::string serialize() const {
    if (!m_enabled) return std::string();
    std::ostringstream os;
    os << 1 << ' ' << m_checks << ' ' << m_colorIndex << ' '
       << int(m_fillOnlySavebox) << ' ' << m_columnPath.size();
    for (int value : m_columnPath) os << ' ' << value;
    const TPixel32 colors[] = {m_inkColor,
                               m_ink1Color,
                               m_paintColor,
                               m_transparencyInkOnWhite,
                               m_transparencyInkOnBlack,
                               m_transparencyPaint};
    for (const TPixel32 &color : colors)
      os << ' ' << int(color.r) << ' ' << int(color.g) << ' ' << int(color.b)
         << ' ' << int(color.m);
    return os.str();
  }

  bool deserialize(const std::string &text) {
    *this = TRenderCheckSettings();
    if (text.empty()) return true;
    std::istringstream is(text);
    TRenderCheckSettings value;
    int version, fillOnlySavebox, count;
    if (!(is >> version >> value.m_checks >> value.m_colorIndex >>
          fillOnlySavebox >> count) ||
        version != 1 || value.m_checks < 0 || value.m_checks > 255 ||
        value.m_colorIndex < -1 || value.m_colorIndex > 4095 ||
        (fillOnlySavebox != 0 && fillOnlySavebox != 1) || count < 0 ||
        count > 65 || (count != 0 && count % 2 == 0))
      return false;
    for (int i = 0; i < count; ++i) {
      int part;
      if (!(is >> part) || part < 0) return false;
      value.m_columnPath.push_back(part);
    }
    TPixel32 *colors[] = {&value.m_inkColor,
                          &value.m_ink1Color,
                          &value.m_paintColor,
                          &value.m_transparencyInkOnWhite,
                          &value.m_transparencyInkOnBlack,
                          &value.m_transparencyPaint};
    for (TPixel32 *color : colors) {
      int r, g, b, a;
      if (!(is >> r >> g >> b >> a) || r < 0 || r > 255 || g < 0 || g > 255 ||
          b < 0 || b > 255 || a < 0 || a > 255)
        return false;
      *color = TPixel32(r, g, b, a);
    }
    is >> std::ws;
    if (!is.eof()) return false;
    value.m_enabled         = true;
    value.m_fillOnlySavebox = fillOnlySavebox != 0;
    *this                   = value;
    return true;
  }

  std::string cacheKey() const {
    if (activeChecks() == 0) return std::string();
    return serialize() + " " + std::to_string(m_targetFxId);
  }

  bool operator==(const TRenderCheckSettings &other) const {
    return cacheKey() == other.cacheKey();
  }
  bool operator!=(const TRenderCheckSettings &other) const {
    return !(*this == other);
  }
};

#endif
