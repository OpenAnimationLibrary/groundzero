// toonzpreview.cpp
#include "toonzpreview.h"
#include "thumbnailprocess.h"

#include <shlwapi.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <codecvt>
#include <locale>

#pragma comment(lib, "shlwapi.lib")
// Ensure we link against the standard UUIDs
#pragma comment(lib, "uuid.lib")

// Global Variables
long g_cDllRef        = 0;
HINSTANCE g_hInst     = NULL;
const wchar_t *exts[] = {L".tnz", L".tlv", L".pli", L".tga"};
const wchar_t *progId = L"OpenToonz.ToonzPreview";
const int EXT_COUNT   = 4;

// CONSTANTS FOR REGISTRY
// This is the Standard Windows Interface ID for IThumbnailProvider.
// DO NOT CHANGE THIS. It must match system expectations.
const wchar_t *SZ_ITHUMBNAILPROVIDER_IID =
    L"{e357fccd-a995-4576-b01f-234630154e96}";

#ifdef TOONZPREVIW_DEBUG
void DebugLog(const std::string &message) {
  std::string fullMessage = "[ToonzPreview] " + message + "\n";
  OutputDebugStringA(fullMessage.c_str());

  WCHAR dllPath[MAX_PATH];
  if (GetModuleFileNameW(g_hInst, dllPath, MAX_PATH)) {
    WCHAR logFilePath[MAX_PATH];
    wcscpy_s(logFilePath, MAX_PATH, dllPath);
    PathRemoveFileSpecW(logFilePath);
    wcscat_s(logFilePath, MAX_PATH, L"\\toonz_preview.log");

    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    std::string narrowLogPath = converter.to_bytes(logFilePath);

    std::ofstream logfile(narrowLogPath, std::ios::app);
    if (logfile.is_open()) {
      logfile << "[" << GetCurrentProcessId() << ":" << GetCurrentThreadId()
              << "] " << message << std::endl;
    }
  }
}

#else
void DebugLog(const std::string &message) {
  // No operation in release builds
}
#endif  // TOONZPREVIW_DEBUG

std::wstring getPaletteIconPath() {
  // Match TEnv's Windows configuration contract without loading Qt/Toonz
  // into Explorer merely to register the palette's static icon.
  const auto renderer = ThumbnailProcess::siblingRenderer(g_hInst);
  const auto separator = renderer.find_last_of(L"\\/");
  const std::wstring portable = separator == std::wstring::npos ? L""
      : renderer.substr(0, separator + 1) + L"portablestuff\\config\\icon\\paletteIcon.ico";
  if (!portable.empty() && GetFileAttributesW(portable.c_str()) != INVALID_FILE_ATTRIBUTES)
    return portable;
  auto systemValue = [](const wchar_t *name) {
    std::vector<wchar_t> value(32768);
    DWORD size = DWORD(value.size() * sizeof(wchar_t));
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\OpenToonz\\OpenToonz", name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
        nullptr, value.data(), &size);
    return status == ERROR_SUCCESS ? std::wstring(value.data()) : std::wstring();
  };
  std::wstring config = systemValue(L"TOONZCONFIG");
  if (config.empty()) {
    config = systemValue(L"TOONZROOT");
    if (!config.empty()) config += L"\\config";
  }
  return config.empty() ? std::wstring() : config + L"\\icon\\paletteIcon.ico";
}

std::string WideToNarrow(const std::wstring &wideStr) {
  if (wideStr.empty()) return "";
  int size_needed = WideCharToMultiByte(
      CP_UTF8, 0, wideStr.c_str(), (int)wideStr.size(), NULL, 0, NULL, NULL);
  if (size_needed == 0) return "";
  std::string narrowStr(size_needed, 0);
  WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), (int)wideStr.size(),
                      &narrowStr[0], size_needed, NULL, NULL);
  return narrowStr;
}

// ---------------------------------------------------------------------------
// Process Isolation Implementation
// ---------------------------------------------------------------------------

