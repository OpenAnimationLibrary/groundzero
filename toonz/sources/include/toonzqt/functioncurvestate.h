#pragma once
#include "tdoubleparam.h"

// Editor locks deliberately leave evaluation and native undo restoration alone.
namespace FunctionCurveState {
inline bool isLocked(const TDoubleParam *curve) {
  return curve && curve->isEditorLocked();
}
}
