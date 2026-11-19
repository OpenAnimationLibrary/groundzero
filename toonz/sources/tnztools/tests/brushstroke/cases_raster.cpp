#include "cases.h"

#include "toonz/mypaintbrushstyle.h"
#include "trasterimage.h"
#include "ttoonzimage.h"

#include <QDir>
#include <QImage>

namespace brushstroke {

namespace {

// Every option of the Toonz raster Brush.
void toonzBrush(Fixture& f) {
  f.newToonzRasterLevel();
  f.useTool("T_Brush");
  f.setPair("Size", 2, 9);
  f.setNumber("Hardness:", 100);
  f.setNumber("Smooth:", 0);
  f.setEnum("Draw Order:", 0);
  f.setNumber("ModifierSize", 0);
  f.setBool("ModifierEraser", false);
  f.setEnum("ModifierEraserMode", 0);
  f.setBool("Lock Alpha", false);
  f.setBool("Pencil", false);
  f.setBool("Pressure", true);
  f.setBool("Assistants", true);
  f.setStyle(1);
}

// Ink of style 3 in a band, ink of style 2 in a column with a soft edge, and
// paint of style 4 in a block.
void addInkAndPaint(Fixture& f) {
  TToonzImageP image  = f.image();
  TRasterCM32P raster = image->getRaster();
  for (int y = 54; y < 62; ++y)
    for (int x = 20; x < 172; ++x) raster->pixels(y)[x] = TPixelCM32(3, 0, 0);
  for (int y = 30; y < 100; ++y)
    for (int x = 90; x < 100; ++x)
      raster->pixels(y)[x] = TPixelCM32(2, 0, x < 92 ? 128 : 0);
  for (int y = 66; y < 96; ++y)
    for (int x = 110; x < 160; ++x)
      raster->pixels(y)[x] = TPixelCM32(0, 4, 255);
  image->setSavebox(TRect(20, 30, 171, 99));
}

// The MyPaint style of the fixtures: libmypaint's default brush with a size
// that covers a few pixels.
int addMyPaintStyle(Fixture& f) {
  TMyPaintBrushStyle* style = new TMyPaintBrushStyle;
  style->setBaseValue(MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC, 1.2f);
  style->setMainColor(TPixel32(30, 60, 200));
  TPalette* palette = f.level()->getPalette();
  const int id      = palette->addStyle(style);
  palette->getPage(0)->addStyle(id);
  return id;
}

void addToonzPencilCases(std::vector<Case>& cases) {
  cases.push_back({"toonz_pencil", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     f.setBool("Pencil", true);
                     f.setBool("Pressure", false);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"toonz_pencil_pressure", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     f.setBool("Pencil", true);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"toonz_hard", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"toonz_hard_mouse", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     Play mouse;
                     mouse.tablet = false;
                     paint(f, r, "curve", mouse);
                   }});
  cases.push_back({"toonz_hard_smooth", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     f.setNumber("Smooth:", 10);
                     paint(f, r, "zigzag");
                   }});
  cases.push_back({"toonz_hard_dot", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     paint(f, r, "dot");
                   }});
  cases.push_back({"toonz_hard_shift_line", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     Play shift;
                     shift.shift = true;
                     paint(f, r, "line", shift);
                   }});
  cases.push_back({"toonz_hard_lock_alpha", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     addInkAndPaint(f);
                     f.setBool("Lock Alpha", true);
                     paint(f, r, "curve");
                   }});
  for (int order = 0; order < 3; ++order) {
    static const char* const names[] = {"toonz_hard_order_over",
                                        "toonz_hard_order_under",
                                        "toonz_hard_order_palette"};
    cases.push_back({names[order], [order](Fixture& f, Record& r) {
                       toonzBrush(f);
                       addInkAndPaint(f);
                       f.setEnum("Draw Order:", order);
                       paint(f, r, "curve");
                     }});
  }
}