bool SetDisableProcessIsolation() {
  std::wstring keyPath = L"CLSID\\";
  keyPath += SZ_CLSID_TOONZPREVIEW;
  // Correction: Do not append "\\InprocServer32".
  // DisableProcessIsolation is a direct property of CLSID, not the DLL.

  HKEY hKey = NULL;
  // Attempt to open the CLSID\{GUID} key
  LSTATUS status = RegOpenKeyExW(HKEY_CLASSES_ROOT, keyPath.c_str(), 0,
                                 KEY_SET_VALUE, &hKey);

  if (status != ERROR_SUCCESS) {
    DebugLog("Failed to open CLSID key for DisableProcessIsolation.");
    return false;
  }

  DWORD dwData = 1;
  // Set the DisableProcessIsolation key value (DWORD = 1)
  status = RegSetValueExW(hKey, L"DisableProcessIsolation", 0, REG_DWORD,
                          (const BYTE *)&dwData, sizeof(dwData));

  RegCloseKey(hKey);

  if (status != ERROR_SUCCESS) {
    DebugLog("Failed to set DisableProcessIsolation value.");
    return false;
  }

  DebugLog("Successfully set DisableProcessIsolation=1.");
  return true;
}

/**
 * @brief Clears the DisableProcessIsolation value from the registry.
 */
bool ClearDisableProcessIsolation() {
  std::wstring keyPath = L"CLSID\\";
  keyPath += SZ_CLSID_TOONZPREVIEW;
  // Correction: Do not append "\\InprocServer32".

  HKEY hKey      = NULL;
  LSTATUS status = RegOpenKeyExW(HKEY_CLASSES_ROOT, keyPath.c_str(), 0,
                                 KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey);

  if (status != ERROR_SUCCESS) {
    // If the key itself doesn't exist, no cleanup is needed, return true
    return true;
  }

  // Delete the DisableProcessIsolation value
  status = RegDeleteValueW(hKey, L"DisableProcessIsolation");

  RegCloseKey(hKey);

  // ERROR_FILE_NOT_FOUND means the value did not exist in the first place,
  // which is also a success
  if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
    DebugLog("Failed to delete DisableProcessIsolation value.");
    return false;
  }

  DebugLog("Successfully cleared DisableProcessIsolation.");
  return true;
}

// ---------------------------------------------------------------------------
// DLL Entry Point
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  switch (ul_reason_for_call) {
  case DLL_PROCESS_ATTACH: {
    g_hInst = hModule;

    DisableThreadLibraryCalls(hModule);
    DebugLog("DLL_PROCESS_ATTACH - ToonzPreview loaded");
    break;
  }
  case DLL_PROCESS_DETACH:

    DebugLog("DLL_PROCESS_DETACH");
    break;
  }
  return TRUE;
}

// ---------------------------------------------------------------------------
// DLL Exports
// ---------------------------------------------------------------------------
STDAPI DllCanUnloadNow() { return (g_cDllRef > 0) ? S_FALSE : S_OK; }

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv) {
  DebugLog("DllGetClassObject called");
  if (!IsEqualCLSID(rclsid, CLSID_ToonzThumbnailProvider)) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  CClassFactory *pFactory = new CClassFactory();
  if (!pFactory) return E_OUTOFMEMORY;

  HRESULT hr = pFactory->QueryInterface(riid, ppv);
  pFactory->Release();
  return hr;
}

