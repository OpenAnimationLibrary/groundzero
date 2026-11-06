#pragma once

#ifndef TXSHLEVEL_INCLUDED
#define TXSHLEVEL_INCLUDED

#include "tfilepath.h"
#include "tpersist.h"
#include "tsmartpointer.h"
#include <QObject>
#include <QStringList>
#include <map>

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

//=============================================================================
// forward declarations
class TXshSimpleLevel;
class TXshChildLevel;
class TXshZeraryFxLevel;
class TXshPaletteLevel;
class HookSet;
class TrackerObjectsSet;
class ToonzScene;
class TXshSoundLevel;
class TXshSoundTextLevel;

//=============================================================================
//! The TXshLevel class is the base class of level managers in xsheet.
/*!Inherits \b TSmartObject and \b TPersist.
\n This is an abstract base class inherited by the concrete classes
   \b TXshChildLevel, \b TXshSimpleLevel and \b TXshZeraryFxLevel.

   The class contains all features about a generic level and gives all methods
   to access to these information.

   It's possible to know level name, getName(), or identify level types
getType(),
   defined in \b txshleveltypes.h, and change it, setType().
   The class provides function to know scene to which level belongs, getScene(),
   and function to change scene setScene().
*/
//=============================================================================

class DVAPI TXshLevel : public QObject, public TSmartObject, public TPersist {
  Q_OBJECT
protected:
  std::wstring m_name;
  std::wstring m_shortName;

  /*!
Identify level types defined in \b txshleveltypes.h
*/
  int m_type;
  HookSet *m_hookSet;
  ToonzScene *m_scene;
  QStringList m_castTags;
  TFrameId m_castCover;
  bool m_castCoverSet = false;

  //! create shortname from the level name
  void updateShortName();

public:
  struct LibraryProvenance {
    enum Relationship { NotRecorded, Linked, LocalCopy, Snapshot, Replaced };
    QString itemId, itemPath, kind, sourceAssetPath;
    int revision = 0;
    Relationship relationship = NotRecorded;
    bool operator==(const LibraryProvenance &other) const {
      return itemId == other.itemId && itemPath == other.itemPath &&
             kind == other.kind && sourceAssetPath == other.sourceAssetPath &&
             revision == other.revision && relationship == other.relationship;
    }
    bool operator!=(const LibraryProvenance &other) const { return !(*this == other); }
  };

private:
  QString m_castId;
  LibraryProvenance m_libraryProvenance;
  bool m_sourceExpectedOnDisk = false;

public:
  /*!
Constructs a TXshLevel with \b TXshLevel name \b name
*/
  TXshLevel(ClassCode classCode, std::wstring name);

  /*!
Destroys the TXshLevel object.
*/
  virtual ~TXshLevel();

  void setName(std::wstring name);
  const QStringList &getCastTags() const { return m_castTags; }
  void setCastTags(const QStringList &tags) { m_castTags = tags; }
  bool hasCastCover() const { return m_castCoverSet; }
  TFrameId getCastCover() const { return m_castCover; }
  void setCastCover(const TFrameId &frame) { m_castCover = frame; m_castCoverSet = true; }
  void clearCastCover() { m_castCoverSet = false; }
  const QString &getCastId() const { return m_castId; }
  //! Distinct imports get a new identity; undo/redo and relocation retain it.
  void renewCastId();
  bool sourceExpectedOnDisk() const { return m_sourceExpectedOnDisk; }
  void setSourceExpectedOnDisk(bool expected) { m_sourceExpectedOnDisk = expected; }
  const LibraryProvenance &getLibraryProvenance() const { return m_libraryProvenance; }
  void setLibraryProvenance(const LibraryProvenance &provenance) { m_libraryProvenance = provenance; }
  //! A recorded link is current only while the asset still addresses its recorded source.
  LibraryProvenance::Relationship getLibraryRelationship() const;
  void detachLibrarySource(LibraryProvenance::Relationship relationship);
  void copyCastMetadataFrom(const TXshLevel *source) {
    m_castTags = source->m_castTags;
    m_castCover = source->m_castCover;
    m_castCoverSet = source->m_castCoverSet;
    m_libraryProvenance = source->m_libraryProvenance;
    if (source->getLibraryRelationship() == LibraryProvenance::Replaced)
      m_libraryProvenance.relationship = LibraryProvenance::Replaced;
    // Identity and save state belong to the destination, not its source.
  }
  void loadCastTags(TIStream &is);
  std::map<std::string, std::string> getCastAttributes() const;
  void saveCastPath(TOStream &os, const TFilePath &path) const;

  //! return the level type (types are defined in \b txshleveltypes.h); \sa
  //! setType()
  int getType() const { return m_type; }

  //! change the level type; \sa getType()
  void setType(int type) { m_type = type; }

  //! Return the \b ToonzScene the level belongs to; \sa setScene()
  ToonzScene *getScene() const { return m_scene; }
  /*!
Set scene to which level belongs to \b scene.
\sa getScene()
*/
  virtual void setScene(ToonzScene *scene);

  //! Method (to be overridden in subclasses) to downcast to \b TXshSimpleLevel
  virtual TXshSimpleLevel *getSimpleLevel() { return 0; }

  //! Method (to be overridden in subclasses) to downcast to \b TXshChildLevel
  virtual TXshChildLevel *getChildLevel() { return 0; }

  //! Method (to be overridden in subclasses) to downcast to \b
  //! TXshZeraryFxLevel
  virtual TXshZeraryFxLevel *getZeraryFxLevel() { return 0; }

  //! Method (to be overridden in subclasses) to downcast to \b TXshPaletteLevel
  virtual TXshPaletteLevel *getPaletteLevel() { return 0; }

  //! Method (to be overridden in subclasses) to downcast to \b TXshSoundLevel
  virtual TXshSoundLevel *getSoundLevel() { return 0; }

  //! Method (to be overridden in subclasses) to downcast to \b
  //! TXshSoundTextLevel
  virtual TXshSoundTextLevel *getSoundTextLevel() { return 0; }

  //! Return level name. \sa getShortName()
  std::wstring getName() const { return m_name; }

  //! Return level short name. \sa getName()
  std::wstring getShortName() const { return m_shortName; }

  //! Return the file path associated with the level
  virtual TFilePath getPath() const { return TFilePath(); }

  virtual bool isEmpty() const { return true; }
  virtual int getFrameCount() const { return 0; }
  virtual void getFids(std::vector<TFrameId> &fids) const {}

  //! Load the level data (i.e. name, path, etc. e.g. from the .tnz file)
  void loadData(TIStream &is) override = 0;
  //! Save the level data (i.e. name, path, etc. e.g. to the .tnz file)
  void saveData(TOStream &os) override = 0;

  //! Load the level content from the associated path
  virtual void load() = 0;

  //! Save the level content to the associated path
  virtual void save() = 0;

  /*!
Return \b HookSet.
*/
  HookSet *getHookSet() const { return m_hookSet; }

  //! Mark the level as "modified"
  virtual void setDirtyFlag(bool on) {}

private:
  // not implemented
  TXshLevel(const TXshLevel &);
  TXshLevel &operator=(const TXshLevel &);
};

#ifdef _WIN32
template class DV_EXPORT_API TSmartPointerT<TXshLevel>;
#endif

typedef TSmartPointerT<TXshLevel> TXshLevelP;

#endif
