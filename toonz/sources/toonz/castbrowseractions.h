#pragma once
#include "toonz/castworkflow.h"
#include <QString>
class QWidget;
class ToonzScene;

namespace CastBrowserActions {
// Stable values are used by native controls and acceptance fixtures.
enum StatusFilter { All = 0, Used = 1, Unused = 2, Absent = 3, External = 4,
                    Modified = 5, Unsaved = 6, Missing = 7, Cached = 8 };
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
