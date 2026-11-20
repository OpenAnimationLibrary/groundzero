#include "automation_import.h"

#include "automation_drawing.h"
#include "automation_fx.h"
#include "automation_fx_profile.h"

#include "tdoubleparam.h"
#include "tfx.h"
#include "tpalette.h"
#include "tparamcontainer.h"
#include "tsimplecolorstyles.h"
#include "tvectorimage.h"
#include "toonz/levelproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <typeinfo>

namespace GroundzeroAutomation {
namespace {

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

// Unadmitted styles become plain solid styles of their colour and name; an
// admitted style and the palette's pages are left alone.
void stylesBecomeSolid(Context&) {
  ToonzScene scene;
  TXshSimpleLevel* level =
      scene.createNewLevel(PLI_XSHLEVEL, L"Styled")->getSimpleLevel();
  TPalette* palette = level->getPalette();
  const int plain   = palette->addStyle(new TSolidColorStyle(TPixel32::Red));
  auto* linked      = new TSolidColorStyle(TPixel32::Green);
  linked->setGlobalName(L"-studio-linked");
  const int link   = palette->addStyle(linked);
  auto* centerLine = new TCenterLineStrokeStyle(TPixel32::Blue, 0, 2.0);
  centerLine->setName(L"Pencil");
  const int line = palette->addStyle(centerLine);
  for (int id : {plain, link, line}) palette->getPage(0)->addStyle(id);
  const std::vector<int> replaced = admitImportStyles(palette);
  require(replaced == std::vector<int>({link, line}),
          "Only the linked and the non-solid style are replaced");
  for (int id : replaced) {
    const TColorStyle* style = palette->getStyle(id);
    require(typeid(*style) == typeid(TSolidColorStyle) &&
                style->getGlobalName().empty(),
            "A replaced style is an unlinked solid style");
  }
  require(palette->getStyle(line)->getMainColor() == TPixel32::Blue &&
              palette->getStyle(line)->getName() == L"Pencil",
          "A replaced style keeps its main colour and name");
  require(palette->getStyle(plain)->getMainColor() == TPixel32::Red,
          "An admitted style is unchanged");
  QString detail;
  require(drawingProfileLimit(level, palette, detail).isEmpty(),
          "The admitted palette passes the profile");
  require(admitImportStyles(palette).empty(), "Admission is idempotent");
}

TXshSimpleLevel* vectorLevel(ToonzScene& scene, const std::wstring& name,
                             int drawings, int first = 1) {
  TXshSimpleLevel* level =
      scene.createNewLevel(PLI_XSHLEVEL, name)->getSimpleLevel();
  for (int index = 0; index < drawings; ++index) {
    TVectorImageP image = new TVectorImage;
    image->setPalette(level->getPalette());
    level->setFrame(TFrameId(first + index), image);
  }
  return level;
}

// Each per-level limit is named without decoding a drawing.
void levelLimitsAreNamed(Context&) {
  ToonzScene scene;
  QString detail;
  TXshSimpleLevel* fits = vectorLevel(scene, L"Fits", 2);
  require(drawingProfileLimit(fits, fits->getPalette(), detail).isEmpty(),
          "A level inside the profile has no limit");
  TXshSimpleLevel* many = vectorLevel(scene, L"Many", 4097);
  require(
      drawingProfileLimit(many, many->getPalette(), detail) == "drawing_limit",
      "4097 drawings pass the drawing limit");
  TXshSimpleLevel* far = vectorLevel(scene, L"Far", 1, 70000);
  require(drawingProfileLimit(far, far->getPalette(), detail) == "format",
          "A vector drawing number past 65535 is a format limit");
  TXshSimpleLevel* wide =
      scene.createNewLevel(OVL_XSHLEVEL, L"Wide")->getSimpleLevel();
  wide->getProperties()->setImageRes(TDimension(9000, 1));
  require(drawingProfileLimit(wide, nullptr, detail) == "raster_size" &&
              detail.contains("9000"),
          "A raster wider than 8192 pixels is a raster size limit");
  std::unique_ptr<TPalette> referenced(fits->getPalette()->clone());
  referenced->setRefImgPath(TFilePath("reference.png"));
  require(
      drawingProfileLimit(fits, referenced.get(), detail) == "palette_limit",
      "A palette naming a reference image is a palette limit");
}

// Values outside the authoring ranges are moved to the nearest bound and the
// effect then passes its profile.
void valuesAreClamped(Context&) {
  TFxP fx     = makeAutomationEffect("STD_blurFx");
  auto* value = dynamic_cast<TDoubleParam*>(fx->getParams()->getParam("value"));
  require(value != nullptr, "The blur has a value parameter");
  value->setDefaultValue(300);
  QStringList violations;
  require(automationFxProfileViolations(fx.getPointer(), violations) &&
              violations.contains("value"),
          "A blur of 300 is outside its authoring range");
  clampAutomationFxValues(fx.getPointer());
  QStringList remaining;
  automationFxProfileViolations(fx.getPointer(), remaining);
  require(remaining.isEmpty(), "The clamped blur passes its profile");
  require(value->getDefaultValue() < 300 && value->getDefaultValue() > 0,
          "The value moved to its upper bound");
}

}  // namespace

void runAutomationImportTests(Context& context) {
  const std::pair<const char*, std::function<void(Context&)>> cases[]{
      {"styles become solid", stylesBecomeSolid},
      {"level limits are named", levelLimitsAreNamed},
      {"values are clamped", valuesAreClamped}};
  for (const auto& test : cases) {
    test.second(context);
    std::cout << "automation-import-test: " << test.first << " PASS\n";
  }
}

}  // namespace GroundzeroAutomation
