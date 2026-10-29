#pragma once

#ifndef LIBRARYCOMMAND_H
#define LIBRARYCOMMAND_H

#include "tfilepath.h"

#include <QString>

#include <string>

class TXshChildLevel;
class PoseLibrary;

//=============================================================================
// The shared Library
//-----------------------------------------------------------------------------
// Items stored outside the scenes, in the Library folder (see
// ToonzFolder::getLibraryFolder()), and shown by the Library panel
// (riglibrarypane.h), from which they are inserted in any scene:
//  - rigs and symbols: scenes in "templates", saved by "Save Selection as
//    Template" and "Save Symbol to Library" with copies of their level files
//    in a folder next to them (see SubsceneCmd::saveXsheetAsTemplate());
//  - pose sets: "poses/<name>.tposes", the poses of a scene.
// Each item has a "<name>.libinfo" file next to it with its identity: a
// random id, a revision counted from 1, its kind and the name of the scene
// it comes from. Nothing about the Library is written into the scenes.
//-----------------------------------------------------------------------------

namespace LibraryCmd {

//! The kinds of library items written in their .libinfo files.
const char RigKind[]     = "rig";
const char SymbolKind[]  = "symbol";
const char PoseSetKind[] = "poseSet";

//! The extension of the pose set files.
const char PoseSetExtension[] = "tposes";

//! The folder of the rigs and symbols, "Library/templates".
TFilePath getTemplatesFolder();
//! The folder of the pose sets, "Library/poses".
TFilePath getPosesFolder();

//! The identity of a library item, read from its .libinfo file.
struct ItemInfo {
  QString m_id;           //!< random, kept when the item is overwritten
  int m_revision;         //!< 1, then incremented at each overwrite
  QString m_kind;         //!< RigKind, SymbolKind or PoseSetKind
  QString m_sourceScene;  //!< the name of the scene it was saved from
  QString m_levelFolder;  //!< templates: the folder of their level files

  ItemInfo() : m_revision(0) {}
};

//! Returns the path of the .libinfo file of the item.
TFilePath getItemInfoPath(const TFilePath &itemPath);
//! Reads the .libinfo file of the item. Returns false if there is none or it
//! cannot be read.
bool readItemInfo(const TFilePath &itemPath, ItemInfo &info);
//! Writes the .libinfo file of an item that has just been saved: a new id
//! and revision 1, or, if an existing item was overwritten, its id and the
//! next revision. Failures are ignored: the item works without it.
void writeItemInfo(const TFilePath &itemPath, const char *kind,
                   bool overwritten,
                   const std::wstring &levelFolder = std::wstring());

//! Returns the name of the folder, next to the template, receiving the
//! copies of its level files: the one the template already uses when it is
//! overwritten, otherwise \b name, unless another template uses that folder
//! (after a rename), in which case a free "name_N" is chosen.
std::wstring getTemplateLevelFolder(const TFilePath &templatePath,
                                    const std::wstring &name);

//! Returns the name made safe as a file name (see LibraryItems).
QString sanitizeName(const QString &name);

//! Asks the name of a new item of the folder, proposing \b defaultName. If
//! an item with that name exists, asks whether to overwrite it or to keep
//! both, choosing then a free "name_N". Returns false if cancelled.
bool askItemName(const QString &title, const QString &defaultName,
                 const TFilePath &folder, const QString &extension,
                 QString &name);

//! The symbol "Save Symbol to Library" works on: the sub-xsheet of the first
//! selected column exposing one, otherwise the current symbol of the Symbols
//! panel, otherwise the sub-xsheet of the current cell. 0 if there is none.
TXshChildLevel *getCommandSymbol();

//! Saves the sub-xsheet of the symbol as a template of the Library, after
//! asking its name. Returns true if it was saved.
bool saveSymbolToLibrary(TXshChildLevel *symbol);

//! Reads a pose set file. Returns false if it cannot be read.
bool readPoseSet(const TFilePath &path, PoseLibrary &poses);

//! Saves all the poses of the current scene (and its base pose) as a pose set
//! of the Library, after asking its name. Returns true if it was saved.
bool savePoseSet();

//! Merges the poses of the pose set into the pose library of the current
//! scene. The poses having the name of a pose of the scene replace it, after
//! asking. The pose library has no undo, so neither has this. Returns true if
//! the scene poses changed.
bool loadPoseSet(const TFilePath &path);

//! Renames the item (a template or a pose set) and its .libinfo and icon
//! files. The level files of a template stay in their folder, which its
//! .libinfo records. Shows the errors. Returns true if renamed.
bool renameItem(const TFilePath &itemPath, const QString &newName);

//! Deletes the item, its .libinfo and icon files and, for a template, the
//! folder of its level files if no other template uses it. The caller asks
//! for confirmation. Shows the errors. Returns true if deleted.
bool deleteItem(const TFilePath &itemPath);

}  // namespace LibraryCmd

#endif  // LIBRARYCOMMAND_H
