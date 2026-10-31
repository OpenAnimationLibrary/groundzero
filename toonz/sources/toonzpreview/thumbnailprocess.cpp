#include "thumbnailprocess.h"
#include <objbase.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace {
class Handle {
  HANDLE m_value;
public:
  explicit Handle(HANDLE value = nullptr) : m_value(value) {}
  ~Handle() { if (m_value && m_value != INVALID_HANDLE_VALUE) CloseHandle(m_value); }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  HANDLE get() const { return m_value; }
  bool valid() const { return m_value && m_value != INVALID_HANDLE_VALUE; }
};
std::wstring quoted(const std::wstring &value) {
  // Windows CommandLineToArgvW/CRT quoting, including trailing backslashes.
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (wchar_t c : value) {
    if (c == L'\\') { ++slashes; continue; }
    result.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0; result += c;
  }
  result.append(slashes * 2, L'\\'); result += L'\"';
  return result;
}
class RequestDirectory {
public:
  std::wstring path;
  bool create() {
    std::vector<wchar_t> buffer(32768);
    const DWORD count = GetTempPathW(DWORD(buffer.size()), buffer.data());
    if (!count || count >= buffer.size()) return false;
    GUID id; if (FAILED(CoCreateGuid(&id))) return false;
    wchar_t name[40];
    if (!StringFromGUID2(id, name, 40)) return false;
    path.assign(buffer.data(), count);
    path += L"groundzero-thumbnail-"; path += name;
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    path.clear(); return false;
  }
  ~RequestDirectory() {
    if (path.empty()) return;
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
      do {
        // Only this request's flat output files are removed. Never follow
        // directory junctions or recurse into renderer-created directories.
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
          DeleteFileW((path + L"\\" + entry.cFileName).c_str());
      } while (FindNextFileW(search, &entry));
      FindClose(search);
    }
    RemoveDirectoryW(path.c_str());
  }
};
HRESULT readBitmap(const std::wstring &fileName, unsigned requestedSize, HBITMAP *bitmap) {
  Handle file(CreateFileW(fileName.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file.valid()) return HRESULT_FROM_WIN32(GetLastError());
  LARGE_INTEGER length;
  if (!GetFileSizeEx(file.get(), &length)) return HRESULT_FROM_WIN32(GetLastError());
  std::array<unsigned char, ShellThumbnail::HeaderSize> header;
  DWORD read = 0;
  if (!ReadFile(file.get(), header.data(), DWORD(header.size()), &read, nullptr) || read != header.size())
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  unsigned width = 0, height = 0;
  if (!ShellThumbnail::dimensions(header.data(), width, height) ||
      width > requestedSize || height > requestedSize ||
      length.QuadPart != LONGLONG(header.size()) + LONGLONG(width) * height * 4)
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  const DWORD byteCount = width * height * 4;
  std::vector<unsigned char> pixels(byteCount);
  if (!ReadFile(file.get(), pixels.data(), byteCount, &read, nullptr) || read != byteCount)
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  // The Shell expects premultiplied color channels. Reject a corrupt payload
  // instead of handing Explorer a malformed or oversized bitmap.
  for (size_t i = 0; i < pixels.size(); i += 4)
    if (pixels[i] > pixels[i+3] || pixels[i+1] > pixels[i+3] || pixels[i+2] > pixels[i+3])
      return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = LONG(width); info.bmiHeader.biHeight = LONG(height);
  info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB; info.bmiHeader.biSizeImage = byteCount;
  void *bits = nullptr;
  HBITMAP result = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!result || !bits) {
    if (result) DeleteObject(result);
    return E_OUTOFMEMORY;
  }
  std::memcpy(bits, pixels.data(), byteCount); *bitmap = result;
  return S_OK;
}
bool canceled(const ThumbnailProcess::Options &options) {
  return options.isCanceled ? options.isCanceled() : CoTestCancel() == RPC_E_CALL_CANCELED;
}
}

std::wstring ThumbnailProcess::siblingRenderer(HMODULE module) {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(module, buffer.data(), DWORD(buffer.size()));
  if (!length || length >= buffer.size()) return {};
  std::wstring path(buffer.data(), length);
  const auto separator = path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) return {};
  return path.substr(0, separator + 1) + L"tconverter.exe";
}

HRESULT ThumbnailProcess::render(const std::wstring &executable,
    const std::wstring &input, UINT requestedSize, HBITMAP *bitmap,
    const Options &options) {
  if (!bitmap) return E_POINTER;
  *bitmap = nullptr;
  if (!requestedSize || executable.empty() || input.empty()) return E_INVALIDARG;
  if (canceled(options)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
  const unsigned side = std::min(requestedSize, ShellThumbnail::MaxSide);
  RequestDirectory temporary;
  if (!temporary.create()) return HRESULT_FROM_WIN32(ERROR_CANNOT_MAKE);
  const std::wstring output = temporary.path + L"\\thumbnail.bin";
  std::wstring command = quoted(executable) + L" --shell-thumbnail " + quoted(input) +
      L" " + std::to_wstring(side) + L" " + quoted(output);
  if (command.size() >= 32767) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
  Handle job(CreateJobObjectW(nullptr, nullptr));
  if (!job.valid()) return HRESULT_FROM_WIN32(GetLastError());
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    return HRESULT_FROM_WIN32(GetLastError());
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  // Explicit application path avoids PATH/current-directory executable search.
  // Start suspended so it cannot create descendants before job assignment.
  if (!CreateProcessW(executable.c_str(), &command[0], nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
                      &startup, &process)) return HRESULT_FROM_WIN32(GetLastError());
  Handle processHandle(process.hProcess), threadHandle(process.hThread);
  if (!AssignProcessToJobObject(job.get(), processHandle.get())) {
    const DWORD error = GetLastError();
    TerminateProcess(processHandle.get(), ERROR_CANCELLED);
    WaitForSingleObject(processHandle.get(), 2000);
    return HRESULT_FROM_WIN32(error);
  }
  if (ResumeThread(threadHandle.get()) == DWORD(-1)) {
    const DWORD error = GetLastError();
    TerminateJobObject(job.get(), ERROR_CANCELLED);
    WaitForSingleObject(processHandle.get(), 2000);
    return HRESULT_FROM_WIN32(error);
  }
  const ULONGLONG started = GetTickCount64();
  for (;;) {
    const DWORD wait = WaitForSingleObject(processHandle.get(), 50);
    if (wait == WAIT_OBJECT_0) break;
    const bool wasCanceled = canceled(options);
    const bool expired = GetTickCount64() - started >= options.timeoutMs;
    if (wait == WAIT_FAILED || wasCanceled || expired) {
      const DWORD error = wait == WAIT_FAILED ? GetLastError()
          : wasCanceled ? ERROR_CANCELLED : ERROR_TIMEOUT;
      TerminateJobObject(job.get(), error);
      WaitForSingleObject(processHandle.get(), 2000);
      return HRESULT_FROM_WIN32(error);
    }
  }
  DWORD exitCode = 0;
  if (canceled(options)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
  if (!GetExitCodeProcess(processHandle.get(), &exitCode) || exitCode != 0) return E_FAIL;
  return readBitmap(output, side, bitmap);
}