HRESULT RegisterRegistryKeys() {
  WCHAR modulePath[MAX_PATH];
  if (!GetModuleFileNameW(g_hInst, modulePath, MAX_PATH)) return E_FAIL;

  // Key: HKLM\SOFTWARE\Classes\CLSID\{SZ_CLSID_TOONZPREVIEW}
  std::wstring clsidKey =
      std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + SZ_CLSID_TOONZPREVIEW;
  HKEY hKey;
  LONG lResult =
      RegCreateKeyExW(HKEY_LOCAL_MACHINE, clsidKey.c_str(), 0, NULL,
                      REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
  if (lResult == ERROR_SUCCESS) {
    RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE *)L"Toonz Thumbnail Provider",
                   (wcslen(L"Toonz Thumbnail Provider") + 1) * sizeof(WCHAR));

    // AppID
    RegSetValueExW(hKey, L"AppID", 0, REG_SZ, (BYTE *)SZ_CLSID_TOONZPREVIEW,
                   (wcslen(SZ_CLSID_TOONZPREVIEW) + 1) * sizeof(WCHAR));

    HKEY hInProc;
    if (RegCreateKeyExW(hKey, L"InProcServer32", 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hInProc,
                        NULL) == ERROR_SUCCESS) {
      RegSetValueExW(hInProc, NULL, 0, REG_SZ, (BYTE *)modulePath,
                     (wcslen(modulePath) + 1) * sizeof(WCHAR));
      RegSetValueExW(hInProc, L"ThreadingModel", 0, REG_SZ,
                     (BYTE *)L"Apartment",
                     (wcslen(L"Apartment") + 1) * sizeof(WCHAR));
      RegCloseKey(hInProc);
    }
    RegCloseKey(hKey);
  }

  // Key: HKLM\SOFTWARE\Classes\{progId}
  std::wstring typeKey = std::wstring(L"SOFTWARE\\Classes\\") + progId;
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, typeKey.c_str(), 0, NULL,
                      REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey,
                      NULL) == ERROR_SUCCESS) {
    RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE *)progId,
                   (wcslen(progId) + 1) * sizeof(WCHAR));
    RegCloseKey(hKey);

    bool installTGA = true;
    for (int i = 0; i < EXT_COUNT; i++) {
      if (wcscmp(exts[i], L".tga") == 0) {
        int result = MessageBoxW(
            NULL,
            L"Do you want to install thumbnail handler for .tga file type?",
            L"Confirm Installation", MB_YESNO | MB_ICONQUESTION);

        if (result != IDYES) {
          installTGA = false;
          DebugLog("User canceled TGA file type registration");
          continue;
        }
      }
    }

    // Key: HKLM\SOFTWARE\Classes\{progId}\ShellEx\{SZ_ITHUMBNAILPROVIDER_IID}
    std::wstring shellExKey = std::wstring(L"SOFTWARE\\Classes\\") + progId +
                              L"\\ShellEx\\" + SZ_ITHUMBNAILPROVIDER_IID;

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, shellExKey.c_str(), 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey,
                        NULL) == ERROR_SUCCESS) {
      RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE *)SZ_CLSID_TOONZPREVIEW,
                     (wcslen(SZ_CLSID_TOONZPREVIEW) + 1) * sizeof(WCHAR));
      RegCloseKey(hKey);
      DebugLog("Registered thumbnail handler for " +
               WideToNarrow(exts[EXT_COUNT - 1]));
    }

    for (int i = 0; i < EXT_COUNT; i++) {
      if (wcscmp(exts[i], L".tga") == 0 && !installTGA) {
        continue;
      }

      // Key:
      // HKLM\SOFTWARE\Classes\SystemFileAssociations\{exts[i]}\ShellEx\{SZ_ITHUMBNAILPROVIDER_IID}
      std::wstring sysKeyPath =
          std::wstring(L"SOFTWARE\\Classes\\SystemFileAssociations\\") +
          exts[i] + L"\\ShellEx\\" + SZ_ITHUMBNAILPROVIDER_IID;

      if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, sysKeyPath.c_str(), 0, NULL,
                          REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey,
                          NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE *)SZ_CLSID_TOONZPREVIEW,
                       (wcslen(SZ_CLSID_TOONZPREVIEW) + 1) * sizeof(WCHAR));
        RegCloseKey(hKey);
        DebugLog("Registered SystemFileAssociations for " +
                 WideToNarrow(exts[i]));
      }
    }
  }

  // Key: HKLM\SOFTWARE\Classes\.tpl\DefaultIcon
  std::wstring tplIconKey = L"SOFTWARE\\Classes\\.tpl\\DefaultIcon";
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, tplIconKey.c_str(), 0, NULL,
                      REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey,
                      NULL) == ERROR_SUCCESS) {
    const std::wstring iconPathW = getPaletteIconPath();

    if (!iconPathW.empty())
      RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE *)iconPathW.c_str(),
                     (iconPathW.length() + 1) * sizeof(WCHAR));

    RegCloseKey(hKey);
    DebugLog("Registered DefaultIcon for .tpl: " + WideToNarrow(iconPathW));
  }

  return S_OK;
}

