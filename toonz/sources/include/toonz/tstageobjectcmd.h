#pragma once

#ifndef PEGBARCMD_INCLUDED
#define PEGBARCMD_INCLUDED

#include "tcommon.h"
#include <QPair>
#include <string>
#include <QPointF>

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

class TStageObjectId;
class TXsheetHandle;
class TObjectHandle;
class TColumnHandle;
class TFxHandle;
class TStageObjectSpline;
class TStageObject;

namespace TStageObjectCmd {

DVAPI void rename(const TStageObjectId &id, std::string name,
                  TXsheetHandle *xshHandle);
DVAPI void resetOffset(const TStageObjectId &id, TXsheetHandle *xshHandle);
DVAPI void resetCenterAndOffset(const TStageObjectId &id,
                                TXsheetHandle *xshHandle);
DVAPI void resetPosition(const TStageObjectId &id, TXsheetHandle *xshHandle);
DVAPI void setHandle(const TStageObjectId &id, std::string handle,
                     TXsheetHandle *xshHandle);
DVAPI void setParentHandle(const std::vector<TStageObjectId> &ids,
                           std::string handle, TXsheetHandle *xshHandle);
DVAPI void setParent(const TStageObjectId &id, TStageObjectId parentId,
                     std::string parentHandle, TXsheetHandle *xshHandle,
                     bool doUndo = true);
DVAPI void setSplineParent(TStageObjectSpline *spline, TStageObject *parentObj,
                           TXsheetHandle *xshHandle);

DVAPI void addNewCamera(TXsheetHandle *xshHandle, TObjectHandle *objHandle,
                        QPointF initialPos = QPointF());
DVAPI void addNewPegbar(TXsheetHandle *xshHandle, TObjectHandle *objHandle,
                        QPointF initialPos = QPointF());
DVAPI void setAsActiveCamera(TXsheetHandle *xshHandle,
                             TObjectHandle *objHandle);
DVAPI void addNewSpline(TXsheetHandle *xshHandle, TObjectHandle *objHandle,
                        TColumnHandle *colHandle,
                        QPointF initialPos = QPointF());
DVAPI void deleteSelection(
    const std::vector<TStageObjectId> &objIds,
    const std::list<QPair<TStageObjectId, TStageObjectId>> &links,
    const std::list<int> &splineIds, TXsheetHandle *xshHandle,
    TObjectHandle *objHandle, TFxHandle *fxHandle, bool doUndo = true);
DVAPI void group(const QList<TStageObjectId> ids, TXsheetHandle *xshHandle);
DVAPI void ungroup(int groupId, TXsheetHandle *xshHandle);
DVAPI void renameGroup(const QList<TStageObject *> objs,
                       const std::wstring &name, bool fromEditor,
                       TXsheetHandle *xshHandle);
DVAPI void duplicateObject(const QList<TStageObjectId> ids,
                           TXsheetHandle *xshHandle);
DVAPI void enableSplineAim(TStageObject *obj, int state,
                           TXsheetHandle *xshHandle);
DVAPI void enableSplineUppk(TStageObject *obj, bool toggled,
                            TXsheetHandle *xshHandle);

// Offset layers (see TStageObject::OffsetLayer). Each command is one undo.

//! Adds a layer, with a name made unique if it is used (or empty). Returns the
//! index of the layer, or -1 on failure.
DVAPI int addOffsetLayer(const TStageObjectId &id, const std::wstring &name,
                         TXsheetHandle *xshHandle);
DVAPI void removeOffsetLayer(const TStageObjectId &id, int index,
                             TXsheetHandle *xshHandle);
//! Returns false, and does nothing, if the name is empty or used by another
//! layer.
DVAPI bool renameOffsetLayer(const TStageObjectId &id, int index,
                             const std::wstring &name,
                             TXsheetHandle *xshHandle);
DVAPI void setOffsetLayerEnabled(const TStageObjectId &id, int index,
                                 bool enabled, TXsheetHandle *xshHandle);
//! Sets a key at \e frame if the weight is animated, and its value otherwise.
DVAPI void setOffsetLayerWeight(const TStageObjectId &id, int index,
                                double weight, int frame,
                                TXsheetHandle *xshHandle);
//! Adds the layer to the base curves, at the keys of the base curve, of the
//! layer and of its weight, and clears the curves of the layer. The result
//! is exact at those keys only. Returns false if the layer has nothing to bake.
DVAPI bool bakeOffsetLayer(const TStageObjectId &id, int index,
                           TXsheetHandle *xshHandle);
//! Adds a layer, with weight 0, whose curves are the base keyframed curves
//! minus their value at the frame 0. Returns the index of the layer, or -1
//! if no channel is animated.
DVAPI int trainOffsetLayerFromKeys(const TStageObjectId &id,
                                   const std::wstring &name,
                                   TXsheetHandle *xshHandle);

// Published channels (see TStageObject::PublishedChannel). Each command is one
// undo.

//! Publishes a channel of the object under the name. \e channel is a
//! TStageObject::Channel having offsets (not T_Path), or T_ChannelCount for
//! the weight of the offset layer named \e layer. Returns the index of the
//! port, or -1 and does nothing if the name is empty or used, or the channel
//! or the layer is not valid.
DVAPI int publishChannel(const TStageObjectId &id, const std::wstring &name,
                         int channel, const std::wstring &layer,
                         TXsheetHandle *xshHandle);
DVAPI void unpublishChannel(const TStageObjectId &id, int index,
                            TXsheetHandle *xshHandle);
}

#endif
