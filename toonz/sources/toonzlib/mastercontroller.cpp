#include "toonz/mastercontroller.h"

#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"

#include "tdoubleparam.h"
#include "tconvert.h"

#include <QString>

#include <algorithm>

namespace MasterController {

bool isController(const TStageObject *obj) {
  return obj && obj->getId().isPegbar() &&
         QString::fromStdString(obj->getName())
             .startsWith("ctrl_", Qt::CaseInsensitive);
}

//------------------------------------------------------------------------------

TDoubleParam *valueCurve(TStageObject *obj) {
  return isController(obj) ? obj->getParam(TStageObject::T_Angle) : nullptr;
}

//------------------------------------------------------------------------------

std::vector<TStageObject *> controllers(TXsheet *xsh) {
  std::vector<TStageObject *> result;
  if (!xsh) return result;
  TStageObjectTree *tree = xsh->getStageObjectTree();
  for (int i = 0; i < tree->getStageObjectCount(); ++i) {
    TStageObject *obj = tree->getStageObject(i);
    if (isController(obj)) result.push_back(obj);
  }
  std::sort(result.begin(), result.end(),
            [](const TStageObject *a, const TStageObject *b) {
              const std::string na = a->getName(), nb = b->getName();
              return na < nb || (na == nb && a->getId() < b->getId());
            });
  return result;
}

//------------------------------------------------------------------------------

bool hasPoseBlendLayers(TXsheet *xsh, const TStageObject *controller) {
  if (!xsh || !controller) return false;
  TStageObjectTree *tree  = xsh->getStageObjectTree();
  const std::wstring name = ::to_wstring(controller->getName());
  for (int i = 0; i < tree->getStageObjectCount(); ++i) {
    TStageObject *obj = tree->getStageObject(i);
    if (obj->findOffsetLayer(name + L"_A") >= 0 &&
        obj->findOffsetLayer(name + L"_B") >= 0)
      return true;
  }
  return false;
}

//------------------------------------------------------------------------------

int widget(TXsheet *xsh, const TStageObject *controller) {
  if (!controller) return SliderWidget;
  const int chosen = controller->getControllerWidget();
  if (chosen >= SliderWidget && chosen <= PoseBlendWidget) return chosen;
  return hasPoseBlendLayers(xsh, controller) ? PoseBlendWidget : SliderWidget;
}

//------------------------------------------------------------------------------

TStageObjectId padY(TXsheet *xsh, const TStageObject *controller) {
  if (!controller) return TStageObjectId::NoneId;
  const std::vector<TStageObject *> list = controllers(xsh);
  const TStageObjectId chosen            = controller->getControllerPadY();
  if (chosen != controller->getId())
    for (const TStageObject *obj : list)
      if (obj->getId() == chosen) return chosen;
  for (const TStageObject *obj : list)
    if (obj != controller) return obj->getId();
  return TStageObjectId::NoneId;
}

}  // namespace MasterController
