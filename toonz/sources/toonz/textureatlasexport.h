#pragma once

#ifndef TEXTUREATLASEXPORT_H
#define TEXTUREATLASEXPORT_H

// Export Texture Atlas command (textureatlasexport.cpp, see
// doc/animate_texture_atlas.md). The packer and the JSON files are in
// toonz/atlaspacker.h and toonz/atlasjson.h.

// TnzCore includes
#include "traster.h"
#include "tgeometry.h"

class TXshSimpleLevel;
class TFrameId;
class TCamera;

/*!
Renders a drawing at the camera resolution, cropped to its visible pixels, as
the Spine export does (it is its renderDrawing(), in spineexport.cpp). \e
stageToPixels converts stage coordinates into camera pixels. Returns in \e
center the center of the returned raster in the column reference, in camera
pixels (y up). Returns a null raster for a blank or unreadable drawing. The
raster is premultiplied, bottom row first, as all OpenToonz rasters.
*/
TRaster32P renderTrimmedDrawing(TXshSimpleLevel *sl, const TFrameId &fid,
                                const TCamera *camera,
                                const TAffine &stageToPixels, TPointD &center);

#endif  // TEXTUREATLASEXPORT_H
