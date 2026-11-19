#pragma once

#ifndef BRUSHSTROKE_CASES_H
#define BRUSHSTROKE_CASES_H

#include "harness.h"

#include "tmetaimage.h"

#include <functional>

namespace brushstroke {

struct Case {
  const char* name;
  std::function<void(Fixture&, Record&)> run;
};

// Replays a gesture and appends what it did to the drawings of the level and
// to the undo history.
void paint(Fixture& fixture, Record& record, const char* gesture,
           const Play& how = Play());

// A mirror replicator across the vertical line x = 3, and a line assistant
// through (-40, -10) and (40, 10).
TMetaObjectP mirrorReplicator();
TMetaObjectP lineAssistant();

void addVectorBrushCases(std::vector<Case>& cases);
void addToonzRasterBrushCases(std::vector<Case>& cases);
void addFullColorBrushCases(std::vector<Case>& cases);
void addEraserCases(std::vector<Case>& cases);

}  // namespace brushstroke

#endif
