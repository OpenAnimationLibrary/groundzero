#include "tools/artlayer.h"

// TnzBase includes
#include "tenv.h"

// TnzCore includes
#include "tproperty.h"
#include "tstroke.h"

// Qt includes
#include <QCoreApplication>

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

// The properties are shared by several tools, so that every tool option bar
// shows the same value. They are created on first use, since the tools bind
// them while being statically constructed.

TEnumProperty &targetProperty() {
  static TEnumProperty property("Art Layer:");
  if (property.getCount() == 0) {
    property.addValue(ART_OFF_WSTR);
    property.addValue(LINE_ART_WSTR);
    property.addValue(COLOUR_ART_WSTR);
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

bool ArtLayer::isEditable(const TStroke &stroke) {
  Target currentTarget = target();
  return currentTarget == TARGET_OFF ||
         isEditable(currentTarget, roleOf(stroke));
}

//-----------------------------------------------------------------------------

bool ArtLayer::isFaded(Role role) { return isFaded(target(), display(), role); }

//-----------------------------------------------------------------------------

void ArtLayer::applyTarget(TStroke &stroke) { applyTarget(target(), stroke); }

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
