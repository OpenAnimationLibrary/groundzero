#include "librarycommand.h"

// Tnz6 includes
#include "tapp.h"
#include "menubarcommandids.h"
#include "subscenecommand.h"
#include "symbolspane.h"
#include "posespane.h"
#include "columnselection.h"
#include "filebrowser.h"

// TnzQt includes
#include "toonzqt/menubarcommand.h"
#include "toonzqt/dvdialog.h"
#include "toonzqt/gutil.h"
#include "toonzqt/tselectionhandle.h"

// TnzLib includes
#include "toonz/libraryitems.h"
#include "toonz/poselibrary.h"
#include "toonz/toonzscene.h"
#include "toonz/toonzfolders.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshchildlevel.h"

// TnzCore includes
#include "tsystem.h"
#include "tstream.h"

// Qt includes
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

// STD includes
#include <map>
#include <set>
#include <string>
#include <vector>

//*****************************************************************************
//    Local namespace
//*****************************************************************************

namespace {

// The root tag of the pose set files
const char PoseSetTag[] = "poseSet";

//-----------------------------------------------------------------------------

QString invalidNameMessage() {
  return QObject::tr(
      "The name cannot be empty, or made only of characters that cannot be "
      "used in file names.");
}

//-----------------------------------------------------------------------------

// The name of an item: its file name without the extension, as the Library
// panel shows it.
QString getItemName(const TFilePath &itemPath) {
  return QFileInfo(itemPath.getQString()).completeBaseName();
}

//-----------------------------------------------------------------------------

TFilePath getItemPath(const TFilePath &folder, const QString &name,
                      const QString &extension) {
  return folder + TFilePath(name + "." + extension);
}

//-----------------------------------------------------------------------------

bool isTemplate(const TFilePath &itemPath) {
  return itemPath.getType() == "tnz";
}

//-----------------------------------------------------------------------------

QString getSourceSceneName() {
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  if (!scene || scene->isUntitled()) return QString();
  return QString::fromStdWString(scene->getSceneName());
}

//-----------------------------------------------------------------------------

// The folder of the level files of a template: the one its .libinfo records
// (only a plain name is accepted: never a path), or the one named after it.
std::wstring getUsedLevelFolder(const TFilePath &templatePath) {
  LibraryCmd::ItemInfo info;
  if (LibraryCmd::readItemInfo(templatePath, info) &&
      LibraryItems::isSafeFileName(info.m_levelFolder.toStdWString()))
    return info.m_levelFolder.toStdWString();
  return getItemName(templatePath).toStdWString();
}

//-----------------------------------------------------------------------------

// The level folders used by the templates next to exceptPath, except it, in
// lower case.
std::set<std::wstring> getClaimedLevelFolders(const TFilePath &exceptPath) {
  std::set<std::wstring> folders;
  QString exceptName = exceptPath.withoutParentDir().getQString();
  QDir dir(exceptPath.getParentDir().getQString());
  QFileInfoList files =
      dir.entryInfoList(QStringList() << "*.tnz", QDir::Files);
  for (const QFileInfo &file : files) {
    if (file.fileName().compare(exceptName, Qt::CaseInsensitive) == 0) continue;
    TFilePath path(file.absoluteFilePath());
    folders.insert(LibraryItems::foldCase(getUsedLevelFolder(path)));
  }
  return folders;
}

//-----------------------------------------------------------------------------

bool removeFileIfAny(const TFilePath &path) {
  QFile file(path.getQString());
  return !file.exists() || file.remove();
}

//-----------------------------------------------------------------------------

// Renames a file following an item (an icon), replacing a stale file with the
// new name unless the two names are the same file.
void renameFileIfAny(const TFilePath &src, const TFilePath &dst, bool replace) {
  if (!QFile::exists(src.getQString())) return;
  if (replace) QFile::remove(dst.getQString());
  QFile::rename(src.getQString(), dst.getQString());
}

//-----------------------------------------------------------------------------

void writeItemInfoFile(const TFilePath &itemPath,
                       const LibraryCmd::ItemInfo &info) {
  QFile file(LibraryCmd::getItemInfoPath(itemPath).getQString());
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;

  QXmlStreamWriter xml(&file);
  xml.setAutoFormatting(true);
  xml.writeStartDocument();
  xml.writeStartElement("libraryItem");
  xml.writeAttribute("version", "1");
  xml.writeTextElement("id", info.m_id);
  xml.writeTextElement("revision", QString::number(info.m_revision));
  xml.writeTextElement("kind", info.m_kind);
  xml.writeTextElement("sourceScene", info.m_sourceScene);
  if (!info.m_levelFolder.isEmpty())
    xml.writeTextElement("levelFolder", info.m_levelFolder);
  xml.writeEndElement();
  xml.writeEndDocument();
  file.close();
}

//-----------------------------------------------------------------------------

LibraryCmd::ItemInfo newItemInfo(const char *kind) {
  LibraryCmd::ItemInfo info;
  info.m_id       = QUuid::createUuid().toString(QUuid::WithoutBraces);
  info.m_revision = 1;
  info.m_kind     = QString::fromLatin1(kind);
  return info;
}

//-----------------------------------------------------------------------------

bool writePoseSet(const TFilePath &path, const PoseLibrary &poses) {
  try {
    if (!TSystem::touchParentDir(path)) return false;
    TOStream os(path);
    if (!os) return false;
    std::map<std::string, std::string> attributes;
    attributes["version"] = "1";
    os.openChild(PoseSetTag, attributes);
    poses.saveData(os);
    os.closeChild();
    if (!os.checkStatus()) return false;
  } catch (...) {
    return false;
  }
  return TFileStatus(path).doesExist();
}

//-----------------------------------------------------------------------------

// The sub-xsheet exposed by the column, or 0.
TXshChildLevel *getColumnSymbol(TXsheet *xsh, int col) {
  if (col < 0) return 0;
  TXshColumn *column         = xsh->getColumn(col);
  TXshCellColumn *cellColumn = column ? column->getCellColumn() : 0;
  int r0 = 0, r1 = -1;
  if (!cellColumn || !cellColumn->getRange(r0, r1)) return 0;
  for (int r = r0; r <= r1; r++)
    if (TXshChildLevel *childLevel = cellColumn->getCell(r).getChildLevel())
      return childLevel;
  return 0;
}

//=============================================================================
// SaveSymbolToLibraryCommand
//-----------------------------------------------------------------------------

class SaveSymbolToLibraryCommand final : public MenuItemHandler {
public:
  SaveSymbolToLibraryCommand() : MenuItemHandler(MI_SaveSymbolToLibrary) {}

