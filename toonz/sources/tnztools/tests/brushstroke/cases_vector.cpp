#include "cases.h"

#include "tools/assistant.h"
#include "tools/strokeselection.h"
#include "tregion.h"
#include "tstroke.h"
#include "tundo.h"

namespace brushstroke {

void paint(Fixture& fixture, Record& record, const char* gesture,
           const Play& how) {
  const Record before = snapshot(fixture);
  TUndoManager::manager()->reset();
  fixture.play(gesture, how);
  recordChange(record, before, snapshot(fixture));
  recordUndo(record, fixture, before);
}

namespace {
TMetaObjectP metaObject(const char* type, const TPointD& a, const TPointD& b) {
  TMetaObjectP object(new TMetaObject(std::string(type)));
  TAssistantBase* handler = object->getHandler<TAssistantBase>();
  require(handler, std::string("meta object type is not registered: ") + type);
  handler->setDefaults();
  // Moving a carries b along, so a goes first.
  handler->movePoint(TStringId("a"), a);
  handler->movePoint(TStringId("b"), b);
  const TAssistantPoint* first  = handler->findPoint(TStringId("a"));
  const TAssistantPoint* second = handler->findPoint(TStringId("b"));
  require(first && second && first->position == a && second->position == b,
          std::string("could not place the points of ") + type);
  return object;
}
}  // namespace

TMetaObjectP mirrorReplicator() {
  return metaObject("replicatorMirror", TPointD(3, 0), TPointD(3, -50));
}

TMetaObjectP lineAssistant() {
  return metaObject("assistantLine", TPointD(-40, -10), TPointD(40, 10));
}

namespace {

// Every option of the vector Brush, so that no case depends on the one run
// before it.
void vectorBrush(Fixture& f) {
  f.newVectorLevel();
  f.useTool("T_Brush");
  f.setPair("Size", 2, 6);
  f.setNumber("Accuracy:", 20);
  f.setNumber("Smooth:", 0);
  f.setEnum("Draw Order", 0);
  f.setEnum("Paint:", 0);
  f.setEnum("Selection Mask:", 0);
  f.setBool("Break", false);
  f.setBool("Pressure", true);
  f.setNumber("Fluid Width:", 0);
  f.setBool("Snap", false);
  f.setEnum("Range:", 0);
  f.setEnum("Sensitivity:", 0);
  f.setBool("Assistants", true);
  f.setBool("Auto Close", false);
  f.setBool("Auto Group", false);
  f.setBool("Auto Fill", false);
  f.setEnum("Art Layer:", 0);
  f.setEnum("Cap", 1);
  f.setEnum("Join", 1);
  f.setNumber("Miter:", 4);
  f.setStyle(1);
}

TStroke* strokeThrough(const std::vector<TPointD>& points, double thickness,
                       int style) {
  std::vector<TThickPoint> thick;
  for (const TPointD& point : points) {
    if (!thick.empty())
      thick.push_back(
          TThickPoint((TPointD(thick.back()) + point) * 0.5, thickness));
    thick.push_back(TThickPoint(point, thickness));
  }
  TStroke* stroke = new TStroke(thick);
  stroke->setStyle(style);
  return stroke;
}

// Three crossing lines of styles 4, 1 and 2, bottom to top.
void addLines(Fixture& f) {
  TVectorImageP image = f.image();
  image->addStroke(strokeThrough({{-44, -20}, {44, 20}}, 1.5, 4));
  image->addStroke(strokeThrough({{-44, 20}, {44, -20}}, 1.5, 1));
  image->addStroke(strokeThrough({{-44, 0}, {44, 2}}, 1.5, 2));
  image->findRegions();
}

// A closed rectangle of style 1 filled with style 2.
void addFilledBox(Fixture& f) {
  TVectorImageP image = f.image();
  TStroke* box        = strokeThrough(
      {{-42, -22}, {42, -22}, {42, 22}, {-42, 22}, {-42, -22}}, 1.0, 1);
  box->setSelfLoop(true);
  image->addStroke(box);
  image->findRegions();
  TRegion* region = image->getRegion(TPointD(0, 0));
  require(region, "the fixture box did not make a region");
  region->setStyle(2);
}

void addInputCases(std::vector<Case>& cases) {
  cases.push_back({"vector_mouse", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     Play mouse;
                     mouse.tablet = false;
                     paint(f, r, "curve", mouse);
                   }});
  cases.push_back({"vector_pressure", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"vector_pressure_off", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.setBool("Pressure", false);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"vector_smooth", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.setNumber("Smooth:", 12);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"vector_accuracy_high", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.setNumber("Accuracy:", 100);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"vector_accuracy_low", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.setNumber("Accuracy:", 1);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"vector_zoom_in", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.viewer.pixelSize = 0.25;
                     paint(f, r, "curve");
                   }});
  cases.push_back({"vector_zoom_out", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.viewer.pixelSize = 4;
                     paint(f, r, "curve");
                   }});
  cases.push_back({"vector_dot", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     paint(f, r, "dot");
                   }});
  cases.push_back({"vector_shift_line", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     Play shift;
                     shift.shift = true;
                     paint(f, r, "line", shift);
                   }});
  cases.push_back({"vector_fluid_width", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.setNumber("Fluid Width:", 70);
                     paint(f, r, "speed");
                   }});
  cases.push_back({"vector_smooth_zoom_fluid", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.setNumber("Smooth:", 5);
                     f.setNumber("Fluid Width:", 40);
                     f.viewer.pixelSize = 0.5;
                     paint(f, r, "speed");
                   }});
}

