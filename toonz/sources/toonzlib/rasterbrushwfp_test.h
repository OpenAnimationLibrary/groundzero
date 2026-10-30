#pragma once
#if defined(_WIN32) && defined(RASTERBRUSH_SANDBOX_TESTING)
#include <fwpmu.h>
#include <cstring>

namespace RasterBrush {
namespace {
// Read-only CI evidence collector. It never adds filters, changes collection
// options, or grants capabilities. Unavailable evidence cannot pass a probe.
class BrushWfpDropEvidence {
  HMODULE module                     = nullptr;
  HANDLE engine                      = nullptr;
  FWP_BYTE_BLOB *application         = nullptr;
  FWP_BYTE_BLOB *listenerApplication = nullptr;
  FILETIME start{};
#define BRUSH_WFP_FUNCTION(name) decltype(&name) p##name = nullptr
  BRUSH_WFP_FUNCTION(FwpmEngineOpen0);
  BRUSH_WFP_FUNCTION(FwpmEngineClose0);
  BRUSH_WFP_FUNCTION(FwpmGetAppIdFromFileName0);
  BRUSH_WFP_FUNCTION(FwpmNetEventCreateEnumHandle0);
  BRUSH_WFP_FUNCTION(FwpmNetEventDestroyEnumHandle0);
  BRUSH_WFP_FUNCTION(FwpmNetEventEnum2);
  BRUSH_WFP_FUNCTION(FwpmFreeMemory0);
#undef BRUSH_WFP_FUNCTION

public:
  static bool matches(const FWPM_NET_EVENT2 &event,
                      const FWP_BYTE_BLOB *application, PSID package,
                      quint16 port, const FILETIME &start) {
    const auto &header        = event.header;
    constexpr UINT32 required = FWPM_NET_EVENT_FLAG_IP_VERSION_SET |
                                FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET |
                                FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET |
                                FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET |
                                FWPM_NET_EVENT_FLAG_APP_ID_SET |
                                FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET;
    if ((header.flags & required) != required ||
        header.ipVersion != FWP_IP_VERSION_V4 ||
        header.ipProtocol != IPPROTO_TCP ||
        header.remoteAddrV4 != INADDR_LOOPBACK || header.remotePort != port ||
        header.appId.size != application->size || !header.appId.data ||
        std::memcmp(header.appId.data, application->data, application->size) !=
            0 ||
        !header.packageSid || !IsValidSid(header.packageSid) ||
        !EqualSid(header.packageSid, package) ||
        CompareFileTime(&header.timeStamp, &start) < 0)
      return false;
    const bool classify = event.type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP &&
                          event.classifyDrop && event.classifyDrop->isLoopback;
    const bool capability = event.type == FWPM_NET_EVENT_TYPE_CAPABILITY_DROP &&
                            event.capabilityDrop &&
                            event.capabilityDrop->isLoopback;
    if (!classify && !capability) return false;
    return true;
  }
  // Reply drops carry the listener's identity, not the remote container's.
  // The caller must separately verify the launched child's exact container SID
  // and obtain its source port from that controlled probe's getsockname result.
  static bool matchesReply(const FWPM_NET_EVENT2 &event,
                           const FWP_BYTE_BLOB *listener, quint16 serverPort,
                           quint16 childPort, const FILETIME &start,
                           const FILETIME &end) {
    const auto &h             = event.header;
    constexpr UINT32 required = FWPM_NET_EVENT_FLAG_IP_VERSION_SET |
                                FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET |
                                FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET |
                                FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET |
                                FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET |
                                FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET |
                                FWPM_NET_EVENT_FLAG_APP_ID_SET;
    return listener && childPort && serverPort != childPort &&
           (h.flags & required) == required &&
           h.ipVersion == FWP_IP_VERSION_V4 && h.ipProtocol == IPPROTO_TCP &&
           h.localAddrV4 == INADDR_LOOPBACK &&
           h.remoteAddrV4 == INADDR_LOOPBACK && h.localPort == serverPort &&
           h.remotePort == childPort && h.appId.data &&
           h.appId.size == listener->size &&
           std::memcmp(h.appId.data, listener->data, listener->size) == 0 &&
           CompareFileTime(&h.timeStamp, &start) >= 0 &&
           CompareFileTime(&h.timeStamp, &end) <= 0 &&
           ((!h.packageSid &&
             !(h.flags & FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET)) ||
            (h.packageSid && (h.flags & FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET) &&
             IsValidSid(h.packageSid) &&
             IsWellKnownSid(h.packageSid, WinNullSid))) &&
           event.type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP &&
           event.classifyDrop && event.classifyDrop->isLoopback &&
           event.classifyDrop->msFwpDirection == 0x3901;
  }
  static bool childHasPackage(DWORD pid, PSID expected) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    HANDLE token = nullptr;
    bool valid   = false;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
      DWORD needed = 0;
      GetTokenInformation(token, TokenAppContainerSid, nullptr, 0, &needed);
      if (needed >= sizeof(TOKEN_APPCONTAINER_INFORMATION) && needed <= 4096) {
        QByteArray bytes(int(needed), char(0));
        if (GetTokenInformation(token, TokenAppContainerSid, bytes.data(),
                                needed, &needed)) {
          const auto info =
              reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION *>(bytes.data());
          valid = info->TokenAppContainer &&
                  IsValidSid(info->TokenAppContainer) &&
                  EqualSid(info->TokenAppContainer, expected);
        }
      }
      CloseHandle(token);
    }
    CloseHandle(process);
    return valid;
  }
  static bool testMatcher() {
    alignas(SID) unsigned char first[SECURITY_MAX_SID_SIZE]{};
    alignas(SID) unsigned char second[SECURITY_MAX_SID_SIZE]{};
    DWORD firstSize = sizeof(first), secondSize = sizeof(second);
    if (!CreateWellKnownSid(WinNullSid, nullptr, first, &firstSize) ||
        !CreateWellKnownSid(WinWorldSid, nullptr, second, &secondSize))
      return false;
    unsigned char identity[] = {1, 2}, wrong[] = {1, 3};
    FWP_BYTE_BLOB application{sizeof(identity), identity};
    FILETIME start{2, 0};
    FWPM_NET_EVENT_CLASSIFY_DROP2 drop{};
    drop.isLoopback = TRUE;
    FWPM_NET_EVENT2 event{};
    event.type         = FWPM_NET_EVENT_TYPE_CLASSIFY_DROP;
    event.classifyDrop = &drop;
    event.header.flags = FWPM_NET_EVENT_FLAG_IP_VERSION_SET |
                         FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET |
                         FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET |
                         FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET |
                         FWPM_NET_EVENT_FLAG_APP_ID_SET |
                         FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET;
    event.header.ipVersion    = FWP_IP_VERSION_V4;
    event.header.ipProtocol   = IPPROTO_TCP;
    event.header.remoteAddrV4 = INADDR_LOOPBACK;
    event.header.remotePort   = 12345;
    event.header.appId        = application;
    event.header.packageSid   = reinterpret_cast<SID *>(first);
    event.header.timeStamp    = start;
    auto accepts              = [&](const FWPM_NET_EVENT2 &candidate) {
      return matches(candidate, &application, first, 12345, start);
    };
    if (!accepts(event)) return false;
    auto changed              = event;
    changed.header.appId.data = wrong;
    if (accepts(changed)) return false;
    changed                   = event;
    changed.header.packageSid = reinterpret_cast<SID *>(second);
    if (accepts(changed)) return false;
    changed = event;
    changed.header.remotePort++;
    if (accepts(changed)) return false;
    changed = event;
    changed.header.timeStamp.dwLowDateTime--;
    if (accepts(changed)) return false;
    changed      = event;
    changed.type = FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW;
    if (accepts(changed)) return false;
    changed = event;
    changed.header.flags &= ~FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET;
    if (accepts(changed)) return false;
    drop.isLoopback = FALSE;
    if (accepts(event)) return false;
    drop.isLoopback     = TRUE;
    drop.msFwpDirection = 0x3901;
    event.header.flags |=
        FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET | FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET;
    event.header.localAddrV4 = INADDR_LOOPBACK;
    event.header.localPort   = 12345;
    event.header.remotePort  = 23456;
    FILETIME end{3, 0};
    auto reply = [&](const FWPM_NET_EVENT2 &candidate) {
      return matchesReply(candidate, &application, 12345, 23456, start, end);
    };
    if (!reply(event))
      return false;  // Explicit null SID for ordinary listener.
    changed = event;
    changed.header.remotePort++;
    if (reply(changed)) return false;
    changed = event;
    changed.header.localPort++;
    if (reply(changed)) return false;
    changed                   = event;
    changed.header.appId.data = wrong;
    if (reply(changed)) return false;
    changed                                = event;
    changed.header.timeStamp.dwLowDateTime = 1;
    if (reply(changed)) return false;
    changed.header.timeStamp.dwLowDateTime = 4;
    if (reply(changed)) return false;
    changed                   = event;
    changed.header.packageSid = reinterpret_cast<SID *>(second);
    if (reply(changed)) return false;
    drop.msFwpDirection = 0x3900;
    return !reply(event);
  }
  ~BrushWfpDropEvidence() {
    if (application && pFwpmFreeMemory0)
      pFwpmFreeMemory0(reinterpret_cast<void **>(&application));
    if (listenerApplication && pFwpmFreeMemory0)
      pFwpmFreeMemory0(reinterpret_cast<void **>(&listenerApplication));
    if (engine && pFwpmEngineClose0) pFwpmEngineClose0(engine);
    if (module) FreeLibrary(module);
  }
  bool begin(const QString &executable) {
    // A reply event names this ordinary listener. Never interpret another
    // AppContainer's package as an absent listener package.
    HANDLE token      = nullptr;
    DWORD isContainer = 1, needed = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
      return false;
    const bool ordinary =
        GetTokenInformation(token, TokenIsAppContainer, &isContainer,
                            sizeof(isContainer), &needed) &&
        !isContainer;
    CloseHandle(token);
    if (!ordinary) return false;
    module =
        LoadLibraryExW(L"fwpuclnt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return false;
#define BRUSH_WFP_LOAD(name)                                                   \
  p##name = reinterpret_cast<decltype(&name)>(GetProcAddress(module, #name));  \
  if (!p##name) return false
    BRUSH_WFP_LOAD(FwpmEngineOpen0);
    BRUSH_WFP_LOAD(FwpmEngineClose0);
    BRUSH_WFP_LOAD(FwpmGetAppIdFromFileName0);
    BRUSH_WFP_LOAD(FwpmNetEventCreateEnumHandle0);
    BRUSH_WFP_LOAD(FwpmNetEventDestroyEnumHandle0);
    BRUSH_WFP_LOAD(FwpmNetEventEnum2);
    BRUSH_WFP_LOAD(FwpmFreeMemory0);
#undef BRUSH_WFP_LOAD
    DWORD status =
        pFwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, nullptr, &engine);
    if (status == ERROR_SUCCESS) {
      const auto native = QDir::toNativeSeparators(executable).toStdWString();
      status = pFwpmGetAppIdFromFileName0(native.c_str(), &application);
    }
    if (status == ERROR_SUCCESS) {
      const auto native =
          QDir::toNativeSeparators(QCoreApplication::applicationFilePath())
              .toStdWString();
      status = pFwpmGetAppIdFromFileName0(native.c_str(), &listenerApplication);
    }
    GetSystemTimeAsFileTime(&start);
    if (status != ERROR_SUCCESS)
      std::fprintf(stderr, "WFP evidence initialization failed: %lu\n",
                   static_cast<unsigned long>(status));
    return status == ERROR_SUCCESS;
  }
  bool dropped(quint16 port, PSID package, quint16 childPort,
               const FILETIME &attemptStart, const FILETIME &attemptEnd) {
    if (!engine || !application || !package) return false;
    FWPM_NET_EVENT_ENUM_TEMPLATE0 selection{};
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    if (CompareFileTime(&attemptStart, &start) < 0 ||
        CompareFileTime(&attemptEnd, &attemptStart) < 0 ||
        CompareFileTime(&attemptEnd, &now) > 0)
      return false;
    selection.startTime = attemptStart;
    selection.endTime   = attemptEnd;
    HANDLE enumeration  = nullptr;
    DWORD status =
        pFwpmNetEventCreateEnumHandle0(engine, &selection, &enumeration);
    if (status != ERROR_SUCCESS) {
      std::fprintf(stderr, "WFP event enumeration unavailable: %lu\n",
                   static_cast<unsigned long>(status));
      return false;
    }
    bool matched             = false;
    UINT32 inspected         = 0;
    unsigned int diagnostics = 0;
    QElapsedTimer deadline;
    deadline.start();
    for (unsigned int page = 0; page < 32 && deadline.elapsed() < 2000;
         ++page) {
      FWPM_NET_EVENT2 **events = nullptr;
      UINT32 count             = 0;
      status = pFwpmNetEventEnum2(engine, enumeration, 128, &events, &count);
      if (status != ERROR_SUCCESS || count > 128) {
        if (events) pFwpmFreeMemory0(reinterpret_cast<void **>(&events));
        break;
      }
      inspected += count;
      for (UINT32 i = 0; i < count; ++i) {
        const auto &event = *events[i];
        if (CompareFileTime(&event.header.timeStamp, &attemptEnd) > 0 ||
            (!matches(event, application, package, port, attemptStart) &&
             !matchesReply(event, listenerApplication, port, childPort,
                           attemptStart, attemptEnd))) {
          // Bound diagnostics independently of the event scan. Never disclose
          // application paths, package SIDs, or unrelated network addresses.
          if (diagnostics++ < 16) {
            const auto &header = event.header;
            const bool appMatches =
                (header.flags & FWPM_NET_EVENT_FLAG_APP_ID_SET) &&
                header.appId.data && header.appId.size == application->size &&
                std::memcmp(header.appId.data, application->data,
                            application->size) == 0;
            const bool packageMatches =
                (header.flags & FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET) &&
                header.packageSid && IsValidSid(header.packageSid) &&
                EqualSid(header.packageSid, package);
            int loopback           = -1;
            unsigned int direction = 0;
            UINT64 filter          = 0;
            if (event.type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP &&
                event.classifyDrop) {
              loopback  = event.classifyDrop->isLoopback ? 1 : 0;
              direction = event.classifyDrop->msFwpDirection;
              filter    = event.classifyDrop->filterId;
            } else if (event.type == FWPM_NET_EVENT_TYPE_CAPABILITY_DROP &&
                       event.capabilityDrop) {
              loopback = event.capabilityDrop->isLoopback ? 1 : 0;
              filter   = event.capabilityDrop->filterId;
            }
            std::fprintf(
                stderr,
                "WFP unmatched event: type=%d flags=0x%lx appMatch=%d "
                "packageMatch=%d listenerMatch=%d nullPackage=%d fresh=%d "
                "ipVersion=%d protocol=%u "
                "localPort=%u remotePort=%u expectedPort=%u "
                "remoteLoopback=%d localLoopback=%d loopback=%d "
                "direction=%u filter=%llu\n",
                int(event.type), static_cast<unsigned long>(header.flags),
                int(appMatches), int(packageMatches),
                int(listenerApplication && header.appId.data &&
                    header.appId.size == listenerApplication->size &&
                    std::memcmp(header.appId.data, listenerApplication->data,
                                listenerApplication->size) == 0),
                int(header.packageSid && IsValidSid(header.packageSid) &&
                    IsWellKnownSid(header.packageSid, WinNullSid)),
                int(CompareFileTime(&header.timeStamp, &start) >= 0),
                int(header.ipVersion), unsigned(header.ipProtocol),
                unsigned(header.localPort), unsigned(header.remotePort),
                unsigned(port),
                int(header.ipVersion == FWP_IP_VERSION_V4 &&
                    header.remoteAddrV4 == INADDR_LOOPBACK),
                int(header.ipVersion == FWP_IP_VERSION_V4 &&
                    header.localAddrV4 == INADDR_LOOPBACK),
                loopback, direction, static_cast<unsigned long long>(filter));
          }
          continue;
        }
        const bool classify = event.type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP;
        const UINT64 filter = classify ? event.classifyDrop->filterId
                                       : event.capabilityDrop->filterId;
        std::fprintf(
            stderr,
            "Matched WFP loopback drop: type=%d filter=%llu remotePort=%u\n",
            int(event.type), static_cast<unsigned long long>(filter),
            unsigned(port));
        matched = true;
      }
      if (events) pFwpmFreeMemory0(reinterpret_cast<void **>(&events));
      if (matched || count < 128) break;
    }
    pFwpmNetEventDestroyEnumHandle0(engine, enumeration);
    if (!matched)
      std::fprintf(stderr, "No matching WFP drop: status=%lu events=%u\n",
                   static_cast<unsigned long>(status), unsigned(inspected));
    return matched;
  }
};
}  // namespace
}  // namespace RasterBrush
#endif