  void execute() override {
    TXshChildLevel *symbol = LibraryCmd::getCommandSymbol();
    if (!symbol) {
      DVGui::warning(
          QObject::tr("Select a sub-xsheet column, or a symbol in the Symbols "
                      "panel, to save it to the Library."));
      return;
    }
    LibraryCmd::saveSymbolToLibrary(symbol);
  }
} saveSymbolToLibraryCommand;

}  // namespace

//*****************************************************************************
//    LibraryCmd
//*****************************************************************************

TFilePath LibraryCmd::getTemplatesFolder() {
  return ToonzFolder::getLibraryFolder() + "templates";
}

//-----------------------------------------------------------------------------

TFilePath LibraryCmd::getPosesFolder() {
  return ToonzFolder::getLibraryFolder() + "poses";
}

//-----------------------------------------------------------------------------

TFilePath LibraryCmd::getItemInfoPath(const TFilePath &itemPath) {
  return itemPath.getParentDir() +
         TFilePath(getItemName(itemPath) + ".libinfo");
}

//-----------------------------------------------------------------------------

bool LibraryCmd::readItemInfo(const TFilePath &itemPath, ItemInfo &info) {
  info = ItemInfo();
  QFile file(getItemInfoPath(itemPath).getQString());
  if (!file.open(QIODevice::ReadOnly)) return false;

  QXmlStreamReader xml(&file);
  if (!xml.readNextStartElement() ||
      xml.name().toString() != QString("libraryItem"))
    return false;
  while (xml.readNextStartElement()) {
    const QString tag = xml.name().toString();
    if (tag == "id")
      info.m_id = xml.readElementText().trimmed();
    else if (tag == "revision")
      info.m_revision = xml.readElementText().trimmed().toInt();
    else if (tag == "kind")
      info.m_kind = xml.readElementText().trimmed();
    else if (tag == "sourceScene")
      info.m_sourceScene = xml.readElementText();
    else if (tag == "levelFolder")
      info.m_levelFolder = xml.readElementText();
    else
      xml.skipCurrentElement();
  }
  if (xml.hasError() || info.m_id.isEmpty()) {
    info = ItemInfo();
    return false;
  }
  if (info.m_revision < 1) info.m_revision = 1;
  return true;
}

//-----------------------------------------------------------------------------

void LibraryCmd::writeItemInfo(const TFilePath &itemPath, const char *kind,
                               bool overwritten,
                               const std::wstring &levelFolder) {
  ItemInfo info;
  if (overwritten && readItemInfo(itemPath, info))
    info.m_revision++;
  else
    info = newItemInfo(kind);
  info.m_kind        = QString::fromLatin1(kind);
  info.m_sourceScene = getSourceSceneName();
  info.m_levelFolder = QString::fromStdWString(levelFolder);
  writeItemInfoFile(itemPath, info);
}

