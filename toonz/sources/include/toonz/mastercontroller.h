#pragma once

#ifndef MASTERCONTROLLER_H
#define MASTERCONTROLLER_H

#include "tcommon.h"
#include "toonz/tstageobjectid.h"

#include <vector>

class TStageObject;
class TDoubleParam;
class TXsheet;

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

//================================================================================================
//
// Master controllers
//
//================================================================================================

/*!
  A master controller is an ordinary pegbar whose name starts with "ctrl_".
  Its value is the pegbar's Rotation channel, and other channels follow it
  through expressions such as "peg3.rot*30" (see the Master Controllers
  panel). The functions here are shared by the panel and by the controller
  widgets that the Animate tool draws in the viewer.

  Each controller shows as a widget: a slider, a 2D pad that also sets a
  second controller (its Y), or a pose blend slider. The widget chosen is kept
  in the controller's stage object (TStageObject::getControllerWidget()).
*/
namespace MasterController {

enum Widget { SliderWidget = 0, PadWidget = 1, PoseBlendWidget = 2 };

//! Returns whether obj is a master controller: a pegbar named "ctrl_...".
DVAPI bool isController(const TStageObject *obj);

//! Returns the curve holding the value of the controller, or null if obj is
//! not a controller.
DVAPI TDoubleParam *valueCurve(TStageObject *obj);

//! Returns the controllers of the xsheet, sorted by name.
DVAPI std::vector<TStageObject *> controllers(TXsheet *xsh);

//! Returns whether the controller has been bound to poses: some object has
//! the offset layers "<controller>_A" and "<controller>_B".
DVAPI bool hasPoseBlendLayers(TXsheet *xsh, const TStageObject *controller);

//! Returns the widget of the controller: the one chosen, or else a pose blend
//! when it is bound to poses, or else a slider.
DVAPI int widget(TXsheet *xsh, const TStageObject *controller);

//! Returns the controller giving the Y of the controller's pad: the one
//! chosen if it is still a controller, and the first other one otherwise.
//! Returns NoneId when the xsheet has no other controller.
DVAPI TStageObjectId padY(TXsheet *xsh, const TStageObject *controller);

}  // namespace MasterController

#endif  // MASTERCONTROLLER_H
