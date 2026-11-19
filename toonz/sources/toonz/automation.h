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

struct ReadOnlyScene;
class JobRegistry;
class Journal;
struct SceneHostRegistry;
struct RuntimeState;
struct SelectionRegistry;

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

// What a session may do, fixed at launch: its grant classes and the toolsets
// it may use, both sorted. inspect and core are always granted.
struct Authority final {
  QStringList grants;
  QStringList domains;
};

struct Document final {
  explicit Document(std::unique_ptr<ToonzScene> ownedScene);
  // A read-only document under an id its worker chose; no profile setup.
  Document(std::unique_ptr<ToonzScene> ownedScene, QString documentId,
           std::shared_ptr<ReadOnlyScene> source);
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
  // Set only for read-only documents opened from user scenes.
  std::shared_ptr<ReadOnlyScene> readOnly;
  // The project a draft was created in (scene.create project_id): its
  // identity only; the draft's native project stays the worker's private one.
  QJsonObject project;
  // Selection handles issued for this document's drawings.
  std::shared_ptr<SelectionRegistry> selections;

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
  // privateRoot, inside the workspace, holds the worker project and the copies
  // of level and sound files it writes; it defaults to the workspace itself.
  Context(QString workspace, QStringList inputRoots,
          bool helpersContained = false, QString privateRoot = QString());
  ~Context();
  Context(const Context&)            = delete;
  Context& operator=(const Context&) = delete;

