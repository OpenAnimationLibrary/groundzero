#pragma once

#include "tdoubleparam.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QStringList>

#include <map>
#include <memory>
#include <stdexcept>

class QApplication;
class TFx;
class TProject;
class TStageObject;
class TStageObjectSpline;
class ToonzScene;
class TXshColumn;
class TXshLevel;

namespace GroundzeroAutomation {

struct RenderState;

class Error final : public std::runtime_error {
public:
  Error(QString code, QString message, QJsonObject details = {});
  QString code;
  QString message;
  QJsonObject details;
};

// One explicitly granted shared root; read-only unless writable is granted.
struct SharedRootGrant final {
  QString path;
  bool writable = false;
};

// The shared root roles in their reporting order.
const QStringList& sharedRootRoles();

struct Document final {
  explicit Document(std::unique_ptr<ToonzScene> ownedScene);
  ~Document();
  Document(const Document&)            = delete;
  Document& operator=(const Document&) = delete;

  QString id;
  std::unique_ptr<ToonzScene> scene;
  quint64 revision = 0;
  QHash<QString, TXshLevel*> levels;
  QHash<QString, TXshColumn*> columns;
  QHash<QString, TFx*> effects;
  QHash<QString, TStageObject*> objects;
  QHash<QString, TStageObjectSpline*> paths;
  QHash<TStageObjectSpline*, QString> pathIds;
  // Verified file metadata of sound sources, keyed by their cast level.
  QHash<TXshLevel*, QJsonObject> soundSources;
  QHash<QString, TDoubleParamP> offsetLayers;

  QString revisionText() const;
  QString idForLevel(TXshLevel* level);
  QString idForColumn(TXshColumn* column);
  QString idForFx(TFx* fx);
  QString idForObject(TStageObject* object);
  TXshLevel* level(const QString& id) const;
  TXshColumn* column(const QString& id) const;
  TFx* fx(const QString& id) const;
  TStageObject* object(const QString& id) const;
};

class Context final {
public:
  Context(QString workspace, QStringList inputRoots, bool helpersContained = false);
  ~Context();
  Context(const Context&)            = delete;
  Context& operator=(const Context&) = delete;

  Document& document(const QString& id);
  Document& addDocument(std::unique_ptr<ToonzScene> scene);
  void removeDocument(const QString& id);
  QJsonArray documents() const;
  std::shared_ptr<TProject> project() const;
  QString workspace() const;
  QString resolvePath(const QString& path, bool write) const;
  void markMutationStarted();
  void resetMutationStarted();
  bool mutationStarted() const;
  bool helpersContained() const;
  std::shared_ptr<RenderState> renderState;
  // Set once from validated startup configuration; empty disables media tools.
  QString mediaToolsDirectory;
  // Set once from validated startup configuration, keyed by shared root role.
  QMap<QString, SharedRootGrant> sharedRoots;
  // The private per-process runtime directory inside the workspace.
  QString runtimeDirectory;

private:
  QString m_workspace;
  QStringList m_inputRoots;
  std::shared_ptr<TProject> m_project;
  std::map<QString, std::unique_ptr<Document>> m_documents;
  bool m_mutationStarted = false;
  const bool m_helpersContained;
};

// Catalog entries use MCP tool descriptors with strict inputSchema objects.
QJsonObject operation(const QString& name, const QString& description,
                      QJsonObject properties, QJsonArray required,
                      bool writes = false);
QString requiredString(const QJsonObject& params, const QString& name);
int integerParam(const QJsonObject& params, const QString& name, int minimum,
                 int maximum, int fallback);

QJsonArray sceneOperations();
QJsonArray fxOperations();
QJsonArray drawingOperations();
QJsonArray renderOperations();
QJsonArray persistenceOperations();
QJsonValue executeScene(Context& context, const QString& method,
                        const QJsonObject& params);
QJsonValue executeFx(Context& context, const QString& method,
                     const QJsonObject& params);
QJsonValue executeDrawing(Context& context, const QString& method,
                          const QJsonObject& params);
QJsonValue executeRender(Context& context, const QString& method,
                         const QJsonObject& params);
QJsonValue executePersistence(Context& context, const QString& method,
                              const QJsonObject& params);
// Request cancellation on the application thread without waiting for callbacks.
void requestRenderCancellation(Context& context);
void shutdownRender(Context& context);
std::unique_ptr<ToonzScene> createRenderSnapshot(Context& context,
                                                 Document& document,
                                                 const QString& directory);
void validateAutomationScene(ToonzScene& scene);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationFxTests(Context& context);
void runAutomationFxAuthoringTests(Context& context);
void runAutomationColorCompositeTests(Context& context);
void runAutomationGroupTests(Context& context);
void runAutomationMediaProcessTests(Context& context);
#endif

// Worker dispatch and protocol I/O run on the QApplication thread.
int runWorker(int argc, char* argv[]);

}  // namespace GroundzeroAutomation