//-----------------------------------------------------------------------------

std::wstring LibraryCmd::getTemplateLevelFolder(const TFilePath &templatePath,
                                                const std::wstring &name) {
  std::wstring folder = name;
  ItemInfo info;
  if (TFileStatus(templatePath).doesExist() &&
      readItemInfo(templatePath, info) &&
      LibraryItems::isSafeFileName(info.m_levelFolder.toStdWString()))
    folder = info.m_levelFolder.toStdWString();

  // A renamed template keeps using the folder named after its old name
  std::set<std::wstring> claimed = getClaimedLevelFolders(templatePath);
  if (claimed.count(LibraryItems::foldCase(folder)) == 0) return folder;

  TFilePath templatesFolder = templatePath.getParentDir();
  std::wstring unique       = LibraryItems::uniqueName(
      name, [&claimed, &templatesFolder](const std::wstring &candidate) {
        return claimed.count(LibraryItems::foldCase(candidate)) > 0 ||
               TFileStatus(templatesFolder + candidate).doesExist();
      });
  return unique.empty() ? folder : unique;
}

//-----------------------------------------------------------------------------

QString LibraryCmd::sanitizeName(const QString &name) {
  return QString::fromStdWString(
      LibraryItems::sanitizeFileName(name.toStdWString()));
}

//-----------------------------------------------------------------------------

bool LibraryCmd::askItemName(const QString &title, const QString &defaultName,
                             const TFilePath &folder, const QString &extension,
                             QString &name) {
  bool ok       = false;
  QString typed = DVGui::getText(title, QObject::tr("Name:"), defaultName, &ok);
  if (!ok) return false;
  name = sanitizeName(typed);
  if (name.isEmpty()) {
    DVGui::warning(invalidNameMessage());
    return false;
  }
  if (!TFileStatus(getItemPath(folder, name, extension)).doesExist())
    return true;

  QString question =
      QObject::tr(
          "The Library already has an item named %1.\nDo you want to "
          "overwrite it, or to keep both and save this one with a new name?")
          .arg(name);
  int ret = DVGui::MsgBox(question, QObject::tr("Overwrite"),
                          QObject::tr("Keep Both"), QObject::tr("Cancel"), 0);
  if (ret == 1) return true;
  if (ret != 2) return false;

  // a template also needs a free folder for its level files
  bool withFolder = extension == "tnz";
  std::wstring unique =
      LibraryItems::uniqueName(name.toStdWString(), [&](const std::wstring &n) {
        QString candidate = QString::fromStdWString(n);
        return TFileStatus(getItemPath(folder, candidate, extension))
                   .doesExist() ||
               (withFolder && TFileStatus(folder + n).doesExist());
      });
  name = QString::fromStdWString(unique);
  return !name.isEmpty();
}

//-----------------------------------------------------------------------------

TXshChildLevel *LibraryCmd::getCommandSymbol() {
  TApp *app    = TApp::instance();
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  if (!xsh) return 0;

  if (TColumnSelection *selection = dynamic_cast<TColumnSelection *>(
          app->getCurrentSelection()->getSelection())) {
    for (int index : selection->getIndices())
      if (TXshChildLevel *symbol = getColumnSymbol(xsh, index)) return symbol;
  }

  if (TXshChildLevel *symbol = SymbolsPane::getPanelSymbol()) return symbol;

  int row = app->getCurrentFrame()->getFrame();
  int col = app->getCurrentColumn()->getColumnIndex();
  if (row < 0 || col < 0) return 0;
  return xsh->getCell(row, col).getChildLevel();
}

//-----------------------------------------------------------------------------

bool LibraryCmd::saveSymbolToLibrary(TXshChildLevel *symbol) {
  if (!symbol || !symbol->getXsheet()) return false;
  // the scene may be saved first: keep the symbol meanwhile
  TXshChildLevelP symbolHolder(symbol);
  TXsheet *childXsh  = symbol->getXsheet();
  QString symbolName = QString::fromStdWString(symbol->getName());
  if (childXsh->getFrameCount() <= 0) {
    DVGui::warning(QObject::tr("The symbol %1 is empty.").arg(symbolName));
    return false;
  }

  QString defaultName = sanitizeName(symbolName);
  if (defaultName.isEmpty()) defaultName = QObject::tr("Symbol");
  QString name;
  if (!askItemName(QObject::tr("Save Symbol to Library"), defaultName,
                   getTemplatesFolder(), "tnz", name))
    return false;

  return SubsceneCmd::saveXsheetAsTemplate(childXsh, name, SymbolKind);
}