STDAPI DllRegisterServer() {
  DebugLog("DllRegisterServer called");
  HRESULT hr = RegisterRegistryKeys();

  // Critical Step: After successful COM registration, set to disable process
  // isolation
  if (SUCCEEDED(hr)) {
    if (!SetDisableProcessIsolation()) {
      DebugLog(
          "WARNING: Failed to set DisableProcessIsolation=1. "
          "IInitializeWithFile may not be called.");
      // Do not return E_FAIL, allow registration to continue but log a warning
    }
  }

  SHChangeNotify(SHCNE_UPDATEIMAGE, SHCNF_DWORD | SHCNF_FLUSH, NULL, NULL);
  system("ie4uinit.exe -show");
  return hr;
}

STDAPI DllUnregisterServer() {
  DebugLog("DllUnregisterServer called");

  ClearDisableProcessIsolation();

  std::wstring clsidKey =
      std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + SZ_CLSID_TOONZPREVIEW;
  HKEY hTestKey;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, clsidKey.c_str(), 0, KEY_READ,
                    &hTestKey) == ERROR_SUCCESS) {
    RegCloseKey(hTestKey);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidKey.c_str());
  }

  std::wstring shellExKey = std::wstring(L"SOFTWARE\\Classes\\") + progId +
                            L"\\ShellEx\\" + SZ_ITHUMBNAILPROVIDER_IID;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, shellExKey.c_str(), 0, KEY_READ,
                    &hTestKey) == ERROR_SUCCESS) {
    RegCloseKey(hTestKey);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, shellExKey.c_str());
  }

  std::wstring progIDPath = L"SOFTWARE\\Classes\\" + std::wstring(progId);
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, progIDPath.c_str(), 0, KEY_READ,
                    &hTestKey) == ERROR_SUCCESS) {
    RegCloseKey(hTestKey);
    WCHAR existingValue[100];
    DWORD dataSize = sizeof(existingValue);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, progIDPath.c_str(), NULL,
                     RRF_RT_REG_SZ, NULL, existingValue,
                     &dataSize) == ERROR_SUCCESS) {
      if (wcscmp(existingValue, progId) == 0) {
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, progIDPath.c_str());
      }
    }
  }

  for (int i = 0; i < EXT_COUNT; i++) {
    std::wstring sysKeyPath =
        std::wstring(L"SOFTWARE\\Classes\\SystemFileAssociations\\") + exts[i] +
        L"\\ShellEx\\" + SZ_ITHUMBNAILPROVIDER_IID;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sysKeyPath.c_str(), 0, KEY_READ,
                      &hTestKey) == ERROR_SUCCESS) {
      RegCloseKey(hTestKey);
      WCHAR existingClsid[100];
      DWORD dataSize = sizeof(existingClsid);
      if (RegGetValueW(HKEY_LOCAL_MACHINE, sysKeyPath.c_str(), NULL,
                       RRF_RT_REG_SZ, NULL, existingClsid,
                       &dataSize) == ERROR_SUCCESS) {
        if (wcscmp(existingClsid, SZ_CLSID_TOONZPREVIEW) == 0) {
          RegDeleteKeyW(HKEY_LOCAL_MACHINE, sysKeyPath.c_str());
        }
      }
    }

    std::wstring extPath = L"SOFTWARE\\Classes\\" + std::wstring(exts[i]);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, extPath.c_str(), 0, KEY_READ,
                      &hTestKey) == ERROR_SUCCESS) {
      RegCloseKey(hTestKey);
      WCHAR existingValue[100];
      DWORD dataSize = sizeof(existingValue);
      if (RegGetValueW(HKEY_LOCAL_MACHINE, extPath.c_str(), NULL, RRF_RT_REG_SZ,
                       NULL, existingValue, &dataSize) == ERROR_SUCCESS) {
        if (wcscmp(existingValue, progId) == 0) {
          RegDeleteKeyW(HKEY_LOCAL_MACHINE, extPath.c_str());
        }
      }
    }
  }

  std::wstring tplIconKey = L"SOFTWARE\\Classes\\.tpl\\DefaultIcon";
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, tplIconKey.c_str(), 0, KEY_READ,
                    &hTestKey) == ERROR_SUCCESS) {
    RegCloseKey(hTestKey);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, tplIconKey.c_str());
  }

  SHChangeNotify(SHCNE_UPDATEIMAGE, SHCNF_DWORD | SHCNF_FLUSH, NULL, NULL);
  system("ie4uinit.exe -show");

  return S_OK;
}

