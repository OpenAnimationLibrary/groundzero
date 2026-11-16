#pragma once

class TXshSimpleLevel;

namespace GroundzeroAutomation {

// Rejects drawings and palette dependencies outside the admitted persistence
// profile.
void validateDrawingProfile(TXshSimpleLevel* level);
}  // namespace GroundzeroAutomation
