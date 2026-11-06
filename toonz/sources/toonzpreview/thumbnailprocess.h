#pragma once

#include <windows.h>
#include <string>
#include "toonz/shellthumbnail.h"

namespace ThumbnailProcess {
struct Options {
  DWORD timeoutMs = ShellThumbnail::TimeoutMs;
  // The provider uses COM call cancellation. Tests may supply a deterministic
  // cancellation callback, without changing the installed timeout policy.
  bool (*isCanceled)() = nullptr;
};
std::wstring siblingRenderer(HMODULE providerModule);
HRESULT render(const std::wstring &executable, const std::wstring &input,
               UINT requestedSize, HBITMAP *bitmap,
               const Options &options = Options());
} // namespace ThumbnailProcess
