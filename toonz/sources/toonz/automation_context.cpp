#include "automation.h"
#include "automation_files.h"
#include "automation_path.h"

#include "toonz/fxdag.h"
#include "toonz/filepathproperties.h"
#include "toonz/levelset.h"
#include "toonz/tcolumnfx.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"

#include <QDir>
#include <QFileInfo>
#include <QUuid>

#include <cmath>
#include <limits>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxNameBytes = 255;

QString opaqueId(const QString& kind) {
  return kind + "_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool beneath(const QString& path, const QString& root) {
  const QString prefix = root.endsWith('/') ? root : root + '/';
  return path == root || path.startsWith(prefix);
}

bool allowedPath(const QString& absolute, const QString& canonical,
                 const QStringList& roots) {
  for (const QString& root : roots)
    if (beneath(absolute, root) && beneath(canonical, root)) return true;
  return false;
}

QString canonicalDirectory(const QString& path) {
  const QFileInfo info(path);
  if (!info.isAbsolute() || !info.isDir() || info.isSymLink())
    throw Error("invalid_path", "Root must be an existing absolute directory.");
  const QString canonical = info.canonicalFilePath();
  if (canonical.isEmpty())
    throw Error("invalid_path", "Could not resolve the directory.");
  return canonical;
}

QString normalizedWorkerPath(const QString& path) {
  const QString normalized = QDir::fromNativeSeparators(path);
  if (normalized.isEmpty() || normalized.contains(QChar::Null) ||
      normalized.contains('\\'))
    throw Error("invalid_path",
                "Use a nonempty path with forward-slash separators.");
  for (const QString& part : normalized.split('/')) {
    if (part == "..")
      throw Error("invalid_path", "Parent traversal is not allowed.");
    if (part.toUtf8().size() > MaxNameBytes)
      throw Error("invalid_path", "A path component is longer than 255 bytes.");
  }
  return normalized;
}

QString canonicalAncestor(const QString& path) {
  QString ancestor = path;
  while (!QFileInfo::exists(ancestor)) {
    if (QFileInfo(ancestor).isSymLink())
      throw Error("path_denied",
                  "Symbolic links are not allowed in worker paths.");
    const QString parent = QFileInfo(ancestor).absolutePath();
    if (parent == ancestor)
      throw Error("invalid_path", "Could not resolve the path.");
    ancestor = parent;
  }
  const QFileInfo existing(ancestor);
  const QString canonical = existing.canonicalFilePath();
  if (existing.isSymLink())
    throw Error("path_denied",
                "Symbolic links are not allowed in worker paths.");
  if (canonical != ancestor)
    throw Error("path_denied",
                "Name the path exactly as it is stored: no symbolic link, "
                "and no case or Unicode-normalization variant of a name.");
  return canonical;
}

bool isColumnFx(TXsheet& sheet, TFx* value) {
  for (int index = 0; index < sheet.getColumnCount(); ++index) {
    TXshColumn* column = sheet.getColumn(index);
    if (column && column->getFx() == value) return true;
  }
  return false;
}

template <typename T>
QString registerHandle(QHash<QString, T*>& handles, T* value,
                       const QString& kind) {
  if (!value)
    throw Error("invalid_handle", "Cannot register an empty native object.");
  for (auto it = handles.constBegin(); it != handles.constEnd(); ++it)
    if (it.value() == value) return it.key();
  const QString id = opaqueId(kind);
  handles.insert(id, value);
  return id;
}

template <typename T>
T* findHandle(const QHash<QString, T*>& handles, const QString& id) {
  auto found = handles.constFind(id);
  if (found == handles.constEnd())
    throw Error("invalid_handle",
                "The object handle is unknown or has expired.");
  return found.value();
}

void createDirectory(const QString& path) {
  if (!QDir().mkpath(path))
    throw Error("io_error", "Could not create a private worker directory.");
}

}  // namespace

Document::Document(std::unique_ptr<ToonzScene> ownedScene)
    : id(opaqueId("document")), scene(std::move(ownedScene)) {
  initializeAutomationPaths(*this);
}

Document::Document(std::unique_ptr<ToonzScene> ownedScene, QString documentId,
                   std::shared_ptr<ReadOnlyScene> source)
    : id(std::move(documentId))
    , scene(std::move(ownedScene))
    , readOnly(std::move(source)) {}

Document::~Document() = default;

QString Document::revisionText() const { return QString::number(revision); }

QString Document::idForLevel(TXshLevel* value) {
  return registerHandle(levels, value, "level");
}

QString Document::idForColumn(TXshColumn* value) {
  return registerHandle(columns, value, "column");
}

QString Document::idForFx(TFx* value) {
  return registerHandle(effects, value, "fx");
}

QString Document::idForObject(TStageObject* value) {
  return registerHandle(objects, value, "object");
}

QVector<TXshLevel*> distinctLevels(const TLevelSet* set) {
  QVector<TXshLevel*> levels;
  QSet<TXshLevel*> seen;
  for (int index = 0; index < set->getLevelCount(); ++index) {
    TXshLevel* level = set->getLevel(index);
    if (seen.contains(level)) continue;
    seen.insert(level);
    levels.push_back(level);
  }
  return levels;
}