// ---------------------------------------------------------------------------
// CToonzThumbnailProvider Implementation
// ---------------------------------------------------------------------------
CToonzThumbnailProvider::CToonzThumbnailProvider() : m_cRef(1) {
  InterlockedIncrement(&g_cDllRef);
  DebugLog("CToonzThumbnailProvider constructor called");
}

CToonzThumbnailProvider::~CToonzThumbnailProvider() {
  InterlockedDecrement(&g_cDllRef);
  DebugLog("CToonzThumbnailProvider destructor called");
}

IFACEMETHODIMP CToonzThumbnailProvider::QueryInterface(REFIID riid,
                                                       void **ppv) {
  if (!ppv) return E_INVALIDARG;
  *ppv = NULL;

  DebugLog("=== QueryInterface Start ===");

  LPOLESTR riidStr = NULL;
  StringFromIID(riid, &riidStr);
  std::wstring wriid(riidStr);
  DebugLog("Requested IID: " + WideToNarrow(wriid));
  CoTaskMemFree(riidStr);

  // Completely manual interface query
  if (IsEqualIID(riid, IID_IUnknown) ||
      IsEqualIID(riid, IID_IThumbnailProvider) ||
      IsEqualIID(riid, IID_IInitializeWithFile) ||
      IsEqualIID(riid, IID_IPersistFile) || IsEqualIID(riid, IID_IPersist)) {
    //|| IsEqualIID(riid, IID_IInitializeWithStream)) {
    // Return the correct pointer based on the requested interface
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_IThumbnailProvider)) {
      DebugLog("Returning IThumbnailProvider interface");
      *ppv = static_cast<IThumbnailProvider *>(this);
    } else if (IsEqualIID(riid, IID_IInitializeWithFile)) {
      DebugLog("Returning IInitializeWithFile interface");
      *ppv = static_cast<IInitializeWithFile *>(this);
    } else if (IsEqualIID(riid, IID_IPersistFile)) {
      DebugLog("Returning IPersistFile interface");
      *ppv = static_cast<IPersistFile *>(this);
    } else if (IsEqualIID(riid, IID_IPersist)) {
      DebugLog("Returning IPersist interface");
      *ppv = static_cast<IPersist *>(this);
    } else if (IsEqualIID(riid, IID_IInitializeWithStream)) {
      DebugLog("Returning IInitializeWithStream interface");
      *ppv = static_cast<IInitializeWithStream *>(this);
    }

    if (*ppv) {
      AddRef();
      DebugLog("QueryInterface SUCCEEDED");
      DebugLog("=== QueryInterface End ===");
      return S_OK;
    }
  }

  DebugLog("Interface not supported");
  DebugLog("=== QueryInterface End ===");
  return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) CToonzThumbnailProvider::AddRef() {
  return InterlockedIncrement(&m_cRef);
}

IFACEMETHODIMP_(ULONG) CToonzThumbnailProvider::Release() {
  ULONG cRef = InterlockedDecrement(&m_cRef);
  if (0 == cRef) delete this;
  return cRef;
}

IFACEMETHODIMP CToonzThumbnailProvider::Initialize(LPCWSTR pszFilePath,
                                                   DWORD grfMode) {
  DebugLog("IInitializeWithFile::Initialize called");

  if (!pszFilePath) {
    DebugLog("IInitializeWithFile::Initialize: pszFilePath is NULL");
    return E_INVALIDARG;
  }

  m_filePath = pszFilePath;
  DebugLog("IInitializeWithFile::Initialize: File path set to: " +
           WideToNarrow(m_filePath));
  DebugLog("IInitializeWithFile::Initialize: Mode: " + std::to_string(grfMode));

  return S_OK;
}

