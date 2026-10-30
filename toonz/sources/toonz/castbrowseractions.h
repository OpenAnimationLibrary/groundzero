#pragma once
#include "toonz/castworkflow.h"
#include <QString>
class QWidget;
class ToonzScene;

namespace CastBrowserActions {
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