QJsonObject boundedNameSchema(int maxCharacters, bool noControls) {
  return {
      {"type", "string"},
      {"minLength", 1},
      {"maxLength", maxCharacters},
      {"pattern", noControls ? "^[^\\x00-\\x1f\\x7f-\\x9f]+$" : "^[^\\x00]+$"}};
}

TXshLevel* Document::level(const QString& handle) const {
  TXshLevel* value     = findHandle(levels, handle);
  const TLevelSet* set = scene->getLevelSet();
  for (int index = 0; index < set->getLevelCount(); ++index)
    if (set->getLevel(index) == value) return value;
  throw Error("invalid_handle", "The level is no longer in this document.");
}

TXshColumn* Document::column(const QString& handle) const {
  TXshColumn* value = findHandle(columns, handle);
  TXsheet* sheet    = scene->getXsheet();
  for (int index = 0; index < sheet->getColumnCount(); ++index)
    if (sheet->getColumn(index) == value) return value;
  throw Error("invalid_handle", "The column is no longer in this document.");
}

TFx* Document::fx(const QString& handle) const {
  TFx* value     = findHandle(effects, handle);
  TXsheet* sheet = scene->getXsheet();
  FxDag* dag     = sheet->getFxDag();
  if (dag->getXsheetFx() == value) return value;
  for (int index = 0; index < dag->getOutputFxCount(); ++index)
    if (dag->getOutputFx(index) == value) return value;
  if (isColumnFx(*sheet, value)) return value;
  std::vector<TFx*> current;
  dag->getFxs(current);
  for (TFx* fx : current)
    if (fx == value) return value;
  throw Error("invalid_handle", "The effect is no longer in this document.");
}

TStageObject* Document::object(const QString& handle) const {
  TStageObject* value    = findHandle(objects, handle);
  TStageObjectTree* tree = scene->getXsheet()->getStageObjectTree();
  for (int index = 0; index < tree->getStageObjectCount(); ++index)
    if (tree->getStageObject(index) == value) return value;
  throw Error("invalid_handle",
              "The stage object is no longer in this document.");
}

Context::Context(QString workspacePath, QStringList inputRoots,
                 bool helpersContained, QString privateRoot)
    : m_workspace(canonicalDirectory(workspacePath))
    , m_privateRoot(privateRoot.isEmpty() ? m_workspace
                                          : canonicalDirectory(privateRoot))
    , m_project(std::make_shared<TProject>())
    , m_helpersContained(helpersContained) {
  for (const QString& root : inputRoots)
    m_inputRoots.append(canonicalDirectory(root));
  const QString projectDirectory =
      resolvePath(privateDirectory(opaqueId("project")), true);
  createDirectory(projectDirectory);
  const std::vector<std::string> folders = {
      TProject::Inputs,  TProject::Drawings, TProject::Scenes,
      TProject::Extras,  TProject::Outputs,  TProject::Scripts,
      TProject::Palettes};
  for (const std::string& folder : folders) {
    const QString path =
        QDir(projectDirectory).filePath(QString::fromStdString(folder));
    createDirectory(path);
    m_project->setFolder(folder, TFilePath(path.toStdWString()));
  }
  const TFilePath projectPath(
      QDir(projectDirectory).filePath("automation_otprj.xml").toStdWString());
  FilePathProperties* pathProperties = m_project->getFilePathProperties();
  pathProperties->setUseStandard(false);
  pathProperties->setAcceptNonAlphabetSuffix(false);
  pathProperties->setLetterCountForSuffix(8);
  pathProperties->setNoSeparatorFormatAllowed(false);
  TFilePath::setFilePathProperties(false, false, 8, false);
  if (!m_project->save(projectPath))
    throw Error("io_error", "Could not write the private worker project.");
  TProjectManager::instance()->setCurrentProjectPath(projectPath);
  TProjectManager::instance()->getCurrentProject();
}

Context::~Context() = default;

Document& Context::document(const QString& id) {
  auto found = m_documents.find(id);
  if (found == m_documents.end())
    throw Error("document_not_found",
                "The document handle is unknown or has expired.");
  return *found->second;
}

void Context::requireDocumentCapacity() const {
  if (m_documents.size() >= 32)
    throw Error("resource_limit",
                "Close a document before creating another (limit: 32).");
}

Document& Context::addDocument(std::unique_ptr<ToonzScene> scene) {
  if (!scene)
    throw Error("invalid_document", "Cannot register an empty scene.");
  requireDocumentCapacity();
  scene->setProject(m_project);
  auto document    = std::make_unique<Document>(std::move(scene));
  const QString id = document->id;
  auto inserted    = m_documents.emplace(id, std::move(document));
  return *inserted.first->second;
}