/**
 * @brief Attempts to extract the full path of the file from an IStream object.
 * * This is a standard method to get the source file path in
 * IInitializeWithStream::Initialize. It first tries IStream::Stat, then
 * attempts the IPropertyStore interface.
 *
 * @param pStream The input IStream pointer.
 * @return The extracted full file path; an empty string if unsuccessful.
 */
std::wstring GetFullPathFromStream(IStream *pStream) {
  if (!pStream) {
    DebugLog("GetFullPathFromStream: pStream is NULL.");
    return L"";
  }

  std::wstring filePath;

  // --- Method 1: Try to get path from STATSTG ---
  STATSTG stat = {0};
  HRESULT hr   = pStream->Stat(&stat, STATFLAG_DEFAULT);

  if (SUCCEEDED(hr) && stat.pwcsName) {
    filePath = stat.pwcsName;
    DebugLog("Method 1: Got name from STATSTG: " + WideToNarrow(filePath));

    // Check if it's a full path (contains drive letter ':' or starts with root
    // '\')
    bool isFullPath = (filePath.find(L':') != std::wstring::npos) ||
                      (filePath.find(L'\\') == 0);

    // If it's a full path, success, free memory and return
    if (isFullPath) {
      DebugLog("STATSTG path appears complete.");
      CoTaskMemFree(stat.pwcsName);
      return filePath;
    }

    // If it's not a full path (only filename), proceed to Method 2
    DebugLog("STATSTG path is incomplete, attempting Method 2...");
    CoTaskMemFree(stat.pwcsName);  // Free memory allocated for stat.pwcsName
  } else {
    DebugLog(
        "Method 1: Could not get STATSTG information or pwcsName is NULL.");
  }

  // --- Method 2: Try to get PKEY_ItemPathDisplay via IPropertyStore ---
  IPropertyStore *pPropertyStore = nullptr;
  HRESULT propHr = pStream->QueryInterface(IID_PPV_ARGS(&pPropertyStore));

  if (SUCCEEDED(propHr) && pPropertyStore) {
    PROPVARIANT propVar;
    PropVariantInit(&propVar);

    // Get the full file path property
    propHr = pPropertyStore->GetValue(PKEY_ItemPathDisplay, &propVar);

    if (SUCCEEDED(propHr) && propVar.vt == VT_LPWSTR && propVar.pwszVal) {
      filePath = propVar.pwszVal;
      DebugLog("Method 2: Got full path from IPropertyStore: " +
               WideToNarrow(filePath));
    } else {
      DebugLog(
          "Method 2: Failed to get PKEY_ItemPathDisplay or property type is "
          "wrong.");
    }

    PropVariantClear(&propVar);
    pPropertyStore->Release();
  } else {
    DebugLog("GetFullPathFromStream: Failed to query IPropertyStore.");
  }

  return filePath;
}

IFACEMETHODIMP CToonzThumbnailProvider::Initialize(IStream *pStream,
                                                   DWORD grfMode) {
  DebugLog("IInitializeWithStream::Initialize called");

  if (!pStream) {
    DebugLog("IInitializeWithStream::Initialize: pStream is NULL");
    return E_INVALIDARG;
  }

  // GetFullPathFromStream owns and frees its STATSTG name allocation.
  m_filePath = GetFullPathFromStream(pStream);

  // If still no full path, log a warning
  if (m_filePath.find(L':') == std::wstring::npos &&
      m_filePath.find(L'\\') != 0) {
    DebugLog(
        "IInitializeWithStream::Initialize: WARNING - Only have filename, "
        "palette files may not be found: " +
        WideToNarrow(m_filePath));
  }

  DebugLog("IInitializeWithStream::Initialize: Mode: " +
           std::to_string(grfMode));
  return S_OK;
}
IFACEMETHODIMP CToonzThumbnailProvider::GetThumbnail(UINT cx, HBITMAP *phbmp,
                                                     WTS_ALPHATYPE *pdwAlpha) {
  if (!phbmp || !pdwAlpha) return E_INVALIDARG;
  *phbmp = nullptr; *pdwAlpha = WTSAT_UNKNOWN;
  if (!cx || m_filePath.empty()) return E_INVALIDARG;
  try {
    std::wstring type = PathFindExtensionW(m_filePath.c_str());
    std::transform(type.begin(), type.end(), type.begin(),
                   [](wchar_t c) { return wchar_t(std::towlower(c)); });
    if (type != L".pli" && type != L".tnz" && type != L".tlv" && type != L".tga") return E_FAIL;
    // Explorer/COM worker threads never create a Qt GUI or own an OpenGL
    // context. The installed helper renders once and exits; the provider
    // validates the bounded bitmap payload before exposing it to the Shell.
    const HRESULT result = ThumbnailProcess::render(
        ThumbnailProcess::siblingRenderer(g_hInst), m_filePath, cx, phbmp);
    if (SUCCEEDED(result)) *pdwAlpha = WTSAT_ARGB;
    return result;
  } catch (...) {
    return E_FAIL;
  }
}