void addToonzSoftCases(std::vector<Case>& cases) {
  cases.push_back({"toonz_soft", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     f.setNumber("Hardness:", 40);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"toonz_soft_pressure_off", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     f.setNumber("Hardness:", 70);
                     f.setBool("Pressure", false);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"toonz_soft_lock_alpha", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     addInkAndPaint(f);
                     f.setNumber("Hardness:", 40);
                     f.setBool("Lock Alpha", true);
                     paint(f, r, "curve");
                   }});
  for (int order = 0; order < 3; ++order) {
    static const char* const names[] = {"toonz_soft_order_over",
                                        "toonz_soft_order_under",
                                        "toonz_soft_order_palette"};
    cases.push_back({names[order], [order](Fixture& f, Record& r) {
                       toonzBrush(f);
                       addInkAndPaint(f);
                       f.setNumber("Hardness:", 40);
                       f.setEnum("Draw Order:", order);
                       paint(f, r, "curve");
                     }});
  }
  cases.push_back({"toonz_soft_mirror", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     f.addMetaColumn({mirrorReplicator()});
                     f.setNumber("Hardness:", 40);
                     paint(f, r, "line");
                   }});
  cases.push_back({"toonz_pencil_assistant", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     f.addMetaColumn({lineAssistant()});
                     f.setBool("Pencil", true);
                     paint(f, r, "line");
                   }});
}

void addToonzMyPaintCases(std::vector<Case>& cases) {
  cases.push_back({"toonz_mypaint", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     addInkAndPaint(f);
                     f.setStyle(addMyPaintStyle(f));
                     paint(f, r, "curve");
                   }});
  cases.push_back({"toonz_mypaint_size_lock", [](Fixture& f, Record& r) {
                     toonzBrush(f);
                     addInkAndPaint(f);
                     f.setStyle(addMyPaintStyle(f));
                     f.setNumber("ModifierSize", 1.5);
                     f.setBool("Lock Alpha", true);
                     paint(f, r, "curve");
                   }});
  for (int mode = 0; mode < 3; ++mode) {
    static const char* const names[] = {"toonz_mypaint_erase_lines",
                                        "toonz_mypaint_erase_areas",
                                        "toonz_mypaint_erase_all"};
    cases.push_back({names[mode], [mode](Fixture& f, Record& r) {
                       toonzBrush(f);
                       addInkAndPaint(f);
                       f.setStyle(addMyPaintStyle(f));
                       f.setBool("ModifierEraser", true);
                       f.setEnum("ModifierEraserMode", mode);
                       f.setNumber("ModifierSize", 1);
                       paint(f, r, "across");
                     }});
  }
}

//-----------------------------------------------------------------------------

// Every option of the raster Brush except the tip, which is the round dab.
void fullColorBrush(Fixture& f) {
  f.newFullColorLevel();
  f.useTool("T_Brush");
  f.setPair("Size", 3, 9);
  f.setNumber("Hardness:", 100);
  f.setPair("Opacity", 100, 100);
  f.setNumber("ModifierSize", 0);
  f.setNumber("ModifierOpacity", 100);
  f.setBool("ModifierEraser", false);
  f.setBool("Lock Alpha", false);
  f.setBool("Pressure", true);
  f.setBool("Assistants", true);
  f.setEnumValue("Tip:", L"<round>");
  f.setStyle(2);
}

// An opaque block, a half transparent block and nothing elsewhere.
void addColour(Fixture& f) {
  TRasterImageP image = f.image();
  TRaster32P raster   = image->getRaster();
  for (int y = 40; y < 90; ++y) {
    for (int x = 30; x < 90; ++x) raster->pixels(y)[x] = TPixel32(20, 150, 60);
    for (int x = 100; x < 160; ++x)
      raster->pixels(y)[x] = TPixel32(60, 30, 100, 128);
  }
}

