#include "cases.h"

#include "tregion.h"
#include "trasterimage.h"
#include "tstroke.h"
#include "ttoonzimage.h"

namespace brushstroke {

namespace {

TStroke* wave(double y, double thickness, int style) {
  std::vector<TThickPoint> points;
  for (int i = 0; i <= 8; ++i)
    points.push_back(
        TThickPoint(-44 + 11 * i, y + (i % 2 ? 6 : -6), thickness));
  TStroke* stroke = new TStroke(points);
  stroke->setStyle(style);
  return stroke;
}

// Three waves of styles 1, 2 and 3 and a small closed shape of style 1
// filled with style 4.
void vectorEraser(Fixture& f) {
  f.newVectorLevel();
  TVectorImageP image = f.image();
  image->addStroke(wave(-12, 1.5, 1));
  image->addStroke(wave(0, 2.5, 2));
  image->addStroke(wave(12, 1.0, 3));
  TStroke* box = new TStroke(std::vector<TThickPoint>{{-8, -4, 1},
                                                      {0, -4, 1},
                                                      {8, -4, 1},
                                                      {8, 0, 1},
                                                      {8, 4, 1},
                                                      {0, 4, 1},
                                                      {-8, 4, 1},
                                                      {-8, 0, 1},
                                                      {-8, -4, 1}});
  box->setStyle(1);
  box->setSelfLoop(true);
  image->addStroke(box);
  image->findRegions();
  if (TRegion* region = image->getRegion(TPointD(0, 1))) region->setStyle(4);
  f.useTool("T_Eraser");
  f.setNumber("Size:", 10);
  f.setEnumValue("Type:", L"Normal");
  f.setBool("Selective", false);
  f.setBool("Invert", false);
  f.setBool("Frame Range", false);
  f.setEnum("Art Layer:", 0);
  f.setStyle(1);
  f.tool()->onEnter();
}

void addVectorEraserCases(std::vector<Case>& cases) {
  cases.push_back({"erase_vector_normal", [](Fixture& f, Record& r) {
                     vectorEraser(f);
                     Play steep;
                     steep.scale = 0.9;
                     paint(f, r, "curve", steep);
                   }});
  cases.push_back({"erase_vector_normal_selective", [](Fixture& f, Record& r) {
                     vectorEraser(f);
                     f.setBool("Selective", true);
                     f.setStyle(2);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"erase_vector_normal_large_zoom", [](Fixture& f, Record& r) {
                     vectorEraser(f);
                     f.setNumber("Size:", 40);
                     f.viewer.pixelSize = 4;
                     paint(f, r, "across");
                   }});
  cases.push_back({"erase_vector_rect", [](Fixture& f, Record& r) {
                     vectorEraser(f);
                     f.setEnumValue("Type:", L"Rectangular");
                     Play wide;
                     wide.scale = 1.4;
                     paint(f, r, "line", wide);
                   }});
  cases.push_back({"erase_vector_rect_invert", [](Fixture& f, Record& r) {
                     vectorEraser(f);
                     f.setEnumValue("Type:", L"Rectangular");
                     f.setBool("Invert", true);
                     Play small;
                     small.scale = 0.4;
                     paint(f, r, "line", small);
                   }});
  cases.push_back({"erase_vector_freehand", [](Fixture& f, Record& r) {
                     vectorEraser(f);
                     f.setEnumValue("Type:", L"Freehand");
                     paint(f, r, "loop");
                   }});
  cases.push_back(
      {"erase_vector_freehand_selective", [](Fixture& f, Record& r) {
         vectorEraser(f);
         f.setEnumValue("Type:", L"Freehand");
         f.setBool("Selective", true);
         f.setStyle(1);
         Play large;
         large.scale = 1.6;
         paint(f, r, "loop", large);
       }});
  cases.push_back({"erase_vector_segment", [](Fixture& f, Record& r) {
                     vectorEraser(f);
                     f.setEnumValue("Type:", L"Segment");
                     paint(f, r, "across");
                   }});
}

//-----------------------------------------------------------------------------

// Ink of styles 1 and 3 with soft edges, and paint of styles 2 and 4.
void toonzEraser(Fixture& f) {
  f.newToonzRasterLevel();
  TToonzImageP image  = f.image();
  TRasterCM32P raster = image->getRaster();
  for (int y = 20; y < 108; ++y)
    for (int x = 16; x < 176; ++x) {
      const int band = (x - 16) / 40;
      const int edge = std::min((x - 16) % 40, 39 - (x - 16) % 40);
      const int tone = edge < 4 ? 255 - 60 * edge : 0;
      if (y < 64)
        raster->pixels(y)[x] = TPixelCM32(band % 2 ? 3 : 1, 2, tone);
      else
        raster->pixels(y)[x] = TPixelCM32(0, band % 2 ? 4 : 2, 255);
    }
  image->setSavebox(TRect(16, 20, 175, 107));
  f.useTool("T_Eraser");
  f.setNumber("Hardness:", 100);
  f.setNumber("Size:", 12);
  f.setEnumValue("Type:", L"Normal");
  f.setEnumValue("Mode:", L"Lines & Areas");
  f.setBool("Selective", false);
  f.setBool("Invert", false);
  f.setBool("Frame Range", false);
  f.setBool("Pencil Mode", false);
  f.setStyle(1);
  f.tool()->onEnter();
}

void addToonzEraserCases(std::vector<Case>& cases) {
  static const wchar_t* const modes[] = {L"Lines", L"Areas", L"Lines & Areas"};
  for (int mode = 0; mode < 3; ++mode) {
    static const char* const hard[] = {"erase_toonz_hard_lines",
                                       "erase_toonz_hard_areas",
                                       "erase_toonz_hard_all"};
    cases.push_back({hard[mode], [mode](Fixture& f, Record& r) {
                       toonzEraser(f);
                       f.setEnumValue("Mode:", modes[mode]);
                       paint(f, r, "curve");
                     }});
    static const char* const soft[] = {"erase_toonz_soft_lines",
                                       "erase_toonz_soft_areas",
                                       "erase_toonz_soft_all"};
    cases.push_back({soft[mode], [mode](Fixture& f, Record& r) {
                       toonzEraser(f);
                       f.setNumber("Hardness:", 40);
                       f.setEnumValue("Mode:", modes[mode]);
                       paint(f, r, "curve");
                     }});
  }
  cases.push_back({"erase_toonz_pencil", [](Fixture& f, Record& r) {
                     toonzEraser(f);
                     f.setBool("Pencil Mode", true);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"erase_toonz_hard_selective", [](Fixture& f, Record& r) {
                     toonzEraser(f);
                     f.setBool("Selective", true);
                     f.setStyle(3);
                     paint(f, r, "across");
                   }});
  cases.push_back({"erase_toonz_soft_selective", [](Fixture& f, Record& r) {
                     toonzEraser(f);
                     f.setNumber("Hardness:", 40);
                     f.setBool("Selective", true);
                     f.setStyle(3);
                     paint(f, r, "across");
                   }});
  cases.push_back({"erase_toonz_rect", [](Fixture& f, Record& r) {
                     toonzEraser(f);
                     f.setEnumValue("Type:", L"Rectangular");
                     paint(f, r, "line");
                   }});
  cases.push_back({"erase_toonz_rect_invert_lines", [](Fixture& f, Record& r) {
                     toonzEraser(f);
                     f.setEnumValue("Type:", L"Rectangular");
                     f.setEnumValue("Mode:", L"Lines");
                     f.setBool("Invert", true);
                     paint(f, r, "line");
                   }});
  cases.push_back({"erase_toonz_freehand", [](Fixture& f, Record& r) {
                     toonzEraser(f);
                     f.setEnumValue("Type:", L"Freehand");
                     paint(f, r, "loop");
                   }});
}

//-----------------------------------------------------------------------------

// Opaque and half transparent colour over most of the drawing.
void fullColorEraser(Fixture& f) {
  f.newFullColorLevel();
  TRasterImageP image = f.image();
  TRaster32P raster   = image->getRaster();
  for (int y = 16; y < 112; ++y)
    for (int x = 12; x < 180; ++x)
      raster->pixels(y)[x] =
          x < 96 ? TPixel32(200, 30, 30) : TPixel32(15, 30, 100, 128);
  f.useTool("T_Eraser");
  f.setNumber("Hardness:", 100);
  f.setNumber("Size:", 12);
  f.setNumber("Opacity:", 100);
  f.setEnumValue("Type:", L"Normal");
  f.setBool("Invert", false);
  f.setBool("Frame Range", false);
  f.tool()->onEnter();
}

void addFullColorEraserCases(std::vector<Case>& cases) {
  cases.push_back({"erase_raster_hard", [](Fixture& f, Record& r) {
                     fullColorEraser(f);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"erase_raster_soft", [](Fixture& f, Record& r) {
                     fullColorEraser(f);
                     f.setNumber("Hardness:", 30);
                     f.setNumber("Size:", 20);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"erase_raster_opacity", [](Fixture& f, Record& r) {
                     fullColorEraser(f);
                     f.setNumber("Opacity:", 40);
                     paint(f, r, "across");
                   }});
  cases.push_back({"erase_raster_dot", [](Fixture& f, Record& r) {
                     fullColorEraser(f);
                     paint(f, r, "dot");
                   }});
  cases.push_back({"erase_raster_rect", [](Fixture& f, Record& r) {
                     fullColorEraser(f);
                     f.setEnumValue("Type:", L"Rectangular");
                     paint(f, r, "line");
                   }});
  cases.push_back({"erase_raster_rect_invert", [](Fixture& f, Record& r) {
                     fullColorEraser(f);
                     f.setEnumValue("Type:", L"Rectangular");
                     f.setBool("Invert", true);
                     paint(f, r, "line");
                   }});
  cases.push_back({"erase_raster_freehand", [](Fixture& f, Record& r) {
                     fullColorEraser(f);
                     f.setEnumValue("Type:", L"Freehand");
                     paint(f, r, "loop");
                   }});
}

}  // namespace

void addEraserCases(std::vector<Case>& cases) {
  addVectorEraserCases(cases);
  addToonzEraserCases(cases);
  addFullColorEraserCases(cases);
}

}  // namespace brushstroke
