#include "tgldisplaylistsmanager.h"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(1);
}

// Opaque identities only: the manager must not call any platform GL API.
TGlContext context(std::uintptr_t id) {
#ifdef _WIN32
  return TGlContext(reinterpret_cast<HDC>(id), reinterpret_cast<HGLRC>(id));
#else
  return reinterpret_cast<void *>(id);
#endif
}

class Proxy final : public TGLDisplayListsProxy {
  int &m_destroyed;

public:
  explicit Proxy(int &destroyed) : m_destroyed(destroyed) {}
  ~Proxy() override { ++m_destroyed; }
  void makeCurrent() override {}
  void doneCurrent() override {}
};

class Observer final : public TGLDisplayListsManager::Observer {
public:
  int calls = 0;
  std::function<void(int)> action;

  void onDisplayListDestroyed(int id) override {
    ++calls;
    require(TGLDisplayListsManager::instance()->dlProxy(id) != nullptr,
            "proxy unavailable during destruction notification");
    if (action) action(id);
  }
};

void sharingAndInvalidHandles() {
  auto *manager = TGLDisplayListsManager::instance();
  Observer observer;
  manager->addObserver(&observer);
  int destroyed = 0, otherDestroyed = 0;
  auto *proxy = new Proxy(destroyed);
  const int id = manager->storeProxy(proxy);
  const int other = manager->storeProxy(new Proxy(otherDestroyed));
  const auto first = context(1), second = context(2), third = context(3);
  manager->attachContext(id, first);
  manager->attachContext(id, first);  // Must not acquire twice.
  manager->attachContext(id, second);
  manager->attachContext(other, first);  // Must not replace an owned mapping.
  manager->attachContext(other, third);
  require(manager->displayListsSpaceId(first) == id,
          "conflicting attach replaced the context's display-list space");

  manager->releaseContext(first);
  manager->releaseContext(first);
  require(manager->displayListsSpaceId(first) == -1 &&
              manager->displayListsSpaceId(second) == id &&
              manager->dlProxy(id) == proxy && destroyed == 0 &&
              observer.calls == 0,
          "releasing one shared context retired a surviving context's proxy");
  manager->releaseContext(second);
  require(destroyed == 1 && observer.calls == 1 && !manager->dlProxy(id),
          "final release did not retire the shared proxy exactly once");
  manager->releaseContext(second);
  manager->attachContext(id, first);  // An erased slot is not a valid handle.
  manager->attachContext(-1, first);
  manager->attachContext((std::numeric_limits<int>::max)(), first);
  require(manager->displayListsSpaceId(first) == -1 && !manager->dlProxy(-1) &&
              !manager->dlProxy((std::numeric_limits<int>::max)()),
          "invalid display-list handles were accepted");
  manager->releaseContext(third);
  require(otherDestroyed == 1 && observer.calls == 2,
          "conflicting attach leaked a proxy reference");
}

void reentrantRetirementAndGrowth() {
  auto *manager = TGLDisplayListsManager::instance();
  Observer observer;
  manager->addObserver(&observer);
  int destroyed = 0;
  auto *proxy = new Proxy(destroyed);
  const int id = manager->storeProxy(proxy);
  const auto original = context(10), rejected = context(11);
  manager->attachContext(id, original);

  // Force growth while the retiring slot is still reserved. A reference into
  // the indexed list held across the callback would now point at old storage.
  constexpr int count = 128;
  std::vector<int> destroyedChildren(count, 0), ids;
  observer.action = [&](int notified) {
    if (notified != id) return;
    require(manager->displayListsSpaceId(original) == -1,
            "final release remained visible during observer notification");
    manager->releaseContext(original);
    manager->attachContext(id, original);
    manager->attachContext(id, rejected);
    require(manager->displayListsSpaceId(original) == -1 &&
                manager->displayListsSpaceId(rejected) == -1,
            "retiring proxy accepted a new context");
    for (int i = 0; i < count; ++i) {
      const int child = manager->storeProxy(new Proxy(destroyedChildren[i]));
      require(child != id, "retiring slot was reused during notification");
      ids.push_back(child);
      manager->attachContext(child, i == 0 ? original : context(100 + i));
    }
    require(manager->dlProxy(id) == proxy && destroyed == 0,
            "proxy died or changed while its observer was running");
  };
  manager->releaseContext(original);
  observer.action = {};
  require(destroyed == 1 && observer.calls == 1 && !manager->dlProxy(id),
          "reentrant release retired the original proxy more than once");
  require(manager->displayListsSpaceId(original) == ids.front(),
          "outer release removed a replacement registration from its callback");

  int reusedDestroyed = 0;
  const int reused = manager->storeProxy(new Proxy(reusedDestroyed));
  require(reused == id, "retired slot was not returned to the reusable pool");
  manager->attachContext(reused, rejected);
  manager->releaseContext(rejected);
  require(reusedDestroyed == 1, "reused slot did not retire normally");
  for (int i = 0; i < count; ++i) {
    manager->releaseContext(i == 0 ? original : context(100 + i));
    require(destroyedChildren[i] == 1 && !manager->dlProxy(ids[i]),
            "growth or slot reuse corrupted a surviving proxy");
  }
  require(observer.calls == count + 2,
          "growth or reentrant release lost or duplicated notifications");
}

void throwingObserverStillRetires() {
  auto *manager = TGLDisplayListsManager::instance();
  Observer observer;
  manager->addObserver(&observer);
  int destroyed = 0;
  const int id = manager->storeProxy(new Proxy(destroyed));
  const auto owner = context(1000);
  manager->attachContext(id, owner);
  observer.action = [](int) { throw std::runtime_error("observer failure"); };
  bool caught = false;
  try {
    manager->releaseContext(owner);
  } catch (const std::runtime_error &) {
    caught = true;
  }
  require(caught && destroyed == 1 && observer.calls == 1 &&
              manager->displayListsSpaceId(owner) == -1 && !manager->dlProxy(id),
          "throwing observer left a registered or live retiring proxy");
  manager->releaseContext(owner);
  require(observer.calls == 1, "repeated release notified after observer failure");
}

}  // namespace

int main() {
  sharingAndInvalidHandles();
  reentrantRetirementAndGrowth();
  throwingObserverStillRetires();
  std::cout << "display-list context ownership and retirement checks passed\n";
}