void addFullColorCases(std::vector<Case>& cases) {
  cases.push_back({"raster_hard", [](Fixture& f, Record& r) {
                     fullColorBrush(f);
                     f.setBool("Pressure", false);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"raster_pressure", [](Fixture& f, Record& r) {
                     fullColorBrush(f);
                     f.setPair("Opacity", 30, 100);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"raster_mouse", [](Fixture& f, Record& r) {
                     fullColorBrush(f);
                     Play mouse;
                     mouse.tablet = false;
                     paint(f, r, "curve", mouse);
                   }});
  cases.push_back({"raster_soft", [](Fixture& f, Record& r) {
                     fullColorBrush(f);
                     f.setNumber("Hardness:", 30);
                     f.setPair("Size", 6, 14);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"raster_over_colour", [](Fixture& f, Record& r) {
                     fullColorBrush(f);
                     addColour(f);
                     f.setPair("Opacity", 60, 60);
                     paint(f, r, "across");
                   }});
  cases.push_back({"raster_lock_alpha", [](Fixture& f, Record& r) {
                     fullColorBrush(f);
                     addColour(f);
                     f.setBool("Lock Alpha", true);
                     paint(f, r, "across");
                   }});
  cases.push_back({"raster_dot", [](Fixture& f, Record& r) {
                     fullColorBrush(f);
                     paint(f, r, "dot");
                   }});
  cases.push_back({"raster_shift_line", [](Fixture& f, Record& r) {
                     fullColorBrush(f);
                     Play shift;
                     shift.shift = true;
                     paint(f, r, "line", shift);
                   }});
  cases.push_back({"raster_tip", [](Fixture& f, Record& r) {
                     const std::wstring tip = f.addBrushTip();
                     fullColorBrush(f);
                     f.setPair("Size", 10, 16);
                     f.setEnumValue("Tip:", tip);
                     paint(f, r, "curve");
                     f.setEnumValue("Tip:", L"<round>");
                   }});
  cases.push_back({"raster_mirror", [](Fixture& f, Record& r) {
                     fullColorBrush(f);
                     f.addMetaColumn({mirrorReplicator()});
                     paint(f, r, "line");
                   }});
  cases.push_back({"raster_assistant", [](Fixture& f, Record& r) {
                     fullColorBrush(f);
                     f.addMetaColumn({lineAssistant()});
                     paint(f, r, "line");
                   }});
}

void addFullColorMyPaintCases(std::vector<Case>& cases) {
  cases.push_back({"raster_mypaint", [](Fixture& f, Record& r) {
                     fullColorBrush(f);
                     addColour(f);
                     f.setStyle(addMyPaintStyle(f));
                     paint(f, r, "curve");
                   }});
  cases.push_back({"raster_mypaint_modifiers", [](Fixture& f, Record& r) {
                     fullColorBrush(f);
                     addColour(f);
                     f.setStyle(addMyPaintStyle(f));
                     f.setNumber("ModifierSize", 1);
                     f.setNumber("ModifierOpacity", 50);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"raster_mypaint_eraser", [](Fixture& f, Record& r) {
                     fullColorBrush(f);
                     addColour(f);
                     f.setStyle(addMyPaintStyle(f));
                     f.setBool("ModifierEraser", true);
                     f.setNumber("ModifierSize", 1);
                     paint(f, r, "across");
                   }});
  cases.push_back({"raster_mypaint_lock_alpha", [](Fixture& f, Record& r) {
                     fullColorBrush(f);
                     addColour(f);
                     f.setStyle(addMyPaintStyle(f));
                     f.setBool("Lock Alpha", true);
                     f.setNumber("ModifierSize", 1);
                     paint(f, r, "across");
                   }});
}

}  // namespace

void addToonzRasterBrushCases(std::vector<Case>& cases) {
  addToonzPencilCases(cases);
  addToonzSoftCases(cases);
  addToonzMyPaintCases(cases);
}

void addFullColorBrushCases(std::vector<Case>& cases) {
  addFullColorCases(cases);
  addFullColorMyPaintCases(cases);
}

}  // namespace brushstroke
