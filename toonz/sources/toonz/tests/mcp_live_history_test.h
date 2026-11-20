#pragma once

// Run inside the native live fixture after it prepares its camera key and
// Blur. Leaves edits in the scene and history; the caller resets both.
void runNativeLiveHistoryTests();
