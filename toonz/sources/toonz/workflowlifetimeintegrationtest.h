#pragma once

class QMainWindow;

/** Exercises a pending workflow switch during real lazy room initialization.
 *
 * Args:
 *   window: Native application window using isolated fixture settings.
 *
 * Throws:
 *   std::runtime_error: If initialization destroys its room or loses the
 * request.
 */
void exerciseWorkflowRoomLifetime(QMainWindow* window);