void addStrokeOptionCases(std::vector<Case>& cases) {
  cases.push_back({"vector_break", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.setBool("Break", true);
                     f.setNumber("Accuracy:", 100);
                     paint(f, r, "zigzag");
                   }});
  cases.push_back({"vector_break_off", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.setNumber("Accuracy:", 100);
                     paint(f, r, "zigzag");
                   }});
  cases.push_back({"vector_cap_join", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.setEnum("Cap", 0);
                     f.setEnum("Join", 0);
                     f.setNumber("Miter:", 10);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"vector_colour_art", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.setEnum("Art Layer:", 2);
                     paint(f, r, "curve");
                     f.setEnum("Art Layer:", 0);
                   }});
  cases.push_back({"vector_snap_stroke", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     addLines(f);
                     f.setBool("Snap", true);
                     f.setEnum("Sensitivity:", 2);
                     Play shifted;
                     shifted.offset = TPointD(0, 1.5);
                     paint(f, r, "line", shifted);
                   }});
  cases.push_back({"vector_snap_self", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.setBool("Snap", true);
                     f.setEnum("Sensitivity:", 2);
                     paint(f, r, "loop");
                   }});
  cases.push_back({"vector_auto_close", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.setBool("Auto Close", true);
                     paint(f, r, "loop");
                   }});
  cases.push_back({"vector_auto_group", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.setBool("Auto Group", true);
                     f.setBool("Break", true);
                     f.setNumber("Accuracy:", 100);
                     paint(f, r, "zigzag");
                   }});
  cases.push_back({"vector_auto_fill", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.setStyle(2);
                     f.setBool("Auto Fill", true);
                     paint(f, r, "loop");
                   }});
}

void addOrderCases(std::vector<Case>& cases) {
  for (int order = 0; order < 3; ++order) {
    static const char* const names[] = {
        "vector_order_over", "vector_order_under", "vector_order_palette"};
    cases.push_back({names[order], [order](Fixture& f, Record& r) {
                       vectorBrush(f);
                       addLines(f);
                       // Style 4 is last in the palette: Palette Order puts
                       // it below the strokes of styles 1 and 2.
                       f.setStyle(4);
                       f.setEnum("Draw Order", order);
                       paint(f, r, "curve");
                     }});
  }
  // The same orders for a stroke with sharp corners, which takes another
  // branch of the insertion.
  for (int order = 1; order < 3; ++order) {
    static const char* const names[] = {"", "vector_order_under_corners",
                                        "vector_order_palette_corners"};
    cases.push_back({names[order], [order](Fixture& f, Record& r) {
                       vectorBrush(f);
                       addLines(f);
                       f.setStyle(4);
                       f.setEnum("Draw Order", order);
                       f.setNumber("Accuracy:", 100);
                       paint(f, r, "zigzag");
                     }});
  }
  cases.push_back({"vector_two_strokes", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     paint(f, r, "curve");
                     f.setStyle(2);
                     f.setEnum("Draw Order", 1);
                     paint(f, r, "across");
                   }});
}

void addPaintModeCases(std::vector<Case>& cases) {
  cases.push_back({"vector_paint_fills", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     addLines(f);
                     f.setStyle(2);
                     f.setEnum("Paint:", 1);
                     paint(f, r, "curve");
                   }});
  cases.push_back({"vector_paint_behind", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     addFilledBox(f);
                     f.setStyle(4);
                     f.setEnum("Paint:", 2);
                     paint(f, r, "across");
                   }});
  cases.push_back({"vector_paint_inside", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     addFilledBox(f);
                     f.setStyle(4);
                     f.setEnum("Paint:", 3);
                     // Starts inside the box and ends outside it.
                     Play wide;
                     wide.scale  = 1.3;
                     wide.offset = TPointD(14, 0);
                     paint(f, r, "curve", wide);
                   }});
  cases.push_back({"vector_paint_selection", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     addFilledBox(f);
                     // The Brush takes its mask from the Selection tool when
                     // that tool is left.
                     TTool* selector = f.useTool("T_Selection");
                     StrokeSelection* selection =
                         dynamic_cast<StrokeSelection*>(
                             selector->getSelection());
                     require(selection, "the Selection tool has no selection");
                     selection->selectNone();
                     selection->select(0, true);
                     selection->makeCurrent();
                     f.useTool("T_Brush");
                     f.setStyle(4);
                     f.setEnum("Paint:", 4);
                     Play wide;
                     wide.scale = 1.3;
                     paint(f, r, "curve", wide);
                   }});
}

void addAssistedCases(std::vector<Case>& cases) {
  cases.push_back({"vector_assistant_line", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.addMetaColumn({lineAssistant()});
                     paint(f, r, "line");
                   }});
  cases.push_back({"vector_assistants_off", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.addMetaColumn({lineAssistant(), mirrorReplicator()});
                     f.setBool("Assistants", false);
                     paint(f, r, "line");
                   }});
  cases.push_back({"vector_replicator_mirror", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.addMetaColumn({mirrorReplicator()});
                     paint(f, r, "curve");
                   }});
  cases.push_back({"vector_range_linear", [](Fixture& f, Record& r) {
                     vectorBrush(f);
                     f.addDrawing();
                     f.addDrawing();
                     f.setEnum("Range:", 1);
                     f.goToRow(0);
                     f.play("curve");
                     f.goToRow(2);
                     Play moved;
                     moved.offset = TPointD(0, 8);
                     moved.scale  = 0.8;
                     paint(f, r, "curve", moved);
                     f.setEnum("Range:", 0);
                   }});
}

}  // namespace

void addVectorBrushCases(std::vector<Case>& cases) {
  addInputCases(cases);
  addStrokeOptionCases(cases);
  addOrderCases(cases);
  addPaintModeCases(cases);
  addAssistedCases(cases);
}

}  // namespace brushstroke
