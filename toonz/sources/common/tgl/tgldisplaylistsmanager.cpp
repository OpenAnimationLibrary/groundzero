

// TnzCore includes
#include "tsmartpointer.h"

// tcg includes
#include "tcg/tcg_list.h"

#include "tgldisplaylistsmanager.h"

//***********************************************************************************************
//    Local namespace - declarations
//***********************************************************************************************

namespace {

struct ProxyReference {
  TGLDisplayListsProxy *m_proxy;
  int m_refCount;
  bool m_retiring = false;

  ProxyReference(TGLDisplayListsProxy *proxy) : m_proxy(proxy), m_refCount() {}
};

}  // namespace

//***********************************************************************************************
//    Local namespace - globals
//***********************************************************************************************

namespace {

tcg::list<ProxyReference> m_proxies;
std::map<TGlContext, int> m_proxyIdsByContext;

bool validProxyId(int id) {
  return id >= 0 && static_cast<size_t>(id) < m_proxies.nodesCount() &&
         m_proxies.isValid(id);
}

// Keep the slot occupied through callbacks and proxy destruction: either may
// register another context and grow the indexed list.
struct ProxyRetirement {
  int m_id;
  ~ProxyRetirement() {
    TGLDisplayListsProxy *proxy = m_proxies[m_id].m_proxy;
    m_proxies[m_id].m_proxy = nullptr;
    delete proxy;
    m_proxies.erase(m_id);
  }
};

}  // namespace

//***********************************************************************************************
//    TGLDisplayListsManager  implementation
//***********************************************************************************************

TGLDisplayListsManager *TGLDisplayListsManager::instance() {
  static TGLDisplayListsManager theInstance;
  return &theInstance;
}

//-----------------------------------------------------------------------------------

int TGLDisplayListsManager::storeProxy(TGLDisplayListsProxy *proxy) {
  return m_proxies.push_back(ProxyReference(proxy));
}

//-----------------------------------------------------------------------------------

void TGLDisplayListsManager::attachContext(int dlSpaceId, TGlContext context) {
  if (!validProxyId(dlSpaceId) || m_proxies[dlSpaceId].m_retiring) return;
  // A context has one registration. Repeated or conflicting attaches must not
  // acquire another reference without a corresponding mapping.
  if (m_proxyIdsByContext.insert(std::make_pair(context, dlSpaceId)).second)
    ++m_proxies[dlSpaceId].m_refCount;
}

//-----------------------------------------------------------------------------------

void TGLDisplayListsManager::releaseContext(TGlContext context) {
  std::map<TGlContext, int>::iterator it = m_proxyIdsByContext.find(context);

  if (it == m_proxyIdsByContext.end()) return;

  const int dlSpaceId = it->second;
  // Remove admission before notifying observers. A callback can release the
  // same context again, or register a replacement with the same native handle.
  m_proxyIdsByContext.erase(it);
  if (!validProxyId(dlSpaceId) || m_proxies[dlSpaceId].m_retiring) return;
  if (--m_proxies[dlSpaceId].m_refCount > 0) return;

  m_proxies[dlSpaceId].m_retiring = true;
  ProxyRetirement retirement{dlSpaceId};
  // dlProxy(id) remains available while textures are destroyed by observers.
  // Do not retain a reference into m_proxies across these callbacks.
  const observers_container snapshot = observers();
  for (auto *observer : snapshot) {
    if (observers().find(observer) != observers().end())
      static_cast<Observer *>(observer)->onDisplayListDestroyed(dlSpaceId);
  }
}

//-----------------------------------------------------------------------------------

int TGLDisplayListsManager::displayListsSpaceId(TGlContext context) {
  std::map<TGlContext, int>::iterator it = m_proxyIdsByContext.find(context);
  return (it == m_proxyIdsByContext.end()) ? -1 : it->second;
}

//-----------------------------------------------------------------------------------

TGLDisplayListsProxy *TGLDisplayListsManager::dlProxy(int dlSpaceId) {
  return validProxyId(dlSpaceId) ? m_proxies[dlSpaceId].m_proxy : nullptr;
}
