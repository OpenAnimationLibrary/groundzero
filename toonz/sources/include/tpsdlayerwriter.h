#pragma once

#ifndef TPSDLAYERWRITER_INCLUDED
#define TPSDLAYERWRITER_INCLUDED

#include "tcommon.h"
#include "traster.h"
#include "tfilepath.h"

#include <string>
#include <vector>

#undef DVAPI
#undef DVVAR
#ifdef IMAGE_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

//! A layer of a PSD file written by writeLayeredPsd().
struct TLayeredPsdLayer {
  std::wstring m_name;
  //! Premultiplied TRaster32P (8 bit) or TRaster64P (16 bit), the size of the
  //! merged image and of the same kind as it.
  TRasterP m_raster;
  int m_opacity  = 255;  //!< 0 (transparent) to 255 (opaque)
  bool m_visible = true;
};

//! Writes an RGBA PSD file with several layers, all of the size of the image.
/*!
  \e merged is the flattened image stored for the programs which do not read
  layers (premultiplied TRaster32P or TRaster64P). \e layers are listed from
  the bottom one to the top one and must have the same bit depth as \e merged.
  The layers use the normal blend mode, are RLE compressed (8 bit) or raw (16
  bit) as the single layer files of TLevelWriterPsd, and are named with an
  ASCII fallback plus a Unicode name. The resolution is stored in pixels per
  inch.
  Throws a TImageException if the file cannot be written.
*/
DVAPI void writeLayeredPsd(const TFilePath &path, const TRasterP &merged,
                           const std::vector<TLayeredPsdLayer> &layers,
                           double dpiX, double dpiY);

#endif  // TPSDLAYERWRITER_INCLUDED
