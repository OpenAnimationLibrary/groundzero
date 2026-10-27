#include "tools/artlayer.h"

// TnzBase includes
#include "tenv.h"

// TnzQt includes
#include "toonzqt/imageutils.h"

// TnzCore includes
#include "tproperty.h"
#include "tregion.h"
#include "tstroke.h"
#include "tvectorimage.h"

// Qt includes
#include <QCoreApplication>

// STD includes
#include <set>
#include <vector>

TEnv::IntVar VectorArtLayerTarget("VectorArtLayerTarget", 0);
TEnv::IntVar VectorArtLayerDisplay("VectorArtLayerDisplay", 0);

//*******************************************************************************
//    Local namespace  stuff
//*******************************************************************************

namespace {

#define ART_OFF_WSTR L"Off"
#define ART_BOTH_WSTR L"Both"
#define LINE_ART_WSTR L"Line Art"
#define COLOUR_ART_WSTR L"Colour Art"
#define UNDERLAY_WSTR L"Underlay"
#define OVERLAY_WSTR L"Overlay"

// The properties are shared by several tools, so that every tool option bar
// shows the same value. They are created on first use, since the tools bind
// them while being statically constructed.

TEnumProperty &targetProperty() {
  static TEnumProperty property("Art Layer:");
  if (property.getCount() == 0) {
    property.addValue(ART_OFF_WSTR);
    property.addValue(LINE_ART_WSTR);
    property.addValue(COLOUR_ART_WSTR);
    property.addValue(UNDERLAY_WSTR);
    property.addValue(OVERLAY_WSTR);
    property.setId("ArtLayer");
  }
  return property;
}

TEnumProperty &displayProperty() {
  static TEnumProperty property("Show Art:");
  if (property.getCount() == 0) {
    property.addValue(ART_BOTH_WSTR);
    property.addValue(LINE_ART_WSTR);
    property.addValue(COLOUR_ART_WSTR);
    property.setId("ArtLayerDisplay");
  }
  return property;
}

void setIndex(TEnumProperty &property, int index) {
  if (index >= 0 && index < property.getCount()) property.setIndex(index);
}

// Set by the Art Layer Manager; not saved.
int l_hiddenRoles     = 0;
bool l_colourArtShown = false;

std::vector<double> pathOf(const TStroke &stroke) {
  std::vector<double> path;
  for (int i = 0; i < stroke.getControlPointCount(); ++i) {
    TThickPoint point = stroke.getControlPoint(i);
    path.push_back(point.x);
    path.push_back(point.y);
  }
  return path;
}

}  // namespace

//*******************************************************************************
//    ArtLayer  implementation
//*******************************************************************************

void ArtLayer::loadSettings() {
  static bool loaded = false;
  if (loaded) return;
  loaded = true;

  setIndex(targetProperty(), VectorArtLayerTarget);
  setIndex(displayProperty(), VectorArtLayerDisplay);
}

//-----------------------------------------------------------------------------

ArtLayer::Target ArtLayer::target() {
  loadSettings();
  return Target(targetProperty().getIndex());
}

//-----------------------------------------------------------------------------

ArtLayer::ShowMode ArtLayer::display() {
  loadSettings();
  return ShowMode(displayProperty().getIndex());
}

//-----------------------------------------------------------------------------

ArtLayer::Filter::Filter(const TVectorImage &image)
    : m_image(&image), m_target(target()) {
  // Only these targets depend on the place of the strokes in the stack
  if (m_target == TARGET_UNDERLAY || m_target == TARGET_OVERLAY) {
    m_range  = colourRange(image);
    m_target = effectiveTarget(m_target, m_range);
  }
}

//-----------------------------------------------------------------------------

bool ArtLayer::Filter::isEditable(int index) const {
  if (m_target == TARGET_OFF) return true;
  if (index < 0 || index >= int(m_image->getStrokeCount())) return true;
  Role role =
      roleInStack(m_range, index, hasZeroThickness(*m_image->getStroke(index)));
  return ArtLayer::isEditable(m_target, role);
}

//-----------------------------------------------------------------------------

bool ArtLayer::isFaded(Role role) { return isFaded(target(), display(), role); }

//-----------------------------------------------------------------------------

