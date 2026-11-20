#pragma once

#include "automation.h"

#include <memory>

namespace GroundzeroAutomation {

// Inspect only the owned detached document, within the advertised read budgets.
QJsonArray productionOperations();
QJsonValue executeProduction(Context& context, const QString& method,
                             const QJsonObject& params);

// The rows of scene.dependencies, gathered one cast level per step and
// bounded by `rowLimit` rows instead of the read's record limit.
class DependencyWalk final {
public:
  DependencyWalk(Context& context, Document& document, int rowLimit);
  ~DependencyWalk();
  // Add the next level's rows, then the scene's own; false when no work is
  // left or the row limit was reached.
  bool step();
  int levels() const;
  int levelsDone() const;
  const QJsonArray& rows() const;
  // The read's incompleteness reasons: level_limit, record_limit, ...
  QStringList reasons() const;

private:
  struct Imp;
  std::unique_ptr<Imp> m_imp;
};

#ifdef BUILD_UI_PORT_TESTS
void runAutomationProductionTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