IFACEMETHODIMP CToonzThumbnailProvider::GetClassID(CLSID *pClassID) {
  DebugLog("IPersist::GetClassID called");
  if (!pClassID) return E_INVALIDARG;
  *pClassID = CLSID_ToonzThumbnailProvider;
  return S_OK;
}

// IPersistFile
IFACEMETHODIMP CToonzThumbnailProvider::IsDirty() { return S_FALSE; }
IFACEMETHODIMP CToonzThumbnailProvider::Save(LPCWSTR pszFileName,
                                             BOOL fRemember) {
  return E_NOTIMPL;
}
IFACEMETHODIMP CToonzThumbnailProvider::SaveCompleted(LPCWSTR pszFileName) {
  return E_NOTIMPL;
}
IFACEMETHODIMP CToonzThumbnailProvider::GetCurFile(LPWSTR *ppszFileName) {
  return E_NOTIMPL;
}

IFACEMETHODIMP CToonzThumbnailProvider::Load(LPCWSTR pszFileName,
                                             DWORD dwMode) {
  DebugLog("IPersistFile::Load called");

  if (!pszFileName) {
    DebugLog("IPersistFile::Load: pszFileName is NULL");
    return E_INVALIDARG;
  }

  m_filePath = pszFileName;
  DebugLog("IPersistFile::Load: File path set to: " + WideToNarrow(m_filePath));
  DebugLog("IPersistFile::Load: Mode: " + std::to_string(dwMode));

  return S_OK;
}

// ---------------------------------------------------------------------------
// CClassFactory Implementation
// ---------------------------------------------------------------------------
CClassFactory::CClassFactory() : m_cRef(1) { InterlockedIncrement(&g_cDllRef); }

CClassFactory::~CClassFactory() { InterlockedDecrement(&g_cDllRef); }

IFACEMETHODIMP CClassFactory::QueryInterface(REFIID riid, void **ppv) {
  if (!ppv) return E_INVALIDARG;
  *ppv = NULL;

  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
    *ppv = static_cast<IClassFactory *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) CClassFactory::AddRef() {
  return InterlockedIncrement(&m_cRef);
}

IFACEMETHODIMP_(ULONG) CClassFactory::Release() {
  ULONG cRef = InterlockedDecrement(&m_cRef);
  if (0 == cRef) delete this;
  return cRef;
}

IFACEMETHODIMP CClassFactory::CreateInstance(IUnknown *pUnkOuter, REFIID riid,
                                             void **ppv) {
  if (pUnkOuter) return CLASS_E_NOAGGREGATION;

  CToonzThumbnailProvider *pObj = new CToonzThumbnailProvider();
  if (!pObj) return E_OUTOFMEMORY;

  HRESULT hr = pObj->QueryInterface(riid, ppv);
  pObj->Release();
  return hr;
}

IFACEMETHODIMP CClassFactory::LockServer(BOOL fLock) {
  if (fLock)
    InterlockedIncrement(&g_cDllRef);
  else
    InterlockedDecrement(&g_cDllRef);
  return S_OK;
}

STDAPI DllInstall(BOOL bInstall, LPCWSTR pszCmdLine) {
  if (bInstall) {
    DllRegisterServer();
  } else {
    DllUnregisterServer();
  }
  return S_OK;
}