Document& Context::addReadOnlyDocument(std::unique_ptr<ToonzScene> scene,
                                       const QString& id,
                                       std::shared_ptr<ReadOnlyScene> source) {
  if (!scene || !source)
    throw Error("invalid_document", "Cannot register an empty scene.");
  if (m_documents.size() >= 32)
    throw Error("resource_limit",
                "Close a document before creating another (limit: 32).",
                {{"limit", "documents"}});
  if (m_documents.count(id))
    throw Error("internal_error", "The read-only document id is in use.");
  auto document =
      std::make_unique<Document>(std::move(scene), id, std::move(source));
  auto inserted = m_documents.emplace(id, std::move(document));
  return *inserted.first->second;
}

// Level and sound files the worker wrote for a document are named by a
// UUID inside its private directories; they end with the document.
void Context::removePrivateFiles(Document& document) const {
  TLevelSet* cast = document.scene->getLevelSet();
  for (int index = 0; index < cast->getLevelCount(); ++index) {
    const QFileInfo file(QString::fromStdWString(
        cast->getLevel(index)->getPath().getWideString()));
    const QString name = file.fileName();
    for (const QString& kind : {QString("levels"), QString("sounds")}) {
      if (name.size() < 36 || file.absolutePath() != privateDirectory(kind))
        continue;
      const QDir directory(file.absolutePath());
      for (const QString& entry :
           directory.entryList({name.left(36) + "*"}, QDir::Files))
        QFile::remove(directory.filePath(entry));
    }
  }
}

void Context::removeDocument(const QString& id) {
  const auto found = m_documents.find(id);
  if (found == m_documents.end())
    throw Error("document_not_found",
                "The document handle is unknown or has expired.");
  if (!found->second->readOnly) removePrivateFiles(*found->second);
  m_documents.erase(found);
}

QJsonArray Context::documents() const {
  QJsonArray result;
  for (const auto& entry : m_documents)
    result.append(QJsonObject{
        {"document_id", entry.first},
        {"revision", entry.second->revisionText()},
        {"access", entry.second->readOnly ? "read_only" : "editable"}});
  return result;
}

std::shared_ptr<TProject> Context::project() const { return m_project; }
QString Context::workspace() const { return m_workspace; }
QString Context::privateDirectory(const QString& name) const {
  return QDir(m_privateRoot).filePath(name);
}
QStringList Context::inputRoots() const { return m_inputRoots; }
void Context::markMutationStarted() { m_mutationStarted = true; }
void Context::resetMutationStarted() {
  m_mutationStarted = false;
  m_failureRecorded = false;
}
bool Context::mutationStarted() const { return m_mutationStarted; }
void Context::markFailureRecorded() { m_failureRecorded = true; }
bool Context::failureRecorded() const { return m_failureRecorded; }
bool Context::helpersContained() const { return m_helpersContained; }

QString Context::resolvePath(const QString& path, bool write) const {
  const QString normalized = normalizedWorkerPath(path);
  const QString absolute =
      QDir::cleanPath(QDir(m_workspace).absoluteFilePath(normalized));
  QStringList roots{m_workspace};
  if (!write) roots.append(m_inputRoots);
  if (!allowedPath(absolute, absolute, roots))
    throw Error("path_denied", "The path is outside the allowed roots.");
  const QString canonical = canonicalAncestor(absolute);
  if (!allowedPath(absolute, canonical, roots))
    throw Error("path_denied", "The path is outside the allowed roots.");
#ifdef BUILD_UI_PORT_TESTS
  firePathSwap("validated", absolute);
#endif
  return absolute;
}

QJsonObject operation(const QString& name, const QString& description,
                      QJsonObject properties, QJsonArray required,
                      bool writes) {
  if (writes) {
    properties.insert("operation_id",
                      QJsonObject{{"type", "string"}, {"maxLength", 128}});
    if (!required.contains("operation_id")) required.append("operation_id");
    if (properties.contains("document_id")) {
      properties.insert("expected_revision", QJsonObject{{"type", "string"}});
      if (!required.contains("expected_revision"))
        required.append("expected_revision");
    }
  }
  return {{"name", name},
          {"description", description},
          {"inputSchema", QJsonObject{{"type", "object"},
                                      {"properties", properties},
                                      {"required", required},
                                      {"additionalProperties", false}}},
          {"annotations", QJsonObject{{"readOnlyHint", !writes},
                                      {"destructiveHint", writes},
                                      {"idempotentHint", true}}}};
}

QString requiredString(const QJsonObject& params, const QString& name) {
  const QJsonValue value = params.value(name);
  if (!value.isString() || value.toString().isEmpty())
    throw Error("invalid_params", "Expected a nonempty string: " + name);
  return value.toString();
}

int integerParam(const QJsonObject& params, const QString& name, int minimum,
                 int maximum, int fallback) {
  const QJsonValue value = params.value(name);
  if (value.isUndefined()) return fallback;
  const double number =
      value.toDouble(std::numeric_limits<double>::quiet_NaN());
  if (!std::isfinite(number) || std::floor(number) != number ||
      number < minimum || number > maximum)
    throw Error("invalid_params",
                "Expected an integer in the supported range: " + name);
  return static_cast<int>(number);
}

}  // namespace GroundzeroAutomation