//-----------------------------------------------------------------------------

bool LibraryCmd::readPoseSet(const TFilePath &path, PoseLibrary &poses) {
  poses.clear();
  try {
    TIStream is(path);
    if (!is) return false;
    std::string tagName;
    if (!is.matchTag(tagName) || tagName != PoseSetTag) return false;
    poses.loadData(is);
    if (!is.matchEndTag()) {
      poses.clear();
      return false;
    }
  } catch (...) {
    poses.clear();
    return false;
  }
  return true;
}

//-----------------------------------------------------------------------------

bool LibraryCmd::savePoseSet() {
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  if (!scene) return false;
  PoseLibrary *poseLibrary = scene->getPoseLibrary();
  if (poseLibrary->getPoseCount() == 0) {
    DVGui::warning(
        QObject::tr("The scene has no poses to save. Create them in the Poses "
                    "panel."));
    return false;
  }

  QString defaultName = getSourceSceneName();
  defaultName         = sanitizeName(defaultName);
  if (defaultName.isEmpty()) defaultName = QObject::tr("Poses");

  QString extension = QString::fromLatin1(PoseSetExtension);
  TFilePath folder  = getPosesFolder();
  QString name;
  if (!askItemName(QObject::tr("Save Pose Set to Library"), defaultName, folder,
                   extension, name))
    return false;

  TFilePath path   = getItemPath(folder, name, extension);
  bool overwritten = TFileStatus(path).doesExist();
  if (!writePoseSet(path, *poseLibrary)) {
    DVGui::warning(QObject::tr("Couldn't save %1").arg(toQString(path)));
    return false;
  }
  writeItemInfo(path, PoseSetKind, overwritten);
  FileBrowser::refreshFolder(folder);

  DVGui::info(QObject::tr("The %1 poses of the scene have been saved to %2.")
                  .arg(poseLibrary->getPoseCount())
                  .arg(toQString(path)));
  return true;
}

//-----------------------------------------------------------------------------

bool LibraryCmd::loadPoseSet(const TFilePath &path) {
  TApp *app         = TApp::instance();
  ToonzScene *scene = app->getCurrentScene()->getScene();
  if (!scene) return false;

  PoseLibrary incoming;
  if (!readPoseSet(path, incoming)) {
    DVGui::warning(
        QObject::tr("The pose set %1 could not be read.").arg(toQString(path)));
    return false;
  }
  if (incoming.getPoseCount() == 0) {
    DVGui::warning(
        QObject::tr("The pose set %1 has no poses.").arg(getItemName(path)));
    return false;
  }

  PoseLibrary *poseLibrary = scene->getPoseLibrary();
  std::vector<PoseLibrary::Pose> existingPoses, incomingPoses;
  std::vector<std::wstring> existingNames, incomingNames;
  for (int i = 0; i < poseLibrary->getPoseCount(); i++) {
    existingPoses.push_back(poseLibrary->getPose(i));
    existingNames.push_back(poseLibrary->getPose(i).m_name.toStdWString());
  }
  for (int i = 0; i < incoming.getPoseCount(); i++) {
    incomingPoses.push_back(incoming.getPose(i));
    incomingNames.push_back(incoming.getPose(i).m_name.toStdWString());
  }

  std::vector<int> pairs =
      LibraryItems::pairByName(existingNames, incomingNames);
  int pairedCount = LibraryItems::countPaired(pairs);

  // The pose library has no undo: ask before replacing poses
  bool replace = true;
  if (pairedCount > 0) {
    QStringList names;
    for (int i = 0; i < (int)pairs.size(); i++) {
      QString name = incomingPoses[i].m_name;
      if (pairs[i] >= 0 && !names.contains(name)) names.append(name);
    }
    QString question =
        QObject::tr(
            "These poses of the set have the same names as poses of the "
            "scene:\n%1\n\nDo you want to replace the poses of the scene with "
            "them? This cannot be undone.")
            .arg(names.join(", "));
    int ret = DVGui::MsgBox(question, QObject::tr("Replace"),
                            QObject::tr("Keep the Scene Poses"),
                            QObject::tr("Cancel"), 0);
    if (ret == 1)
      replace = true;
    else if (ret == 2)
      replace = false;
    else
      return false;
  }

  std::vector<PoseLibrary::Pose> merged =
      LibraryItems::mergeByName(existingPoses, incomingPoses, pairs, replace);
  // The poses are offsets from the base pose: a scene without one takes the
  // base of the set, so that new poses can be added to it.
  std::vector<PoseLibrary::Entry> base = poseLibrary->getBase();
  if (base.empty()) base = incoming.getBase();

  poseLibrary->clear();
  poseLibrary->setBase(base);
  for (const PoseLibrary::Pose &pose : merged) poseLibrary->addPose(pose);

  app->getCurrentScene()->setDirtyFlag(true);
  PoseLibraryNotifier::instance()->notifyChanged();

  int addedCount = (int)incomingPoses.size() - pairedCount;
  DVGui::info(QObject::tr("Pose set %1: %2 poses added, %3 replaced, %4 kept.")
                  .arg(getItemName(path))
                  .arg(addedCount)
                  .arg(replace ? pairedCount : 0)
                  .arg(replace ? 0 : pairedCount));
  return true;
}

