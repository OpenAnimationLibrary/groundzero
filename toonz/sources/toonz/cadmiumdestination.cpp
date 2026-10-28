#include "cadmiumdestination.h"

#include "tsystem.h"
#include "toonz/levelset.h"
#include "toonz/toonzscene.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <limits>
#include <vector>

namespace Cadmium {
namespace {
bool samePath(const TFilePath &left, const TFilePath &right) {
#ifdef Q_OS_WIN
  return QString::compare(left.getQString(), right.getQString(),
                          Qt::CaseInsensitive) == 0;
#else
  return left == right;
#endif
}
}

TXshSimpleLevel *createResultLevel(ToonzScene *scene,
                                  const TXshSimpleLevel *source,
                                  QString &error) {
  error.clear();
  if (!scene || !source || source->getType() != OVL_XSHLEVEL) {
    error = QObject::tr("A full-color source level and scene are required.");
    return nullptr;
  }
  const std::string extension = source->isFloatChannelLevel() ? "exr" :
      source->is16BitChannelLevel() ? "tif" : "png";
  const std::wstring base = source->getName() + L"_paint";
  // Append numeric name suffixes only after the sequence path is formed.
  // Otherwise a trailing "_1" can be parsed as an existing frame number.
  const TFilePath basePath =
      scene->getDefaultLevelPath(OVL_XSHLEVEL, base).withType(extension);
  std::vector<TXshLevel *> levels;
  scene->getLevelSet()->listLevels(levels);
  for (int index = 0; index < std::numeric_limits<int>::max(); ++index) {
    const std::wstring name = index == 0 ? base :
        base + L"_" + std::to_wstring(index);
    if (scene->getLevelSet()->getLevel(name)) continue;
    const TFilePath path = basePath.withName(name);
    const TFilePath actual = scene->decodeFilePath(path);
    if (TSystem::doesExistFileOrLevel(actual)) continue;
    bool occupied = false;
    for (TXshLevel *level : levels) {
      const TXshSimpleLevel *simple = level ? level->getSimpleLevel() : nullptr;
      if (simple && samePath(scene->decodeFilePath(simple->getPath()), actual)) {
        occupied = true;
        break;
      }
    }
    if (occupied) continue;
    TXshLevel *created = scene->createNewLevel(
        OVL_XSHLEVEL, name, TDimension(), 0.0, path);
    TXshSimpleLevel *result = created ? created->getSimpleLevel() : nullptr;
    if (!result || result->getName() != name ||
        !samePath(scene->decodeFilePath(result->getPath()), actual)) {
      if (created) scene->getLevelSet()->removeLevel(created);
      error = QObject::tr("The selected result path changed while creating the level. Try again.");
      return nullptr;
    }
    return result;
  }
  error = QObject::tr("A unique result level could not be created.");
  return nullptr;
}
}
