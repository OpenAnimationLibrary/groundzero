#pragma once

#ifndef LOTTIEEXPORTER_H
#define LOTTIEEXPORTER_H

#include "tgeometry.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
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
// forward declarations
class TXsheet;
class TVectorImage;
class TPalette;
class TXshSimpleLevel;
class TFrameId;

//=============================================================================

/*!
  The core of File > Export > Lottie (toonz/lottieexport.cpp): it turns an
  xsheet into a Lottie animation (Bodymovin JSON) without any GUI, so that it
  can be tested on a scene built in memory (tests/lottieexport_test.cpp).
  See doc/animate_lottie_export.md for the mapping; the maths are in
  toonz/lottiemath.h.
*/
namespace LottieExport {

//! A raster drawing, given by the caller as a PNG image.
struct RasterImage {
  QByteArray m_png;               //!< PNG file data
  int m_width = 0, m_height = 0;  //!< Size, pixels
  TPointD m_offset;  //!< Offset of the image center from the level origin,
                     //!< in image pixels (TRasterImage::getOffset())
};

//! Fills the image of a raster drawing; returns false if it cannot.
typedef std::function<bool(TXshSimpleLevel *, const TFrameId &, RasterImage &)>
    RasterRenderer;

struct Options {
  int m_r0           = 0;   //!< First exported row of the xsheet
  int m_r1           = -1;  //!< Last exported row, -1 for the last one
  double m_frameRate = 24.0;
  QString m_name;                 //!< Name of the animation
  bool m_embedImages = true;      //!< Raster images as data URIs in the JSON
  QString m_imagesFolder;         //!< Otherwise, the folder of the image files,
                                  //!< relative to the JSON file ("x_images/")
  RasterRenderer m_renderRaster;  //!< Empty: raster levels are skipped
};

//! An image file to write next to the JSON file (images not embedded).
struct ImageFile {
  QString m_fileName;  //!< Relative to the images folder
  QByteArray m_data;
};

struct Result {
  QJsonObject m_animation;  //!< The Lottie document
  std::vector<ImageFile> m_imageFiles;
  QStringList m_skipped;    //!< What could not be exported, one line each
  int m_contentLayers = 0;  //!< Visible layers (shapes, images, precomps)
};

//! Exports the rows m_r0 to m_r1 of \b xsheet, its camera being the
//! composition.
DVAPI Result exportXsheet(TXsheet *xsheet, const Options &options);

/*!
  Returns the shapes of a vector drawing (the "shapes" of a shape layer),
  top first: one group per filled region (with its subregions cut as holes,
  even-odd rule) and per stroke (a Lottie stroke when its thickness is about
  constant or the stroke is closed, else its filled outline), stacked as the
  drawing is drawn. \b toLayer maps the drawing coordinates (stage units) to
  the layer's (pixels, y-down). Notes about what is approximated are added
  to \b notes when given.
*/
DVAPI QJsonArray vectorShapes(const TVectorImage *image,
                              const TPalette *palette, const TAffine &toLayer,
                              QStringList *notes = nullptr);

}  // namespace LottieExport

#endif  // LOTTIEEXPORTER_H
