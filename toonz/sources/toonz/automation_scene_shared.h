#pragma once

#include "automation.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <map>

class TFrameId;
class TStageObjectTree;
class ToonzScene;

namespace GroundzeroAutomation {

/// Helpers shared by the scene, level, column, cell and camera tool files.
///
/// Each of those files brings these names in with `using namespace
/// SceneShared;`, so a handler reads the same in every section.
namespace SceneShared {

constexpr int MaxFrame     = 1000000;
constexpr int MaxSpan      = 10000;
constexpr int MaxEntities  = 4096;
constexpr qint64 MaxPixels = 16777216;

using SceneHandler  = QJsonValue (*)(Context&, const QJsonObject&);
using SceneHandlers = std::map<QString, SceneHandler>;

QJsonObject textSchema();
QJsonObject numberSchema(double minimum, double maximum);
QJsonObject integerSchema(int minimum, int maximum);
QJsonObject enumSchema(const QJsonArray& values);
QJsonObject documentSchema();
QJsonObject merge(QJsonObject target, const QJsonObject& source);
QJsonObject cameraSchema();

double numberParam(const QJsonObject& params, const QString& key,
                   double minimum, double maximum, double fallback);
bool boolParam(const QJsonObject& params, const QString& key, bool fallback);
QString nameParam(const QJsonObject& params);

Document& getDocument(Context& context, const QJsonObject& params);
TStageObjectTree* tree(Document& document);
int columnIndex(Document& document, TXshColumn* column);
void requireEditable(Document& document, TStageObject* object);
QJsonObject drawingId(const TFrameId& frame);
TFrameId readDrawingId(const QJsonObject& params);

/// Describe a level; defined with the level tools.
QJsonObject describeLevel(Document& document, TXshLevel* level,
                          bool includeDrawings);
/// Describe a column; defined with the column tools.
QJsonObject describeColumn(Document& document, TXshColumn* column);
/// Describe the current camera; defined with the camera tools.
QJsonObject describeCamera(Document& document);
/// Apply scene.settings; defined with the scene tools, used by camera.update.
QJsonValue updateSettings(Context& context, const QJsonObject& params);

void addCameraOperations(QJsonArray& catalog);
void addCameraHandlers(SceneHandlers& handlers);
void addCameraSheetOperations(QJsonArray& catalog);
void addCameraSheetHandlers(SceneHandlers& handlers);
void addLevelOperations(QJsonArray& catalog);
void addLevelHandlers(SceneHandlers& handlers);
void addColumnOperations(QJsonArray& catalog);
void addColumnHandlers(SceneHandlers& handlers);
void addColumnSheetOperations(QJsonArray& catalog);
void addColumnSheetHandlers(SceneHandlers& handlers);
void addCellOperations(QJsonArray& catalog);
void addCellHandlers(SceneHandlers& handlers);
void addCellToolOperations(QJsonArray& catalog);
void addCellToolHandlers(SceneHandlers& handlers);
void addOutputOperations(QJsonArray& catalog);
void addOutputHandlers(SceneHandlers& handlers);

/// Schema of an explicit {r, g, b, a} scene background color.
QJsonObject backgroundColorSchema();
/// Set the scene background from params.background_color, when given.
void applyBackgroundColor(ToonzScene& scene, const QJsonObject& params);
/// Whether params leave the scene background as it is.
bool sameBackgroundColor(ToonzScene& scene, const QJsonObject& params);

}  // namespace SceneShared
}  // namespace GroundzeroAutomation
