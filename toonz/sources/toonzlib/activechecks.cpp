#include "toonz/activechecks.h"

#include "toonz/tcolumnfx.h"
#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevelcolumn.h"

void resolveActiveRenderChecks(ToonzScene *scene,
                               TRenderCheckSettings &checks) {
  checks.m_targetFxId          = 0;
  const std::vector<int> &path = checks.m_columnPath;
  if (!scene || !checks.m_enabled || path.empty() || path.size() > 65 ||
      path.size() % 2 == 0)
    return;

  TXsheet *xsheet = scene->getTopXsheet();
  for (size_t i = 0; xsheet && i + 1 < path.size(); i += 2) {
    const int column = path[i], row = path[i + 1];
    if (column < 0 || column >= xsheet->getColumnCount() || row < 0) return;
    TXshChildLevel *child = xsheet->getCell(row, column).getChildLevel();
    if (!child) return;
    xsheet = child->getXsheet();
  }

  const int columnIndex = path.back();
  if (!xsheet || columnIndex < 0 || columnIndex >= xsheet->getColumnCount())
    return;
  TXshColumn *column           = xsheet->getColumn(columnIndex);
  TXshLevelColumn *levelColumn = column ? column->getLevelColumn() : nullptr;
  if (levelColumn && levelColumn->getLevelColumnFx())
    checks.m_targetFxId = levelColumn->getLevelColumnFx()->getIdentifier();
}