//-----------------------------------------------------------------------------

bool LibraryCmd::renameItem(const TFilePath &itemPath, const QString &newName) {
  QString name = sanitizeName(newName);
  if (name.isEmpty()) {
    DVGui::warning(invalidNameMessage());
    return false;
  }
  QString oldName = getItemName(itemPath);
  if (name == oldName) return true;
  if (!QFile::exists(itemPath.getQString())) {
    DVGui::warning(QObject::tr("The library item %1 does not exist anymore.")
                       .arg(oldName));
    return false;
  }

  bool templ        = isTemplate(itemPath);
  QString extension = QString::fromStdString(itemPath.getType());
  TFilePath folder  = itemPath.getParentDir();
  TFilePath newPath = getItemPath(folder, name, extension);
  // a change of case only renames the same file on some systems
  bool sameFile = LibraryItems::foldCase(name.toStdWString()) ==
                  LibraryItems::foldCase(oldName.toStdWString());
  if (!sameFile && QFile::exists(newPath.getQString())) {
    DVGui::warning(
        QObject::tr("The Library already has an item named %1.").arg(name));
    return false;
  }

  ItemInfo info;
  bool hasInfo = readItemInfo(itemPath, info);
  // The level files of a template stay where the template refers to them:
  // its .libinfo records that folder, so that no other template uses it.
  std::wstring levelFolder;
  if (templ) {
    levelFolder = getUsedLevelFolder(itemPath);
    if (!LibraryItems::isSafeFileName(levelFolder) ||
        !TFileStatus(folder + levelFolder).isDirectory())
      levelFolder.clear();
  }

  if (!QFile::rename(itemPath.getQString(), newPath.getQString())) {
    DVGui::warning(QObject::tr("Couldn't rename %1 to %2.")
                       .arg(toQString(itemPath))
                       .arg(toQString(newPath)));
    return false;
  }
  if (templ)
    renameFileIfAny(ToonzScene::getIconPath(itemPath),
                    ToonzScene::getIconPath(newPath), !sameFile);

  // The identity follows the item
  if (!hasInfo) info = newItemInfo(templ ? RigKind : PoseSetKind);
  if (templ) info.m_levelFolder = QString::fromStdWString(levelFolder);
  removeFileIfAny(getItemInfoPath(itemPath));
  writeItemInfoFile(newPath, info);

  FileBrowser::refreshFolder(folder);
  return true;
}

//-----------------------------------------------------------------------------

bool LibraryCmd::deleteItem(const TFilePath &itemPath) {
  bool templ = isTemplate(itemPath);
  std::wstring levelFolder;
  if (templ) levelFolder = getUsedLevelFolder(itemPath);

  if (!removeFileIfAny(itemPath)) {
    DVGui::warning(QObject::tr("Couldn't delete %1.").arg(toQString(itemPath)));
    return false;
  }
  removeFileIfAny(getItemInfoPath(itemPath));

  TFilePath folder = itemPath.getParentDir();
  if (templ) {
    removeFileIfAny(ToonzScene::getIconPath(itemPath));

    // The copied level files, unless another template uses them. Only a
    // plain folder name is accepted, so nothing outside the folder of the
    // template can be removed.
    std::set<std::wstring> claimed = getClaimedLevelFolders(itemPath);
    TFilePath levelDir             = folder + levelFolder;
    if (LibraryItems::isSafeFileName(levelFolder) &&
        claimed.count(LibraryItems::foldCase(levelFolder)) == 0 &&
        TFileStatus(levelDir).isDirectory()) {
      try {
        TSystem::rmDirTree(levelDir);
      } catch (...) {
        DVGui::warning(
            QObject::tr("The level files of the item could not be deleted "
                        "from %1.")
                .arg(toQString(levelDir)));
      }
    }
  }

  FileBrowser::refreshFolder(folder);
  return true;
}
