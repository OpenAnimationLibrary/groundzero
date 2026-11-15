#pragma once

class QMainWindow;

/** Exercises repeated native UI ownership and update-check completion.
 *
 * Args:
 *   window: Native application window using isolated portable settings.
 *
 * Throws:
 *   std::runtime_error: If retired UI objects or update checkers remain alive.
 */
void exerciseUiMemoryOwnership(QMainWindow* window);