void ArtLayer::applyTarget(TStroke &stroke) { applyTarget(target(), stroke); }

//-----------------------------------------------------------------------------

int ArtLayer::hiddenRoles() { return l_hiddenRoles; }

//-----------------------------------------------------------------------------

void ArtLayer::setRoleHidden(Role role, bool hidden) {
  if (hidden)
    l_hiddenRoles |= roleBit(role);
  else
    l_hiddenRoles &= ~roleBit(role);
}

//-----------------------------------------------------------------------------

bool ArtLayer::colourArtShown() { return l_colourArtShown; }

//-----------------------------------------------------------------------------

void ArtLayer::setColourArtShown(bool shown) { l_colourArtShown = shown; }

//-----------------------------------------------------------------------------

int ArtLayer::createColourArt(TVectorImage &image) {
  int strokeCount = int(image.getStrokeCount());

  // The paths that already have a Colour Art stroke get no second copy
  std::set<std::vector<double>> paths;
  for (int i = 0; i < strokeCount; ++i)
    if (hasZeroThickness(*image.getStroke(i)))
      paths.insert(pathOf(*image.getStroke(i)));

  std::vector<TStroke *> copies;
  for (int i = 0; i < strokeCount; ++i) {
    const TStroke &stroke = *image.getStroke(i);
    if (hasZeroThickness(stroke) || !paths.insert(pathOf(stroke)).second)
      continue;
    TStroke *copy = new TStroke(stroke);
    applyTarget(TARGET_COLOUR_ART, *copy);
    copies.push_back(copy);
  }
  if (copies.empty()) return 0;

  // Adding strokes recomputes the regions: keep what has been filled. The
  // image is owned by a smart pointer, as the fill functions need.
  TVectorImageP imageP(&image);
  std::vector<TFilledRegionInf> fills;
  ImageUtils::getFillingInformationInArea(imageP, fills, image.getBBox());

  // Below everything, in the order of the Line Art strokes
  for (auto it = copies.rbegin(); it != copies.rend(); ++it)
    image.addStroke(*it, false, true);

  ImageUtils::assignFillingInformation(image, fills);
  return int(copies.size());
}

//-----------------------------------------------------------------------------

void ArtLayer::bindProperties(TPropertyGroup &group) {
  group.bind(targetProperty());
  group.bind(displayProperty());
}

//-----------------------------------------------------------------------------

bool ArtLayer::onPropertyChanged(const std::string &propertyName) {
  if (propertyName == targetProperty().getName())
    VectorArtLayerTarget = targetProperty().getIndex();
  else if (propertyName == displayProperty().getName())
    VectorArtLayerDisplay = displayProperty().getIndex();
  else
    return false;
  return true;
}

//-----------------------------------------------------------------------------

void ArtLayer::updateTranslation() {
  TEnumProperty &targetProp = targetProperty();
  targetProp.setQStringName(
      QCoreApplication::translate("ArtLayer", "Art Layer:"));
  targetProp.setItemUIName(ART_OFF_WSTR,
                           QCoreApplication::translate("ArtLayer", "Off"));
  targetProp.setItemUIName(LINE_ART_WSTR,
                           QCoreApplication::translate("ArtLayer", "Line Art"));
  targetProp.setItemUIName(
      COLOUR_ART_WSTR, QCoreApplication::translate("ArtLayer", "Colour Art"));
  targetProp.setItemUIName(UNDERLAY_WSTR,
                           QCoreApplication::translate("ArtLayer", "Underlay"));
  targetProp.setItemUIName(OVERLAY_WSTR,
                           QCoreApplication::translate("ArtLayer", "Overlay"));

  TEnumProperty &displayProp = displayProperty();
  displayProp.setQStringName(
      QCoreApplication::translate("ArtLayer", "Show Art:"));
  displayProp.setItemUIName(ART_BOTH_WSTR,
                            QCoreApplication::translate("ArtLayer", "Both"));
  displayProp.setItemUIName(
      LINE_ART_WSTR, QCoreApplication::translate("ArtLayer", "Line Art"));
  displayProp.setItemUIName(
      COLOUR_ART_WSTR, QCoreApplication::translate("ArtLayer", "Colour Art"));
}