  Document& document(const QString& id);
  // document_not_found details for an id that names no document here.
  QJsonObject missingDocument(const QString& id) const;
  /// Refuse with resource_limit while the worker holds its maximum documents.
  void requireDocumentCapacity() const;
  Document& addDocument(std::unique_ptr<ToonzScene> scene);
  // Register a read-only document that keeps its own project binding.
  Document& addReadOnlyDocument(std::unique_ptr<ToonzScene> scene,
                                const QString& id,
                                std::shared_ptr<ReadOnlyScene> source);
  void removeDocument(const QString& id);
  QJsonArray documents() const;
  std::shared_ptr<TProject> project() const;
  QString workspace() const;
  // Where short-lived scratch folders go: the worker's private runtime
  // directory, which a later worker sweeps if this one dies. A context without
  // a worker (a unit test) uses its workspace.
  QString scratchDirectory() const {
    return runtimeDirectory.isEmpty() ? workspace() : runtimeDirectory;
  }
  // A named directory of worker-private files, such as levels or sounds.
  QString privateDirectory(const QString& name) const;
  QString resolvePath(const QString& path, bool write) const;
  QStringList inputRoots() const;
  void markMutationStarted();
  void resetMutationStarted();
  // Removes the private level and sound files of one document.
  void removePrivateFiles(Document& document) const;
  bool mutationStarted() const;
  // A failure after this mark is kept in the operation receipt as it is.
  void markFailureRecorded();
  bool failureRecorded() const;
  bool helpersContained() const;
  // Probe results cached for the worker's life; created on first use.
  std::shared_ptr<RuntimeState> runtimeState;
  // Set once by the dispatcher: the toolset names of its catalog.
  QStringList toolsets;
  // Set once from the required --grants and --domains launch options; empty
  // lists grant only inspect in the core toolset.
  Authority authority;
  // Every job this process runs, created on first use.
  std::shared_ptr<JobRegistry> jobs;
  // The worker instance that owns this process's jobs and its monotonic start
  // time; a scene host receives both from its worker.
  QString workerInstance;
  qint64 workerEpochMs = 0;
  // Set once from validated startup configuration; empty disables media tools.
  QString mediaToolsDirectory;
  // Wall-clock budget of one render job; past it the job ends as
  // deadline_exceeded with no artifacts.
  int renderDeadlineMs = 600000;
  // Set once from validated startup configuration, keyed by shared root role.
  QMap<QString, SharedRootGrant> sharedRoots;
  // The private per-process runtime directory inside the workspace.
  QString runtimeDirectory;
  // Set by the dispatcher while a write runs: the effect class of its tool
  // (memory, publication or job) and its operation_id; empty outside a write.
  QString writeClass;
  QString writeOperation;
  // The workspace journal of a worker; null in scene hosts, parse helpers and
  // domain tests.
  std::shared_ptr<Journal> journal;
  // The read-only document a scene host process owns; empty in a worker.
  QString sceneHostDocument;
  // The worker's scene host processes, keyed by read-only document.
  std::shared_ptr<SceneHostRegistry> sceneHosts;
  // Whether a test-only file-writing fault is armed at the named point; always
  // false outside test builds.
  bool writeFaultArmed(const char* point) const {
#ifdef BUILD_UI_PORT_TESTS
    // With faultOperation set, a point inside a write fires only in the write
    // with that operation_id.
    return writeFault == point &&
           (faultOperation.isEmpty() || writeOperation.isEmpty() ||
            writeOperation == faultOperation);
#else
    Q_UNUSED(point);
    return false;
#endif
  }
#ifdef BUILD_UI_PORT_TESTS
  // Test-only fault injected into child processes: crash, hang, memory,
  // read-hang, busy-host (a scene host's render keeps its application
  // thread busy for eight seconds after it starts), or slow-scan (a scene
  // check sleeps 25 ms after each drawing or cast level).
  QString childFault;
  // Test-only: the --test-path-swap arguments, handed on to scene hosts.
  QStringList pathSwap;
  // Test-only render fault: hold-launch, hold-dispatch, hold-publication,
  // hold-writers, fail-after-final-frame, encoder-failure, stray-callbacks,
  // gl-context-failure or teardown-hang (worker teardown never finishes).
  QString renderFault;
  // Test-only file-writing fault point (publish, package:level, ...) and its
  // mode: fail makes the step throw its typed error, crash ends the process,
  // hold blocks the application thread for ten seconds there.
  QString writeFault;
  QString writeFaultMode = "fail";
  // Test-only: the operation_id of the one write in which fault points fire.
  QString faultOperation;
#endif

private:
  QString m_workspace;
  QString m_privateRoot;
  QStringList m_inputRoots;
  std::shared_ptr<TProject> m_project;
  std::map<QString, std::unique_ptr<Document>> m_documents;
  bool m_mutationStarted = false;
  bool m_failureRecorded = false;
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
/// Return the typed catalog for the cell range tools of the sheet toolset.
QJsonArray sheetOperations();
QJsonValue executeSheet(Context& context, const QString& method,
                        const QJsonObject& params);
QJsonArray fxOperations();
// The fx-finishing toolset: read-only FX recipes.
QJsonArray fxRecipeOperations();
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
// The id of a render job that has not reached a terminal state, or empty.
QString activeRenderJob(Context& context);
// The render jobs this process retains.
int renderJobCount(Context& context);
// Removes the unfinished job output of workers that exited without finishing
// it. Completed output and output of a live worker stay.
void sweepAbandonedRenders(Context& context);
// Publish and reopen a private snapshot package; `manifestSha256`, when given,
// receives the package manifest's SHA-256.
std::unique_ptr<ToonzScene> createRenderSnapshot(
    Context& context, Document& document, const QString& directory,
    QString* manifestSha256 = nullptr);
void validateAutomationScene(ToonzScene& scene);

#ifdef BUILD_UI_PORT_TESTS
// Test-only: crash, hang or grow memory in a child process (empty: nothing).
void injectChildFault(const QString& fault);
void runAutomationFxTests(Context& context);
void runAutomationFxAuthoringTests(Context& context);
void runAutomationColorCompositeTests(Context& context);
void runAutomationFxFinishingTests(Context& context);
void runAutomationFxPreflightTests(Context& context);
void runAutomationGroupTests(Context& context);
void runAutomationMediaProcessTests(Context& context);
#endif

// Worker dispatch and protocol I/O run on the QApplication thread.
int runWorker(int argc, char* argv[]);

}  // namespace GroundzeroAutomation
