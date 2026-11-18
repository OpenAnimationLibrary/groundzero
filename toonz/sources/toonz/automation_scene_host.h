#pragma once

#include "automation.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

class TFx;
class TProject;
class TXsheet;
class TXshColumn;
class TXshLevel;

namespace GroundzeroAutomation {

// Read-only scene limits (contract W1 scene access, section 3.4).
constexpr int ReadOnlyDocumentLimit     = 4;
constexpr qint64 SceneHostMemoryBytes   = 2048LL * 1024 * 1024;
constexpr int SceneHostOpenDeadlineMs   = 60000;
constexpr int SceneHostReadDeadlineMs   = 30000;
constexpr qint64 SceneFileLimitBytes    = 64LL * 1024 * 1024;
constexpr qint64 ProjectFileLimitBytes  = 1024LL * 1024;
constexpr qint64 ScenesXmlLimitBytes    = 64LL * 1024;
constexpr int SheetDepthLimit           = 16;
constexpr int SheetLimit                = 1024;
constexpr int LevelLimit                = 4096;
constexpr int DrawingLimit              = 1000000;
constexpr int FxNodesPerSheetLimit      = 4096;
constexpr int FxNodesPerSceneLimit      = 16384;
constexpr int StageObjectsPerSheetLimit = 16384;
constexpr int PinnedFileLimit           = 65536;
constexpr int FirstFrameAxisLimit       = 16384;
constexpr qint64 FirstFramePixelLimit   = 268435456;
constexpr int UnsupportedRowLimit       = 256;
constexpr int BlockerRowLimit           = 64;
constexpr int ChangedPathLimit          = 64;

QString readOnlyProfileName();
QString linkedSnapshotInputs();

// "sha256:" + hex digest of the exact bytes.
QString fileRevision(const QByteArray& bytes);
// "project_" + the first 32 hex digits of SHA-256 over the canonical path.
QString projectIdForFile(const QString& canonicalProjectFile);

// One stat-pinned source file.
struct SourcePin final {
  QString path;
  qint64 size    = -1;
  qint64 mtimeNs = 0;
  quint64 fileId = 0;
  bool exists    = false;

  bool operator==(const SourcePin& other) const;
  bool operator!=(const SourcePin& other) const { return !(*this == other); }
};

// Stat a file without reading it; a missing file gives exists == false.
SourcePin statSourcePin(const QString& path);
struct ReadOnlyScene;
// Pin a file once; throws scene_limit past PinnedFileLimit pins.
void addSourcePin(ReadOnlyScene& source, const QString& path);
// "source:" + SHA-256 over the revisions and the sorted pins.
QString sourceFingerprint(const QStringList& revisions,
                          const std::vector<SourcePin>& pins);
// The pinned paths whose current stat differs, in pin order.
QStringList changedSourcePins(const std::vector<SourcePin>& pins);
// Throws source_changed when any pin differs from the file on disk.
void requireUnchangedSources(const std::vector<SourcePin>& pins);

// What a read-only document reports and blocks (section 5).
struct ContentRow final {
  enum class Rule { Anywhere, Rendered, Never, Camera };
  QString kind;
  QString reason;
  QString name;
  QString detail;
  QString levelId;
  QString columnId;
  QString fxId;
  QJsonArray sheetPath;
  Rule rule          = Rule::Rendered;
  TXshLevel* level   = nullptr;
  TXshColumn* column = nullptr;
  TFx* fx            = nullptr;

  QJsonObject unsupportedRow() const;
  QJsonObject blockerRow() const;
};

// Per-level outcome of the host's own loader.
struct LevelState final {
  QString kind;
  QString loadStatus;
  int bitDepth = 0;
};

// The host's record of a read-only document: its source, project and loader
// results. The worker's placeholder document carries an empty one.
struct ReadOnlyScene final {
  QString projectId;
  QString scenePath;
  QString sceneCopy;
  QString sceneRevision;
  QString fingerprint;
  QStringList revisions;
  QStringList readRoots;
  QString workerWorkspace;
  std::shared_ptr<TProject> project;
  std::vector<SourcePin> pins;
  QSet<QString> pinnedPaths;
  std::vector<ContentRow> rows;
  QHash<TXshLevel*, LevelState> levels;

  QJsonObject summarySource() const;
};

// Section 5.3 render admission: the blockers of the document's current output.
std::vector<ContentRow> renderBlockers(Document& document);
QJsonObject renderReport(Document& document);
// Per-effect admission object of fx.graph for either kind of document.
QJsonObject fxAdmission(Document& document, TFx* fx);
// True when a read-only document may describe the node in full.
bool fxAdmitted(TFx* fx);
// The section 6.4 scene.validate issue rows of a read-only document.
QJsonArray readOnlyIssues(Document& document);
// Classify every level, column and effect of a loaded user scene.
void classifyReadOnlyContent(Document& document);
// Section 3.4 structure bounds: sheets, depth, effects and stage objects.
// Throws scene_limit, or invalid_scene for a cyclic sub-xsheet.
void requireReadOnlyStructure(ToonzScene& scene);
// The open report's counts except drawings.
QJsonObject readOnlyCounts(Document& document);

// Host side: load a scene copy with the host's own loader (section 3.2).
struct HostLoadRequest final {
  QString scenePath;
  QString sceneCopy;
  QString sceneRevision;
  QString projectId;
  QString projectFile;
  QString projectRevision;
  QString scenesXml;
  QString scenesXmlRevision;
  QString binding;
  QString workerWorkspace;
  QStringList readRoots;
};
QJsonObject loadReadOnlyDocument(Context& context, const QString& documentId,
                                 const HostLoadRequest& request);
// A fresh render snapshot of a read-only document, loaded the same way.
std::unique_ptr<ToonzScene> loadReadOnlySnapshot(Context& context,
                                                 Document& document);
// Internal host methods; never in the public catalog.
QJsonValue executeSceneHost(Context& context, const QString& method,
                            const QJsonObject& params);
// Ends the host process when its memory passes the host bound.
void startSceneHostWatchdog();

// Worker side.
QJsonArray sceneAccessOperations();
QJsonValue executeSceneAccess(Context& context, const QString& method,
                              const QJsonObject& params);
// Route a request that addresses a read-only document, or one of its jobs or
// artifacts, to its host. Returns false when the request is the worker's own.
// Refuse an unqualified tool on a read-only document before its arguments
// are validated or a receipt is taken.
void refuseReadOnlyEarly(Context& context, const QString& method,
                         const QJsonObject& params,
                         const QJsonObject& descriptor);
bool routeToSceneHost(Context& context, const QString& method,
                      const QJsonObject& params, const QJsonObject& descriptor,
                      QJsonValue& result);
QJsonObject readOnlyCapabilities(Context& context);
int readOnlyDocumentCount(Context& context);
// Context::documents() with each row's host_state (null when editable).
QJsonArray documentRows(Context& context);
void closeSceneHosts(Context& context);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationSceneHostTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
