#pragma once

#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshlevel.h"

#include <QString>
#include <QStringList>
#include <algorithm>
#include <functional>
#include <set>
#include <vector>

// Read-only scene indexing shared by the native Cast and its regression tests.
// A path identifies an exposed child instance, not merely a shared child sheet.
namespace CastWorkflow {
struct Step {
  int row = 0, column = 0;
  TXshChildLevelP child;
};
struct Occurrence {
  TXshLevelP level;
  TXsheetP sheet;
  std::vector<Step> path;
  int column = 0, first = 0, last = 0;
};
struct Index {
  std::vector<Occurrence> occurrences;
  std::set<TXshLevel *> used;
  bool complete = true;
  bool cycle = false;
  size_t cells = 0;
};
struct Limits {
  size_t cells = 2000000, occurrences = 10000, instances = 10000;
  size_t depth = 64;
  bool recordOccurrences = true, expandInstances = true;
};

// Every distinct exposed run of a child is an instance. Adjacent drawings of
// the same child stay one instance, while different columns/runs remain distinct.
inline Index index(TXsheet *top, const std::set<TXshLevel *> &wanted = {},
                   const std::function<bool()> &cancel = {}, Limits limits = {}) {
  Index result;
  std::set<TXsheet *> ancestors, visited;
  std::vector<Step> path;
  size_t instances = 0;
  std::function<void(TXsheet *)> visit = [&](TXsheet *sheet) {
    if (!sheet || !result.complete) return;
    if (ancestors.count(sheet)) { result.cycle = true; return; }
    if (!limits.expandInstances && !visited.insert(sheet).second) return;
    if (path.size() > limits.depth || ++instances > limits.instances) {
      result.complete = false;
      return;
    }
    ancestors.insert(sheet);
    for (int c = 0; c < sheet->getColumnCount() && result.complete; ++c) {
      int first = 0, last = -1;
      sheet->getCellRange(c, first, last);
      TXshLevel *previous = nullptr;
      int occurrence = -1;
      for (int r = first; r <= last && result.complete; ++r) {
        if (++result.cells > limits.cells ||
            ((result.cells & 255) == 1 && cancel && cancel())) {
          result.complete = false;
          break;
        }
        const TXshCell &cell = sheet->getCell(r, c);
        TXshLevel *level = cell.m_level.getPointer();
        if (level) result.used.insert(level);
        if (limits.recordOccurrences && level && (wanted.empty() || wanted.count(level))) {
          if (level == previous && occurrence >= 0) {
            result.occurrences[occurrence].last = r;
          } else {
            if (result.occurrences.size() >= limits.occurrences) {
              result.complete = false;
              break;
            }
            occurrence = int(result.occurrences.size());
            result.occurrences.push_back({TXshLevelP(level), TXsheetP(sheet),
                                          path, c, r, r});
          }
        } else occurrence = -1;
        if (level && level != previous && level->getChildLevel()) {
          TXshChildLevel *child = level->getChildLevel();
          path.push_back({r, c, TXshChildLevelP(child)});
          visit(child->getXsheet());
          path.pop_back();
        }
        previous = level;
      }
    }
    ancestors.erase(sheet);
  };
  visit(top);
  return result;
}

inline bool validPath(TXsheet *top, const Occurrence &use) {
  TXsheet *sheet = top;
  for (const Step &step : use.path) {
    if (!sheet || sheet->getCell(step.row, step.column).m_level.getPointer() !=
                      step.child.getPointer()) return false;
    sheet = step.child->getXsheet();
  }
  return sheet == use.sheet.getPointer() &&
         sheet->getCell(use.first, use.column).m_level == use.level;
}

inline bool matches(const QString &query, const QString &name,
                    const QString &path, const QStringList &tags) {
  const QString haystack = name + QLatin1Char('\n') + path +
                          QLatin1Char('\n') + tags.join(QLatin1Char('\n'));
  const QStringList words = query.simplified().split(QLatin1Char(' '),
                                                    Qt::SkipEmptyParts);
  for (const QString &word : words)
    if (!haystack.contains(word, Qt::CaseInsensitive)) return false;
  return true;
}

inline QStringList normalizeTags(const QString &text) {
  QStringList tags;
  for (QString tag : text.split(QLatin1Char(','))) {
    tag = tag.trimmed().left(80);
    if (!tag.isEmpty() && !tags.contains(tag, Qt::CaseInsensitive))
      tags.append(tag);
    if (tags.size() == 32) break;
  }
  return tags;
}
}  // namespace CastWorkflow
