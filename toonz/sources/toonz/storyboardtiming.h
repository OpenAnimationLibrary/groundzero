#pragma once

#include <QString>
class TXsheet;

// Move [first,last] immediately before insertion. Preserve native segments when
// cuts are exact; allowBake opts into sampling otherwise unrepresentable curves.
// Nested xsheet contents keep their local time.
// All tracks, tags and original interpolation are restored by one undo.
// Empty error means an already-in-place request. Never partially applies.
bool moveStoryboardTiming(TXsheet *sheet, int first, int last, int insertion,
                          QString *error = nullptr, bool allowBake = false);
