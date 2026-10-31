#pragma once
#include "toonz/castworkflow.h"
#include <QString>
class QWidget;
class ToonzScene;

namespace CastBrowserActions {
// Stable values are used by native controls and acceptance fixtures.
enum StatusFilter { All = 0, Used = 1, Unused = 2, Absent = 3, External = 4,
                    Modified = 5, Unsaved = 6, Missing = 7, Cached = 8 };

// These snapshots live for one refresh only. File, cache and dirty state are
// deliberately re-read on the next refresh, including Refresh Disk Status.
struct StatusQueries {
  quint64 assets = 0, decodedPaths = 0, diskQueries = 0;
  quint64 canonicalQueries = 0, rootSets = 0, cacheQueries = 0;
  quint64 libraryQueries = 0;
};
class StatusContext {
  friend class AssetStatus;
  ToonzScene *m_scene;
  QStringList m_roots;
  bool m_rootsReady = false;
  const QStringList &roots();
public:
  StatusQueries queries;
  explicit StatusContext(ToonzScene *scene) : m_scene(scene) {}
};
class AssetStatus {
  StatusContext &m_context;
  TXshLevel *m_level;
  TFilePath m_path;
  bool m_pathReady = false;
  int m_missing = -1, m_external = -1, m_modified = -1, m_cached = -1;
public:
  AssetStatus(StatusContext &context, TXshLevel *level)
      : m_context(context), m_level(level) { ++m_context.queries.assets; }
  const TFilePath &path();
  bool missing();
  bool external();
  bool modified();
  bool cached();
  bool unsaved();
  QString text();
  QChar badge();
};
bool modified(TXshLevel *level);
bool cached(TXshLevel *level);
bool unsaved(ToonzScene *scene, TXshLevel *level);
bool external(ToonzScene *scene, TXshLevel *level);
QString status(ToonzScene *scene, TXshLevel *level);
bool missing(ToonzScene *scene, TXshLevel *level);
void whereUsed(QWidget *parent, const std::vector<TXshLevel *> &levels);
void inspect(QWidget *parent, const std::vector<TXshLevel *> &levels);
void editTags(QWidget *parent, const std::vector<TXshLevel *> &levels);
void removeUnused(QWidget *parent);
void relink(QWidget *parent, const std::vector<TXshLevel *> &levels);
void makeLocal(QWidget *parent, const std::vector<TXshLevel *> &levels);
void duplicates(QWidget *parent);
void collectionReport(QWidget *parent);
void verifyCollectedScene(QWidget *parent, const TFilePath &path, const TFilePath &root);
}
