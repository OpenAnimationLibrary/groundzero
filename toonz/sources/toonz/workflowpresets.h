#pragma once

// Workspace selection adapted from Ztoryc (BSD-3-Clause).
// See stuff/doc/Ztoryc-LICENSE.txt and doc/workflows.md for provenance.
#include <QtGlobal>

namespace WorkflowPresets {
struct Preset {
  const char *command;
  const char *profile;
  const char *label;
};

// Only shipped, non-storyboard workspaces. Scene roles and production data
// are deliberately independent of workspace selection in Groundzero.
inline constexpr Preset presets[] = {
    {"MI_Workflow2D", "Tradigital",
     QT_TRANSLATE_NOOP("MainWindow", "2D Tradigital")},
    {"MI_WorkflowCutout", "Cutout",
     QT_TRANSLATE_NOOP("MainWindow", "Cutout Digital")},
    {"MI_WorkflowStopMotion", "StopMotion",
     QT_TRANSLATE_NOOP("MainWindow", "Stop-Motion")},
    {"MI_WorkflowCharacter", "Character",
     QT_TRANSLATE_NOOP("MainWindow", "Characters")},
    {"MI_WorkflowDefault", "Default",
     QT_TRANSLATE_NOOP("MainWindow", "Default Rooms")},
    {"MI_WorkflowStudioGhibli", "StudioGhibli",
     QT_TRANSLATE_NOOP("MainWindow", "Studio Ghibli Edition")},
};
}  // namespace WorkflowPresets
