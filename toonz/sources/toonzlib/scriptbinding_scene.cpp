

#include "toonz/scriptbinding_scene.h"
#include "toonz/scriptbinding_level.h"
#include "toonz/scriptbinding_image.h"
#include "toonz/scriptbinding_renderer.h"
#include "toonz/scriptbinding_files.h"
#include "toonz/scriptbinding_stageobject.h"
#include "toonz/txshleveltypes.h"

#include "tsystem.h"
#include "tfiletype.h"

#include "toonz/tproject.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/levelset.h"
#include "toonz/scenechecker.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"

namespace TScriptBinding {

Scene::Scene() {
  m_scene = new ToonzScene();
  TProjectManager::instance()->initializeScene(m_scene);
}

Scene::~Scene() { delete m_scene; }

QJSValue Scene::ctor(QJSEngine *engine, const QJSValueList &args) {
  Scene *scene = new Scene();
  QJSValue obj = create(engine, scene);
  if (args.size() == 1) {
    return scene->load(args[0]);
  }
  return obj;
}

QString Scene::toString() {
  return QString("Scene (%1 frames)").arg(getFrameCount());
}

int Scene::getFrameCount() { return m_scene->getFrameCount(); }

int Scene::getColumnCount() { return m_scene->getXsheet()->getColumnCount(); }

QJSValue Scene::load(const QJSValue &fpArg) {
  TFilePath fp;
  if (!checkFilePath(engine(), fpArg, fp)) return QJSValue();
  if (!fp.isAbsolute())
    fp = TProjectManager::instance()->getCurrentProject()->getScenesPath() + fp;
  try {
    if (!TSystem::doesExistFileOrLevel(fp)) {
      return throwError(tr("File %1 doesn't exist").arg(fpArg.toString()));
    }
    m_scene->load(fp);
    return self();
  } catch (...) {
    return throwError(tr("Exception reading %1").arg(fpArg.toString()));
  }
}

QJSValue Scene::save(const QJSValue &fpArg) {
  TFilePath fp;
  if (!checkFilePath(engine(), fpArg, fp)) return QJSValue();
  if (!fp.isAbsolute())
    fp = TProjectManager::instance()->getCurrentProject()->getScenesPath() + fp;
  try {
    m_scene->save(fp);
    return self();
  } catch (...) {
    return throwError(tr("Exception writing %1").arg(fpArg.toString()));
  }
}

QJSValue Scene::getLevels() const {
  QJSValue result     = engine()->newArray();
  qint32 index        = 0;
  std::vector<TXshLevel *> levels;
  m_scene->getLevelSet()->listLevels(levels);
  for (std::vector<TXshLevel *>::iterator it = levels.begin();
       it != levels.end(); ++it) {
    TXshSimpleLevel *sl = (*it)->getSimpleLevel();
    if (sl) {
      Level *level = new Level(sl);
      level->keepScene(self());
      result.setProperty(index++, create(engine(), level));
    }
  }
  return result;
}

QJSValue Scene::getLevel(const QString &name) const {
  TXshLevel *xl       = m_scene->getLevelSet()->getLevel(name.toStdWString());
  TXshSimpleLevel *sl = xl ? xl->getSimpleLevel() : 0;
  if (sl) {
    Level *level = new Level(sl);
    level->keepScene(self());
    return create(engine(), level);
  } else
    return QJSValue();
}

QJSValue Scene::newLevel(const QString &levelTypeStr,
                         const QString &levelName) const {
  int levelType = NO_XSHLEVEL;
  if (levelTypeStr == "Vector")
    levelType = PLI_XSHLEVEL;
  else if (levelTypeStr == "ToonzRaster")
    levelType = TZP_XSHLEVEL;
  else if (levelTypeStr == "Raster")
    levelType = OVL_XSHLEVEL;
  if (levelType == NO_XSHLEVEL)
    return throwError(
        tr("Bad level type (%1): must be Vector,Raster or ToonzRaster")
            .arg(levelTypeStr));

  if (m_scene->getLevelSet()->hasLevel(levelName.toStdWString()))
    return throwError(
        tr("Can't add the level: name(%1) is already used").arg(levelName));

  TXshLevel *xl = m_scene->createNewLevel(levelType, levelName.toStdWString());
  xl->getSimpleLevel()->setDirtyFlag(true);
  Level *level = new Level(xl->getSimpleLevel());
  level->keepScene(self());
  return create(engine(), level);
}

QJSValue Scene::loadLevel(const QString &levelName,
                          const QJSValue &fpArg) const {
  if (m_scene->getLevelSet()->hasLevel(levelName.toStdWString()))
    return throwError(
        tr("Can't add the level: name(%1) is already used").arg(levelName));
  TFilePath fp;
  if (!checkFilePath(engine(), fpArg, fp)) return QJSValue();
  TFileType::Type type = TFileType::getInfo(fp);
  if ((type & TFileType::VIEWABLE) == 0)
    return throwError(tr("Can't load this kind of file as a level : %1")
                          .arg(fpArg.toString()));
  TXshLevel *xl = m_scene->loadLevel(fp);
  if (!xl || !xl->getSimpleLevel())
    return throwError(tr("Could not load level %1").arg(fpArg.toString()));
  Level *level = new Level(xl->getSimpleLevel());
  level->keepScene(self());
  return create(engine(), level);
}

QString Scene::doSetCell(int row, int col, const QJSValue &levelArg,
                         const QJSValue &fidArg) {
  if (row < 0 || col < 0) {
    return "Bad row/col values";
  }

  QString err;
  TXshCell cell;
  cell.m_frameId = Level::getFid(fidArg, err);
  if (err != "") return err;
  Level *level = cast<Level>(levelArg);
  if (level) {
    TXshSimpleLevel *sl = level->getSimpleLevel();
    TXshLevel *xl       = m_scene->getLevelSet()->getLevel(sl->getName());
    if (!xl || xl->getSimpleLevel() != sl) {
      return tr("Level is not included in the scene : %1")
          .arg(levelArg.toString());
    }
    cell.m_level = sl;
  } else {
    if (!levelArg.isString())
      return tr("%1 : Expected a Level instance or a level name")
          .arg(levelArg.toString());
    QString levelName = levelArg.toString();
    TXshLevel *xl = m_scene->getLevelSet()->getLevel(levelName.toStdWString());
    if (!xl)
      return tr("Level '%1' is not included in the scene").arg(levelName);
    cell.m_level = xl;
  }
  m_scene->getXsheet()->setCell(row, col, cell);
  return "";
}

QJSValue Scene::setCell(int row, int col, const QJSValue &level,
                        const QJSValue &fid) {
  QString err = doSetCell(row, col, level, fid);
  if (err != "") return throwError(err);
  return self();
}

QJSValue Scene::setCell(int row, int col, const QJSValue &cellArg) {
  if (cellArg.isUndefined()) {
    if (row >= 0 && col >= 0)
      m_scene->getXsheet()->setCell(row, col, TXshCell());
  } else {
    if (!cellArg.isObject() || cellArg.property("level").isUndefined() ||
        cellArg.property("fid").isUndefined())
      return throwError(
          "Third argument should be an object with attributes 'level' and "
          "'fid'");
    QJSValue levelArg     = cellArg.property("level");
    QJSValue fidArg       = cellArg.property("fid");
    QString err           = doSetCell(row, col, levelArg, fidArg);
    if (err != "") return throwError(err);
  }
  return self();
}

QJSValue Scene::getCell(int row, int col) {
  TXshCell cell       = m_scene->getXsheet()->getCell(row, col);
  TXshSimpleLevel *sl = cell.getSimpleLevel();
  if (sl) {
    QJSValue level = create(engine(), new Level(sl));
    QJSValue fid;
    if (cell.m_frameId.getLetter().isEmpty())
      fid = cell.m_frameId.getNumber();
    else
      fid = QString::fromStdString(cell.m_frameId.expand());
    QJSValue result = engine()->newObject();
    result.setProperty("level", level);
    result.setProperty("fid", fid);
    return result;
  } else {
    return QJSValue();
  }
}

QJSValue Scene::insertColumn(int col) {
  m_scene->getXsheet()->insertColumn(col);
  return self();
}

QJSValue Scene::deleteColumn(int col) {
  // removeColumn() does not check the index: a bad one corrupts the memory
  if (col < 0 || col >= m_scene->getXsheet()->getColumnCount())
    return throwError(tr("Bad column index (%1)").arg(col));
  m_scene->getXsheet()->removeColumn(col);
  return self();
}

Scene::ColumnInfo Scene::getColumnInfo(ToonzScene *scene, int col) {
  TXsheet *xsh = scene->getXsheet();
  ColumnInfo info;
  info.m_index = col;

  TStageObjectId id = TStageObjectId::ColumnId(col);
  info.m_id         = QString::fromStdString(id.toString());
  TStageObject *obj = xsh->getStageObjectTree()->getStageObject(id, false);
  info.m_name       = obj ? QString::fromStdString(obj->getName()) : info.m_id;

  info.m_type        = "Empty";
  TXshColumn *column = xsh->getColumn(col);
  if (column) {
    switch (column->getColumnType()) {
    case TXshColumn::eLevelType:
      info.m_type = "Level";
      break;
    case TXshColumn::eSoundType:
      info.m_type = "Sound";
      break;
    case TXshColumn::eSoundTextType:
      info.m_type = "SoundText";
      break;
    case TXshColumn::eZeraryFxType:
      info.m_type = "Zerary";
      break;
    case TXshColumn::ePaletteType:
      info.m_type = "Palette";
      break;
    case TXshColumn::eMeshType:
      info.m_type = "Mesh";
      break;
    default:
      info.m_type = "Unknown";
      break;
    }
  }

  int r0 = 0, r1 = -1;
  xsh->getCellRange(col, r0, r1);
  info.m_firstRow = r1 < r0 ? -1 : r0;
  info.m_lastRow  = r1 < r0 ? -1 : r1;
  return info;
}

QJSValue Scene::getColumns() {
  int count       = getColumnCount();
  QJSValue result = engine()->newArray((uint)count);
  for (int i = 0; i < count; ++i) {
    ColumnInfo info = getColumnInfo(m_scene, i);
    QJSValue column = engine()->newObject();
    column.setProperty("index", info.m_index);
    column.setProperty("id", info.m_id);
    column.setProperty("name", info.m_name);
    column.setProperty("type", info.m_type);
    column.setProperty("firstRow", info.m_firstRow);
    column.setProperty("lastRow", info.m_lastRow);
    result.setProperty(i, column);
  }
  return result;
}

QJSValue Scene::renderFrame(int frame) {
  if (frame < 0 || frame >= getFrameCount())
    return throwError(
        tr("Frame %1 is out of range: the scene has %2 frames (the first "
           "is 0)")
            .arg(frame)
            .arg(getFrameCount()));
  TImageP img = Renderer::renderFrameImage(m_scene, frame);
  if (!img) return throwError(tr("Render failed"));
  return create(engine(), new Image(img));
}

QJSValue Scene::check(const QJSValue &options) {
  SceneChecker::Options checkOptions;
  if (options.isObject())
    checkOptions.unusedStyles = options.property("unusedStyles").toBool();
  else if (!options.isUndefined() && !options.isNull())
    return throwError(tr("Bad argument (%1): expected an object such as "
                         "{unusedStyles: true}")
                          .arg(options.toString()));
  std::vector<SceneChecker::Issue> issues =
      SceneChecker::check(m_scene, checkOptions);
  QJSValue result = engine()->newArray((uint)issues.size());
  for (int i = 0; i < (int)issues.size(); ++i) {
    const SceneChecker::Issue &issue = issues[i];
    QJSValue item                    = engine()->newObject();
    item.setProperty("severity", SceneChecker::severityName(issue.severity));
    item.setProperty("kind", SceneChecker::kindName(issue.kind));
    item.setProperty("title", SceneChecker::kindTitle(issue.kind));
    item.setProperty("text", issue.text);
    item.setProperty("column", issue.column);
    item.setProperty("row", issue.row);
    item.setProperty("rowCount", issue.rowCount);
    item.setProperty("level", QString::fromStdWString(issue.levelName));
    item.setProperty("styleId", issue.styleId);
    item.setProperty("subXsheet", QString::fromStdWString(issue.subXsheet));
    result.setProperty(i, item);
  }
  return result;
}

QJSValue Scene::getStageObject(const QString &name) {
  TStageObject *obj = StageObject::findStageObject(m_scene->getXsheet(), name);
  if (!obj) return throwError(tr("Stage object not found: %1").arg(name));
  return create(engine(), new StageObject(self(), this, obj));
}

QJSValue Scene::getStageObjects() {
  std::vector<TStageObject *> objs;
  StageObject::listStageObjects(m_scene->getXsheet(), objs);
  QJSValue sceneValue = self();
  QJSValue result     = engine()->newArray((uint)objs.size());
  for (int i = 0; i < (int)objs.size(); ++i)
    result.setProperty(
        i, create(engine(), new StageObject(sceneValue, this, objs[i])));
  return result;
}

QJSValue Scene::newPegbar() {
  // as TStageObjectCmd::addNewPegbar(), without undo
  TStageObjectTree *tree = m_scene->getXsheet()->getStageObjectTree();
  int index              = 0;
  while (tree->getStageObject(TStageObjectId::PegbarId(index), false)) index++;
  TStageObject *obj =
      tree->getStageObject(TStageObjectId::PegbarId(index), true);
  return create(engine(), new StageObject(self(), this, obj));
}

}  // namespace TScriptBinding
