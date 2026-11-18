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
// Pinned regular files up to this size are content-digested at open while
// the scene's digested total stays within the budget; the rest are checked
// by metadata only (probe P-9).
constexpr qint64 PinDigestFileBytes  = 64LL * 1024 * 1024;
constexpr qint64 PinDigestTotalBytes = 512LL * 1024 * 1024;
constexpr int MetadataOnlyPathLimit  = 64;

QString readOnlyProfileName();
QString linkedSnapshotInputs();

// "sha256:" + hex digest of the exact bytes.
QString fileRevision(const QByteArray& bytes);
// "project_" + the first 32 hex digits of SHA-256 over the canonical path.
QString projectIdForFile(const QString& canonicalProjectFile);

// One pinned source file: its stat and, within the digest bounds, a SHA-256
// of its content.
struct SourcePin final {
  QString path;
  qint64 size    = -1;
  qint64 mtimeNs = 0;
  quint64 fileId = 0;
  bool exists    = false;
  bool regular   = false;
  // The stat failed for a reason other than a missing file.
  bool unreadable = false;
  // Empty: the file is checked by metadata only.
  QByteArray digest;

  bool operator==(const SourcePin& other) const;
  bool operator!=(const SourcePin& other) const { return !(*this == other); }
};

// Stat a file without reading it; a missing file gives exists == false.
SourcePin statSourcePin(const QString& path);
// SHA-256 of a regular file read whole at the stated size; empty when the
// file cannot be read whole at that size.
QByteArray contentDigest(const QString& path, qint64 size);
// The file as it is now, digested when the pin was and the stat still matches.
SourcePin currentSourcePin(const SourcePin& pinned);
struct ReadOnlyScene;
// Pin a file once; throws scene_limit past PinnedFileLimit pins.
void addSourcePin(ReadOnlyScene& source, const QString& path);
// "source:" + SHA-256 over the revisions and the sorted pins.
QString sourceFingerprint(const QStringList& revisions,
                          const std::vector<SourcePin>& pins);
// The pinned paths whose current stat or digest differs, in pin order.
QStringList changedSourcePins(const std::vector<SourcePin>& pins);
// Throws source_changed when any pin differs from the file on disk.
void requireUnchangedSources(const std::vector<SourcePin>& pins);
// scene.source_status of a read-only document in its host.
QJsonObject sourceStatus(Document& document);
// Throws source_changed when a file of that level changed since open.
void requireUnchangedLevel(Document& document, TXshLevel* level);

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
  // The level's own files: pins[firstPin, endPin).
  int firstPin = 0;
  int endPin   = 0;
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
  qint64 digestedBytes = 0;
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
// The distinct levels a column exposes, in row order; none for sound and
// generator columns.
std::vector<TXshLevel*> columnLevels(TXshColumn* column);
// The child sheet an instance path names from the top sheet: each step's
// column exposes that step's child level. Throws invalid_handle otherwise.
TXsheet* resolveSheetPath(Document& document, const QJsonArray& path);
// The input schema of an instance path.
QJsonObject sheetPathSchema();
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

// scene.inventory on either kind of document.
QJsonArray sceneInventoryOperations();
QJsonValue executeSceneInventory(Context& context, const QString& method,
                                 const QJsonObject& params);

// Parse-helper side of a project file: name, version, path rules, folders
// as coded and decoded against the project folder, and scene defaults.
QJsonObject nativeProjectSummary(const QString& projectFile);
// project.inspect with a project_id this worker issued.
QJsonValue inspectGrantedProject(Context& context, const QString& projectId);

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
