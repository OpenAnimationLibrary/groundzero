#pragma once

// A batch render must never wait for a window-system dialog. On macOS the
// window system asks, once a process has ended abnormally, whether to reopen
// its windows; a render with no window can never answer.
//
// Registers, for this process only and without changing anything the
// application stores, that no window state is restored.
void ignoreWindowStateRestoration();
